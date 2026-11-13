"""What mw_local starts, and what it never does.

    python scripts/bench/test_mw_local.py
"""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mw_local  # noqa: E402


class KnowsLocalRequest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="mwlocal-")
        mw_local._KNOWN.clear()

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def exe(self, name, content):
        path = os.path.join(self.dir, name)
        with open(path, "wb") as f:
            f.write(content)
        return path

    def test_the_server_names(self):
        for name in ("MoonlightWeb.exe", "moonlightweb.exe", "MoonlightWebDev.exe", "MoonlightWeb",
                     "moonlight-web", "moonlightweb", "moonlightweb-dev", "MoonlightWebDev"):
            self.assertTrue(mw_local.is_server_exe(os.path.join("x", name)), name)
        for name in ("python.exe", "node.exe", "MoonlightWeb.exe.bak", "notMoonlightWeb.exe", "", None):
            self.assertFalse(mw_local.is_server_exe(name), name)

    def test_ascii_and_utf16_found(self):
        self.assertTrue(mw_local.knows_local_request(self.exe("MoonlightWeb.exe", b"\0" * 64 + b"local-request" + b"\0")))
        mw_local._KNOWN.clear()
        wide = "local-request".encode("utf-16-le")
        self.assertTrue(mw_local.knows_local_request(self.exe("moonlightweb", b"x" * 99 + wide)))

    def test_absent_is_absent(self):
        self.assertFalse(mw_local.knows_local_request(self.exe("MoonlightWeb.exe", b"print-link wait-link" * 100)))
        self.assertFalse(mw_local.knows_local_request(os.path.join(self.dir, "MoonlightWeb.exe.none")))

    def test_across_the_boundary_of_two_reads(self):
        # Reads go by 1 MiB: the name cut in two by that boundary is found all the same.
        wide = "local-request".encode("utf-16-le")
        cut = (1 << 20) - 7
        self.assertTrue(mw_local.knows_local_request(self.exe("MoonlightWeb.exe", b"\0" * cut + wide + b"\0" * 64)))
        mw_local._KNOWN.clear()
        self.assertTrue(mw_local.knows_local_request(
            self.exe("moonlight-web", b"\0" * ((1 << 20) - 5) + b"local-request")))

    def test_another_program_is_never_taken_for_the_server(self):
        # A program that listens on the port and happens to hold the string.
        self.assertFalse(mw_local.knows_local_request(self.exe("other.exe", b"local-request")))

    def test_an_exe_without_it_is_never_started(self):
        path = self.exe("MoonlightWeb.exe", b"nothing here")
        with self.assertRaises(mw_local.LocalRequestError):
            mw_local.local_request("GET /api/health", exe=path, dev=False)


if __name__ == "__main__":
    unittest.main()
