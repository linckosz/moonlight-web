"""python -m unittest test_drive  (no machine, no browser)

What drive.py writes into the page before a launch, against a stand-in for
cdp.py: the real one needs a Chrome on its debugging port.
"""
import sys
import types
import unittest
from unittest import mock


class FakeCdp:
    def __init__(self, port):
        self.evals = []
        self.calls = []

    def eval(self, js):
        self.evals.append(js)
        return '{"x": 10, "y": 20}' if "getBoundingClientRect" in js else "streaming"

    def call(self, method, **params):
        self.calls.append(method)
        return {}


with mock.patch.dict(sys.modules, {"cdp": types.SimpleNamespace(Cdp=FakeCdp)}):
    import drive


class AutoLearn(unittest.TestCase):
    def setUp(self):
        patch = mock.patch.object(drive.time, "sleep", lambda secs: None)
        patch.start()
        self.addCleanup(patch.stop)

    def written(self, d):
        return [js for js in d.c.evals if "mw_auto_learn" in js]

    def test_every_pass_holds_auto_at_its_base(self):
        d = drive.Driver()
        d.apply_settings({"resolution": "auto"})
        self.assertEqual(self.written(d), ["localStorage.setItem('mw_auto_learn', \"0\")"])

    def test_a_launch_without_settings_holds_it_too(self):
        d = drive.Driver()
        d.launch({"uuid": "u"}, {"appId": "1000", "name": "Virtual Display"})
        self.assertEqual(self.written(d), ["localStorage.setItem('mw_auto_learn', \"0\")"])
        self.assertIn("Input.dispatchMouseEvent", d.c.calls)

    def test_the_ceiling_s_bench_says_otherwise(self):
        d = drive.Driver()
        d.auto_learn = "1"
        d.launch({"uuid": "u"}, {"appId": "1000", "name": "Virtual Display"})
        self.assertEqual(self.written(d), ["localStorage.setItem('mw_auto_learn', \"1\")"])

    def test_none_leaves_the_page_s_default(self):
        d = drive.Driver(auto_learn=None)
        d.apply_settings({})
        d.launch({"uuid": "u"}, {"appId": "1000", "name": "Virtual Display"})
        self.assertEqual(self.written(d), [])


if __name__ == "__main__":
    unittest.main()
