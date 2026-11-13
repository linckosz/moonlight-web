"""A share's guest link and PIN go from the owner's page to the guest's kiosk, and nowhere else.

    python scripts/bench/test_guest_link.py
"""
import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "shared-feed"))
import guest_link as gl  # noqa: E402
import drive  # noqa: E402

TOKEN = "FAKETOKENabcdef0123456789"
PIN = "48151623"
LINK = "https://192.168.1.10:8443/p/" + TOKEN


class Page:
    """An owner's or a guest's page: answers eval with what it is given."""

    def __init__(self, answer=None, fail=None):
        self.answer, self.fail, self.calls = answer, fail, []
        self.c = self

    def eval(self, js):
        if self.fail:
            raise self.fail
        return self.answer

    def call(self, method, **params):
        self.calls.append((method, params))
        if self.fail:
            raise self.fail
        return {}


class Activate(unittest.TestCase):
    def test_keeps_link_and_pin_in_memory(self):
        share = gl.activate(Page('{"status": 200, "url": "%s", "pin": "%s", "app_id": 7}' % (LINK, PIN)), 2)
        self.assertEqual((share.token, share.pin), (TOKEN, PIN))
        self.assertEqual(share.info, {"status": 200, "app_id": 7, "host_uuid": None, "state": None})
        self.assertNotIn(TOKEN, repr(share.info))

    def test_failure_says_status_and_two_booleans_only(self):
        with self.assertRaises(drive.PassFailed) as e:
            gl.activate(Page('{"status": 200, "url": "%s", "pin": ""}' % LINK), 3)
        msg = str(e.exception)
        self.assertEqual(msg, "slot 3 did not open: status 200, link True, PIN False")
        self.assertNotIn(TOKEN, msg)

    def test_page_error_is_not_repeated(self):
        with self.assertRaises(drive.PassFailed) as e:
            gl.activate(Page(fail=RuntimeError({"message": "boom " + PIN})), 2)
        self.assertNotIn(PIN, str(e.exception))
        self.assertIsNone(e.exception.__cause__)


class Go(unittest.TestCase):
    def run_go(self, stdout):
        seen = {}

        def fake_run(argv, **kw):
            seen["argv"], seen["input"] = argv, kw.get("input")
            return mock.Mock(stdout=stdout)
        share = gl.Share(2)
        share.token = TOKEN
        with mock.patch.object(gl.subprocess, "run", fake_run), mock.patch.dict(os.environ, {}, clear=False):
            env_before = dict(os.environ)
            try:
                gl.go(9342, "https://192.168.1.10:8443/", share)
                err = None
            except drive.PassFailed as e:
                err = e
            self.assertEqual(env_before, dict(os.environ))
        return seen, err

    def test_link_by_the_pipe_only(self):
        seen, err = self.run_go("navigated\n")
        self.assertIsNone(err)
        self.assertIn("nav-stdin", seen["argv"])
        self.assertFalse(any(TOKEN in str(x) for x in seen["argv"]))
        self.assertIn(TOKEN, seen["input"])

    def test_failure_without_the_link(self):
        _seen, err = self.run_go("not navigated\n")
        self.assertIsNotNone(err)
        self.assertNotIn(TOKEN, str(err))


class EnterPin(unittest.TestCase):
    def test_pin_in_one_call_and_never_in_an_error(self):
        page = Page(answer=1)
        share = gl.Share(2)
        share.pin = PIN
        gl.enter_pin(page, share)
        self.assertEqual(page.calls, [("Input.insertText", {"text": PIN})])

        class Failing(Page):
            def call(self, method, **params):
                raise RuntimeError("Input.insertText failed for " + params.get("text", ""))
        with self.assertRaises(drive.PassFailed) as e:
            gl.enter_pin(Failing(answer=1), share)
        self.assertNotIn(PIN, str(e.exception))
        self.assertIsNone(e.exception.__cause__)


class CloseKiosk(unittest.TestCase):
    def test_profile_removed(self):
        bench = tempfile.mkdtemp(prefix="mwguest-")
        try:
            os.makedirs(os.path.join(bench, ".chrome-guest-2", "Default"))
            with mock.patch.object(gl, "BENCH", bench), \
                 mock.patch.object(gl.subprocess, "run", return_value=mock.Mock(stdout="")) as run:
                self.assertEqual(gl.close_kiosk(".chrome-guest-2"), "")
            self.assertFalse(os.path.exists(os.path.join(bench, ".chrome-guest-2")))
            # The profile is matched at the end of the path: not .chrome-guest-23.
            self.assertIn("chrome\\-guest\\-2(\\s|\"|$)", run.call_args[0][0][-1])
        finally:
            shutil.rmtree(bench, ignore_errors=True)


class Proof(unittest.TestCase):
    def test_a_known_token_alone_counts_once(self):
        with mock.patch.object(gl, "_tokens", [TOKEN]):
            self.assertEqual(gl._count("x %s y %s" % (LINK, TOKEN)), (2, 0))

    def test_counts_only(self):
        results = tempfile.mkdtemp(prefix="mwproof-")
        try:
            with open(os.path.join(results, "r.json"), "w") as f:
                f.write('{"url": "%s", "pin": "%s", "fps": 60}' % (LINK, PIN))
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                links, pins = gl.proof(results, 0, dev_logs=False)
            self.assertEqual((links, pins), (1, 1))
            self.assertNotIn(TOKEN, out.getvalue())
            self.assertNotIn(PIN, out.getvalue())
        finally:
            shutil.rmtree(results, ignore_errors=True)

    def test_stderr_counted_too(self):
        results = tempfile.mkdtemp(prefix="mwproof-")
        try:
            err = gl.Tee(io.StringIO())
            err.write("Traceback ... %s\n" % LINK)
            with mock.patch.object(sys, "stderr", err), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gl.proof(results, 0, dev_logs=False)[0], 1)
        finally:
            shutil.rmtree(results, ignore_errors=True)

    def test_clean_results_count_zero(self):
        results = tempfile.mkdtemp(prefix="mwproof-")
        try:
            with open(os.path.join(results, "r.json"), "w") as f:
                f.write('{"fps": 60, "latencyUs": 48151623}')
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(gl.proof(results, 0, dev_logs=False), (0, 0))
        finally:
            shutil.rmtree(results, ignore_errors=True)


if __name__ == "__main__":
    unittest.main()
