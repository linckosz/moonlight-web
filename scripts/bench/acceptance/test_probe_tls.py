"""python -m unittest test_probe_tls  (no machine, no instance asked)

The Windows probe's HTTPS check (fleet._TLS_PS). Its certificate callback is
compiled C#, given to its own request only: a name that differs from
127.0.0.1 is the one error it accepts, any other refuses the port and is named.
The callback it replaced was a PowerShell script block on the process-wide
ServicePointManager, which ran on a thread with no runspace: every HTTPS check
failed before it was sent, and the probe said httpsPort 0.
"""
import datetime
import os
import shutil
import socket
import ssl
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fleet  # noqa: E402


def run_ps(script):
    fd, path = tempfile.mkstemp(suffix=".ps1")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write(script)
        return subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", path],
                              capture_output=True, text=True, timeout=120)
    finally:
        os.remove(path)


@unittest.skipUnless(shutil.which("powershell"), "the Windows probe needs Windows PowerShell")
class Callback(unittest.TestCase):
    def test_only_a_name_mismatch_is_accepted(self):
        p = run_ps(fleet._TLS_PS + r"""
$E = [System.Net.Security.SslPolicyErrors]
foreach ($e in @($E::None, $E::RemoteCertificateNameMismatch, $E::RemoteCertificateChainErrors,
                 $E::RemoteCertificateNotAvailable,
                 ($E::RemoteCertificateNameMismatch -bor $E::RemoteCertificateChainErrors))) {
    'CHECK ' + [int]$e + ' ' + [MwProbeTls]::Check.Invoke($null, $null, $null, $e)
}
'REFUSED ' + [MwProbeTls]::Refused
""")
        lines = [x for x in p.stdout.splitlines() if x.startswith(("CHECK ", "REFUSED "))]
        self.assertEqual(lines, ["CHECK 0 True", "CHECK 2 True", "CHECK 4 False", "CHECK 1 False",
                                 "CHECK 6 False",
                                 "REFUSED RemoteCertificateNameMismatch, RemoteCertificateChainErrors"],
                         p.stderr)


def self_signed(directory):
    """A certificate no store trusts, for a name that is not 127.0.0.1."""
    from cryptography import x509
    from cryptography.hazmat.primitives import hashes, serialization
    from cryptography.hazmat.primitives.asymmetric import rsa
    from cryptography.x509.oid import NameOID
    key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    name = x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, "mw-probe-test.invalid")])
    now = datetime.datetime.now(datetime.timezone.utc)
    cert = (x509.CertificateBuilder().subject_name(name).issuer_name(name).public_key(key.public_key())
            .serial_number(x509.random_serial_number()).not_valid_before(now - datetime.timedelta(minutes=5))
            .not_valid_after(now + datetime.timedelta(hours=1)).sign(key, hashes.SHA256()))
    cert_path = os.path.join(directory, "c.pem")
    key_path = os.path.join(directory, "k.pem")
    with open(cert_path, "wb") as f:
        f.write(cert.public_bytes(serialization.Encoding.PEM))
    with open(key_path, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.TraditionalOpenSSL,
                                  serialization.NoEncryption()))
    return cert_path, key_path


@unittest.skipUnless(shutil.which("powershell"), "the Windows probe needs Windows PowerShell")
class Live(unittest.TestCase):
    """A TLS listener on 127.0.0.1 whose certificate no store trusts: refused, and named."""

    def test_untrusted_chain_refused_and_named(self):
        try:
            import cryptography  # noqa: F401
        except ImportError:
            self.skipTest("needs the cryptography package to make a certificate")
        tmp = tempfile.mkdtemp()
        try:
            cert, key = self_signed(tmp)
            ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
            ctx.load_cert_chain(cert, key)
            srv = socket.socket()
            srv.bind(("127.0.0.1", 0))
            srv.listen(4)
            port = srv.getsockname()[1]
            asked = []

            def serve():
                for _ in range(2):
                    try:
                        conn, _a = srv.accept()
                    except OSError:
                        return
                    try:
                        with ctx.wrap_socket(conn, server_side=True) as s:
                            asked.append(s.recv(4096))
                            s.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{}")
                    except (ssl.SSLError, OSError):
                        pass
            t = threading.Thread(target=serve, daemon=True)
            t.start()
            p = run_ps(fleet._TLS_PS + "\n'GOT ' + [bool](Get-MwHttpsStatus %d)\n'REFUSED ' + [MwProbeTls]::Refused\n" % port)
            srv.close()
            t.join(5)
        finally:
            shutil.rmtree(tmp, ignore_errors=True)
        lines = [x for x in p.stdout.splitlines() if x.startswith(("GOT ", "REFUSED "))]
        self.assertEqual(lines[0], "GOT False", p.stderr)
        self.assertIn("RemoteCertificateChainErrors", lines[1])
        # Refused during the handshake: no request was ever sent.
        self.assertEqual(asked, [])


class Script(unittest.TestCase):
    def test_the_probe_carries_it_and_no_global_callback(self):
        seen = {}

        def fake_run(mid, script, timeout=600, shell=None, direct=False):
            seen["script"] = script
            return 0, "", ""
        with mock.patch.dict(fleet.MACHINES, {"w1": {"os": "windows", "kind": "ssh", "address": "10.0.0.9"}}), \
             mock.patch.object(fleet, "run_script", fake_run):
            fleet.probe("w1")
        s = seen["script"]
        self.assertNotIn("__TLS_PS__", s)
        self.assertIn(fleet._TLS_PS.strip(), s)
        self.assertNotIn("ServerCertificateValidationCallback = {", s)
        self.assertNotIn("ServicePointManager]::ServerCertificateValidationCallback", s)
        self.assertIn("if ($scheme -eq 'https') { return Get-MwHttpsStatus $port }", s)

    def test_only_this_machine_is_asked(self):
        self.assertIn('"https://127.0.0.1:$port/api/native/status"', fleet._TLS_PS)
        for header in ("Cookie", "Authorization", "X-MW-Admin-Key", "Headers"):
            self.assertNotIn(header, fleet._TLS_PS)


if __name__ == "__main__":
    unittest.main()
