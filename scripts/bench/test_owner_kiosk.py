"""The owner's link goes from the exe to the kiosk through a pipe, and nowhere else.

    python scripts/bench/test_owner_kiosk.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import owner_kiosk  # noqa: E402

FAKE_KEY = "FAKEKEY0123456789abcdef"
FAKE_LINK = "https://localhost:8443/#k=" + FAKE_KEY


class Pipe(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="mwowner-")
        # The instance's exe, as far as this test goes: prints a log line, then the link.
        self.exe = os.path.join(self.dir, "maker.cmd" if sys.platform == "win32" else "maker.sh")
        with open(self.exe, "w", newline="") as f:
            if sys.platform == "win32":
                f.write("@echo off\r\necho.\r\necho %s\r\n" % FAKE_LINK.replace("%", "%%"))
            else:
                f.write("#!/bin/sh\necho\necho '%s'\n" % FAKE_LINK)
        os.chmod(self.exe, 0o755)
        # cdp.py, as far as this test goes: what it was given, and how.
        self.report = os.path.join(self.dir, "report.txt")
        self.cdp = os.path.join(self.dir, "fake_cdp.py")
        with open(self.cdp, "w") as f:
            f.write(
                "import sys\n"
                "data = sys.stdin.read()\n"
                "argv = ' '.join(sys.argv)\n"
                "open(%r, 'w').write('stdin_has_link=%%s argv_has_key=%%s origin=%%s' %% "
                "(%r in data, %r in argv, sys.argv[-1] if sys.argv[-1].startswith('https://') else ''))\n"
                "print('navigated')\n" % (self.report, FAKE_LINK, FAKE_KEY))
        self.argvs = []
        real = subprocess.Popen

        def spy(argv, *a, **k):
            self.argvs.append(list(argv))
            return real(argv, *a, **k)

        self.patch = mock.patch.object(owner_kiosk.subprocess, "Popen", spy)
        self.patch.start()
        # The stand-in exe is a script, not named as the server's: its name is let through here alone.
        self.named = mock.patch.object(owner_kiosk.mw_local, "is_server_exe", return_value=True)
        self.named.start()

    def tearDown(self):
        self.named.stop()
        self.patch.stop()
        shutil.rmtree(self.dir, ignore_errors=True)

    def test_the_link_reaches_the_navigator_by_its_input_alone(self):
        went = owner_kiosk.open_owner(8080, 9333, origin="https://127.0.0.1:8443", cdp=self.cdp,
                                      _instance_for_tests=(self.exe, True))
        self.assertTrue(went)
        report = open(self.report).read()
        self.assertIn("stdin_has_link=True", report)
        self.assertIn("argv_has_key=False", report)
        self.assertIn("origin=https://127.0.0.1:8443", report)
        # No process this module started had the key on its command line.
        self.assertEqual(len(self.argvs), 2)
        for argv in self.argvs:
            self.assertFalse(any(FAKE_KEY in str(x) or "k=" in str(x) for x in argv), argv)
        self.assertIn("--print-link", self.argvs[0])
        self.assertIn("--dev", self.argvs[0])
        self.assertIn("nav-stdin", self.argvs[1])

    def test_without_dev_and_not_the_dev_edition_nothing_starts(self):
        with self.assertRaises(owner_kiosk.Refused):
            owner_kiosk.open_owner(8080, 9333, cdp=self.cdp, _instance_for_tests=(self.exe, False))
        self.assertEqual(self.argvs, [])

    def test_a_given_exe_is_named_as_the_server_or_refused(self):
        self.named.stop()
        try:
            with self.assertRaises(owner_kiosk.Refused):
                owner_kiosk.open_owner(8080, 9333, cdp=self.cdp, _instance_for_tests=(self.exe, True))
        finally:
            self.named.start()
        self.assertEqual(self.argvs, [])


class Production(unittest.TestCase):
    def test_its_ports_and_its_folder(self):
        prog = os.environ.get("ProgramFiles", r"C:\Program Files")
        self.assertTrue(owner_kiosk.is_prod(os.path.join(prog, "MoonlightWeb", "MoonlightWeb.exe"), 8080))
        self.assertTrue(owner_kiosk.is_prod(r"E:\Code\moonlight-web\build\MoonlightWeb.exe", 48080))
        self.assertTrue(owner_kiosk.is_prod(r"E:\Code\moonlight-web\build\MoonlightWeb.exe", 48443))
        self.assertFalse(owner_kiosk.is_prod(r"E:\Code\moonlight-web\build\MoonlightWeb.exe", 8080))

    def test_no_link_is_made_for_it(self):
        prog = os.environ.get("ProgramFiles", r"C:\Program Files")
        prod = os.path.join(prog, "MoonlightWeb", "MoonlightWeb.exe")
        with mock.patch.object(owner_kiosk.mw_local, "instance", return_value=(prod, False)):
            with self.assertRaises(owner_kiosk.Refused):
                owner_kiosk.bench_instance(8080)
        with mock.patch.object(owner_kiosk.subprocess, "Popen") as popen:
            with self.assertRaises(owner_kiosk.Refused):
                owner_kiosk.open_owner(48080, 9333, _instance_for_tests=(r"E:\x\MoonlightWeb.exe", True))
            with self.assertRaises(owner_kiosk.Refused):
                owner_kiosk.open_owner(8080, 9333, _instance_for_tests=(prod, True))
            popen.assert_not_called()

    def test_the_positive_rule(self):
        # A MoonlightWeb.exe without --dev is production wherever it lies, whatever its port.
        knows = mock.patch.object(owner_kiosk.mw_local, "knows", return_value=True)
        with knows:
            with mock.patch.object(owner_kiosk.mw_local, "instance", return_value=(r"E:\x\MoonlightWeb.exe", False)):
                with self.assertRaises(owner_kiosk.Refused):
                    owner_kiosk.bench_instance(8080)
            with mock.patch.object(owner_kiosk.mw_local, "instance", return_value=(r"E:\x\MoonlightWeb.exe", True)):
                self.assertEqual(owner_kiosk.bench_instance(8080), (r"E:\x\MoonlightWeb.exe", True))
            with mock.patch.object(owner_kiosk.mw_local, "instance", return_value=(r"E:\x\MoonlightWebDev.exe", False)):
                self.assertEqual(owner_kiosk.bench_instance(8080), (r"E:\x\MoonlightWebDev.exe", False))

    def test_an_exe_without_the_command_or_another_name(self):
        with mock.patch.object(owner_kiosk.mw_local, "instance", return_value=(r"E:\x\python.exe", True)):
            with self.assertRaises(owner_kiosk.Refused):
                owner_kiosk.bench_instance(8080)
        with mock.patch.object(owner_kiosk.mw_local, "instance", return_value=(r"E:\x\MoonlightWeb.exe", True)):
            with mock.patch.object(owner_kiosk.mw_local, "knows", return_value=False):
                with self.assertRaises(owner_kiosk.Refused):
                    owner_kiosk.bench_instance(8080)


class Profile(unittest.TestCase):
    def test_only_its_own_folder(self):
        removed, why = owner_kiosk.remove_profile(tempfile.gettempdir())
        self.assertFalse(removed)
        self.assertEqual(why, "not the owner kiosk's profile")

    def test_closed_when_it_cannot_tell(self):
        d = tempfile.mkdtemp(prefix="mwowner-profile-")
        try:
            with mock.patch.object(owner_kiosk, "PROFILE", d):
                for answer, why in ((None, "cannot tell whether a Chrome runs on it"),
                                    (1, "a Chrome still runs on it")):
                    with mock.patch.object(owner_kiosk, "chromes_on_profile", return_value=answer):
                        self.assertEqual(owner_kiosk.remove_profile(d), (False, why))
                    self.assertTrue(os.path.isdir(d))
                with mock.patch.object(owner_kiosk, "chromes_on_profile", return_value=0):
                    self.assertEqual(owner_kiosk.remove_profile(d), (True, "removed"))
                self.assertFalse(os.path.isdir(d))
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_an_empty_count_is_no_answer(self):
        empty = mock.Mock(stdout=b"")
        with mock.patch.object(owner_kiosk.subprocess, "run", return_value=empty):
            self.assertIsNone(owner_kiosk.chromes_on_profile())


if __name__ == "__main__":
    unittest.main()
