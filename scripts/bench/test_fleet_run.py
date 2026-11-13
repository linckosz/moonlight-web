"""fleet-run.py hands the PIN to run-fleet.ps1 on its standard input, and nowhere else.

    python scripts/bench/test_fleet_run.py
"""
import contextlib
import importlib.util
import io
import os
import sys
import unittest
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("fleet_run", os.path.join(HERE, "fleet-run.py"))
fleet_run = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fleet_run)

PIN = "48151623"


class Launch(unittest.TestCase):
    def launch(self, argv, pin=PIN):
        seen = {}

        def fake_run(cmd, **kw):
            seen["cmd"], seen["kw"] = cmd, kw
            return mock.Mock(returncode=0)
        out = io.StringIO()
        env_before = dict(os.environ)
        with mock.patch.dict(fleet_run.fleet.MACHINES, {"m1": {}}), \
             mock.patch.object(fleet_run.fleet, "probe", return_value={}), \
             mock.patch.object(fleet_run.fleet, "lan_url", return_value="https://10.0.0.8:49443/"), \
             mock.patch.object(fleet_run.fleet, "mint_pin", return_value=pin), \
             mock.patch.object(fleet_run.subprocess, "run", fake_run), \
             contextlib.redirect_stdout(out):
            code = fleet_run.main(argv)
        self.assertEqual(env_before, dict(os.environ))
        return code, seen, out.getvalue()

    def test_pin_on_stdin_only(self):
        code, seen, out = self.launch(["--machine", "m1", "--", "-Label", "x"])
        self.assertEqual(code, 0)
        self.assertEqual(seen["kw"]["input"], PIN + "\n")
        self.assertFalse(any(PIN in str(a) for a in seen["cmd"]))
        self.assertNotIn("env", seen["kw"])
        self.assertNotIn(PIN, out)
        self.assertIn("run-fleet.ps1", seen["cmd"][-5])

    def test_address_added_when_not_given(self):
        _code, seen, _out = self.launch(["--machine", "m1", "--", "-Label", "x"])
        self.assertEqual(seen["cmd"][-2:], ["-AppUrl", "https://10.0.0.8:49443/"])
        _code, seen, _out = self.launch(["--machine", "m1", "--", "-Label", "x", "-AppUrl", "https://h/"])
        self.assertEqual(seen["cmd"].count("-AppUrl"), 1)

    def test_no_pin_no_run(self):
        code, seen, out = self.launch(["--machine", "m1", "--", "-Label", "x"], pin="")
        self.assertEqual(code, 1)
        self.assertNotIn("cmd", seen)
        self.assertIn("False", out)

    def test_unknown_machine(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(fleet_run.main(["--machine", "nowhere-xyz"]), 2)


if __name__ == "__main__":
    unittest.main()
