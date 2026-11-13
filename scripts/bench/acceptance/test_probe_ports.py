"""python -m unittest test_probe_ports  (no machine, no instance asked)

Which ports the probes ask. With MW_BENCH_LOCAL_PORTS set, the Windows probe
and the Python probe ask ONLY those ports, never a guessed one: a guessed port
may be another instance's (an installed edition, the prod). Without it, they
ask the process's own ports, then the defaults.
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fleet  # noqa: E402

GUESSED = [48080, 48443, 18080, 18443, 80, 443]


def env_with(ports):
    env = {k: v for k, v in os.environ.items() if k != "MW_BENCH_LOCAL_PORTS"}
    if ports is not None:
        env["MW_BENCH_LOCAL_PORTS"] = ports
    return env


@unittest.skipUnless(shutil.which("powershell"), "the Windows probe needs Windows PowerShell")
class WindowsProbe(unittest.TestCase):
    def ports(self, env_ports):
        # The process's own port is 9999, as Get-NetTCPConnection would say.
        script = ("$ids = @(4242)\n"
                  "function Get-NetTCPConnection { [pscustomobject]@{ OwningProcess = 4242; LocalPort = 9999 } }\n"
                  + fleet._PORTS_PS + "\n'PORTS ' + ($ports -join ',')\n")
        fd, path = tempfile.mkstemp(suffix=".ps1")
        try:
            with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
                f.write(script)
            p = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", path],
                               capture_output=True, text=True, timeout=60, env=env_with(env_ports))
        finally:
            os.remove(path)
        line = [x for x in p.stdout.splitlines() if x.startswith("PORTS ")]
        self.assertEqual(len(line), 1, p.stderr)
        return [int(x) for x in line[0][6:].split(",") if x]

    def test_given_ports_only(self):
        self.assertEqual(self.ports("8080,8443"), [8080, 8443])
        self.assertEqual(self.ports(" 18080 , 18443 ,"), [18080, 18443])

    def test_without_them_discovered_then_guessed(self):
        self.assertEqual(self.ports(None), [9999] + GUESSED)

    def test_the_probe_script_carries_it(self):
        seen = {}

        def fake_run(mid, script, timeout=600, shell=None, direct=False):
            seen["script"] = script
            return 0, "", ""
        with mock.patch.dict(fleet.MACHINES, {"w1": {"os": "windows", "kind": "ssh", "address": "10.0.0.9"}}), \
             mock.patch.object(fleet, "run_script", fake_run):
            fleet.probe("w1")
        self.assertNotIn("__PORTS_PS__", seen["script"])
        self.assertIn(fleet._PORTS_PS.strip(), seen["script"])


class PythonProbe(unittest.TestCase):
    def ports(self, env_ports, discovered=(9999,)):
        ns = {"listening_ports": lambda app: list(discovered)}
        exec(fleet._PORTS_PY, ns)
        with mock.patch.dict(os.environ, env_with(env_ports), clear=True):
            return ns["probe_ports"]("MoonlightWebDev")

    def test_given_ports_only(self):
        self.assertEqual(self.ports("8080,8443"), [8080, 8443])

    def test_without_them_discovered_then_guessed(self):
        self.assertEqual(self.ports(None), [9999] + GUESSED)
        self.assertEqual(self.ports(""), [9999] + GUESSED)

    def test_the_probe_body_carries_it(self):
        body = fleet._probe_body_py()
        self.assertNotIn("__PORTS_PY__", body)
        self.assertIn("candidates = probe_ports(APP)", body)
        compile(body, "probe", "exec")

    def test_prod_edition_still_rewrites_the_guesses(self):
        with mock.patch.object(fleet, "EDITION", "prod"):
            body = fleet._for_edition(fleet._probe_body_py(), "linux")
        self.assertIn("for guess in (49080, 49443, 80, 443):", body)


class LanUrl(unittest.TestCase):
    def url(self, mid, kind, env_ports, probe):
        seen = {}

        def fake_from_here(address, candidates, timeout=4):
            seen["candidates"] = list(candidates)
            return 0
        with mock.patch.dict(fleet.MACHINES, {mid: {"kind": kind, "address": "127.0.0.1"}}), \
             mock.patch.object(fleet, "https_port_from_here", fake_from_here), \
             mock.patch.dict(os.environ, env_with(env_ports), clear=True):
            return fleet.lan_url(mid, probe), seen.get("candidates")

    def test_local_with_given_ports_never_guesses(self):
        url, cands = self.url("local", "local", "8080,8443",
                              {"httpPort": 8080, "httpsPort": 0, "listening": [8080, 8443]})
        self.assertEqual(cands, [8443])
        self.assertEqual(url, "https://127.0.0.1:8443/")

    def test_remote_unchanged(self):
        url, cands = self.url("n1", "ssh", "8080,8443",
                              {"httpPort": 48080, "httpsPort": 0, "listening": [48080, 48443]})
        self.assertEqual(cands, [48443, 18443, 443])
        self.assertEqual(url, "https://127.0.0.1:%d/" % fleet.DEV_HTTPS)


if __name__ == "__main__":
    unittest.main()
