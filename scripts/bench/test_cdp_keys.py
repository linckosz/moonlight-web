"""cdp.py keys types a key only while the kiosk has the foreground, asked again before each one.

    python scripts/bench/test_cdp_keys.py
"""
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import cdp  # noqa: E402


class FakeCdp:
    """The page, as far as these tests go: it records the keys it was given."""
    typed = []
    calls = []

    def __init__(self, port, reply_timeout=None):
        FakeCdp.typed = []
        FakeCdp.calls = []

    def call(self, method, **params):
        FakeCdp.calls.append(method)
        return {}

    def eval(self, js):
        return "CANVAS"

    def type_key(self, code, key, vk, shift=False):
        FakeCdp.typed.append(code)


def run_keys(argv, answers):
    """main() on a table of three keys; @p answers are what the foreground check says, in turn."""
    plan = {"gapMs": 0, "keys": [{"code": c, "key": k, "vk": 0} for c, k in
                                 (("Digit1", "&"), ("KeyQ", "a"), ("Enter", "Enter"))]}
    with tempfile.NamedTemporaryFile("w", suffix=".json", delete=False, encoding="utf-8") as f:
        json.dump(plan, f)
    out = io.StringIO()
    code = 0
    try:
        with mock.patch.object(cdp, "Cdp", FakeCdp), \
             mock.patch.object(cdp, "foreground_problem", side_effect=list(answers)), \
             mock.patch.object(sys, "argv", ["cdp.py"] + argv + ["keys", f.name]), \
             contextlib.redirect_stdout(out):
            try:
                cdp.main()
            except SystemExit as e:
                code = e.code
    finally:
        os.unlink(f.name)
    return code, out.getvalue(), FakeCdp.typed


class PerKey(unittest.TestCase):
    GUARD = ["--kiosk-pid", "1234", "--kiosk-display", r"\\.\DISPLAY5"]

    def test_all_keys_while_the_kiosk_has_it(self):
        code, _, typed = run_keys(self.GUARD, ["", "", ""])
        self.assertEqual(code, 0)
        self.assertEqual(typed, ["Digit1", "KeyQ", "Enter"])

    def test_stops_before_the_first_key_that_would_leave(self):
        bad = "the foreground is another process (class X, pid 9)"
        code, out, typed = run_keys(self.GUARD, ["", bad])
        self.assertEqual(code, 3)
        self.assertEqual(typed, ["Digit1"])  # KeyQ and Enter never sent
        line = [json.loads(s) for s in out.splitlines() if s.startswith('{"aborted"')]
        self.assertEqual(line, [{"aborted": bad, "before": "KeyQ"}])

    def test_no_key_at_all_when_the_first_check_fails(self):
        code, _, typed = run_keys(self.GUARD, ["the foreground is another screen (class Y, pid 1234)"])
        self.assertEqual(code, 3)
        self.assertEqual(typed, [])

    def test_never_brings_the_kiosk_to_the_front(self):
        # Page.bringToFront asks Chrome to activate its window: the foreground is never taken.
        run_keys(self.GUARD, ["", "", ""])
        self.assertNotIn("Page.bringToFront", FakeCdp.calls)

    def test_refused_without_the_kiosk(self):
        for argv in ([], ["--kiosk-pid", "1234"], ["--kiosk-display", r"\\.\DISPLAY5"]):
            code, _, typed = run_keys(argv, [])
            self.assertIsInstance(code, str)
            self.assertIn("--kiosk-pid and --kiosk-display", code)
            self.assertEqual(typed, [])


@unittest.skipUnless(sys.platform == "win32", "the foreground is a Windows question")
class RealForeground(unittest.TestCase):
    """The real check, against whatever window has the foreground while the test runs."""

    def now(self):
        import ctypes
        from ctypes import wintypes
        user32 = ctypes.WinDLL("user32")
        user32.GetForegroundWindow.restype = ctypes.c_void_p
        user32.GetWindowThreadProcessId.argtypes = [ctypes.c_void_p, ctypes.POINTER(wintypes.DWORD)]
        hwnd = user32.GetForegroundWindow()
        if not hwnd:
            self.skipTest("no foreground window")
        pid = wintypes.DWORD(0)
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        return pid.value

    def test_another_process(self):
        pid = self.now()
        self.assertIn("another process", cdp.foreground_problem(pid + 4, r"\\.\DISPLAY1"))

    def test_never_a_title(self):
        # Only the rule, the class and the process id: "class ..., pid N" and nothing else.
        msg = cdp.foreground_problem(self.now() + 4, r"\\.\DISPLAY1")
        self.assertRegex(msg, r"^the foreground is [a-z ]+ \(class [^,()]*, pid \d+\)$")


if __name__ == "__main__":
    unittest.main()
