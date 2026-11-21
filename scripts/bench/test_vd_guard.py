"""What vd_guard stops on, and what it lets be. No screen, window or process is touched.

    python scripts/bench/test_vd_guard.py
"""
import io
import os
import shutil
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from unittest import mock

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vd_guard as g  # noqa: E402

D1 = {"dev": r"\\.\DISPLAY1", "x": 0, "y": 0, "w": 2560, "h": 1440, "primary": True}
D5 = {"dev": r"\\.\DISPLAY5", "x": -2560, "y": 0, "w": 2560, "h": 1440, "primary": False}
VD = {"dev": r"\\.\DISPLAY134", "x": 2560, "y": 0, "w": 1440, "h": 1080, "primary": False}


def win(cls, exe, pid, rect):
    return {"class": cls, "exe": exe, "pid": pid, "rect": rect}


ON_VD = [2600, 40, 3600, 900]
ON_D1 = [100, 100, 900, 700]


class Rules(unittest.TestCase):
    def test_the_new_screen_is_the_one_that_was_not_there(self):
        self.assertEqual(g.new_screen([D1, D5], [D1, D5, VD]), VD)
        self.assertIsNone(g.new_screen([D1, D5], [D1, D5]))

    def test_a_new_screen_counts_even_when_it_arrives_primary(self):
        vd_primary = dict(VD, x=0, primary=True)
        now = [dict(D1, x=-1440, primary=False), D5, vd_primary]
        self.assertEqual(g.new_screen([D1, D5], now), vd_primary)

    def test_a_window_counts_only_on_the_virtual_screen(self):
        wins = [win("CASCADIA_HOSTING_WINDOW_CLASS", "WindowsTerminal.exe", 50, ON_D1)]
        self.assertEqual(g.foreign(wins, VD, set(), set()), [])

    def test_a_window_straddling_the_edge_counts(self):
        wins = [win("Chrome_WidgetWin_1", "chrome.exe", 60, [2400, 0, 2700, 500])]
        self.assertEqual(len(g.foreign(wins, VD, set(), set())), 1)

    def test_touching_the_edge_only_does_not_count(self):
        wins = [win("Chrome_WidgetWin_1", "chrome.exe", 60, [2000, 0, 2560, 500])]
        self.assertEqual(g.foreign(wins, VD, set(), set()), [])

    def test_ours_the_desktop_and_the_allowed_pass(self):
        wins = [win("T48 anim", "powershell.exe", 7, ON_VD),
                win("WorkerW", "explorer.exe", 8, ON_VD),
                win("CEF-OSC-WIDGET", "NVIDIA Overlay.exe", 9, ON_VD)]
        self.assertEqual(g.foreign(wins, VD, {7}, {("CEF-OSC-WIDGET", "NVIDIA Overlay.exe")}), [])

    def test_an_unknown_program_stays_foreign(self):
        self.assertEqual(len(g.foreign([win("X", "?", 4, ON_VD)], VD, set(), set())), 1)

    def test_a_foreign_window_is_named_by_class_and_program_never_title(self):
        hit = g.foreign([dict(win("X", "WindowsTerminal.exe", 50, ON_VD), title="secret")], VD, set(), set())
        self.assertEqual(hit, [{"class": "X", "exe": "WindowsTerminal.exe", "pid": 50}])


class OwnFile(unittest.TestCase):
    def setUp(self):
        self.root = tempfile.mkdtemp(prefix="vdguard-root-")
        self.other = tempfile.mkdtemp(prefix="vdguard-other-")

    def tearDown(self):
        shutil.rmtree(self.root, ignore_errors=True)
        shutil.rmtree(self.other, ignore_errors=True)

    def test_a_file_in_the_scratch_area_is_taken(self):
        self.assertTrue(g.own_file_ok(os.path.join(self.root, "s", "own.txt"), root=self.root))

    def test_a_file_elsewhere_is_refused(self):
        self.assertFalse(g.own_file_ok(os.path.join(self.other, "own.txt"), root=self.root))

    def test_a_sibling_with_the_same_prefix_is_refused(self):
        self.assertFalse(g.own_file_ok(self.root + "-x" + os.sep + "own.txt", root=self.root))

    def test_climbing_out_is_refused(self):
        self.assertFalse(g.own_file_ok(os.path.join(self.root, "..", "own.txt"), root=self.root))


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class Run(unittest.TestCase):
    def guard(self, screens, windows, stops, streaming=lambda: True):
        clock = FakeClock()
        seq = iter(screens)
        last = [None]

        def monitors():
            nxt = next(seq, last[0])
            if isinstance(nxt, Exception):
                raise nxt
            last[0] = nxt
            return nxt

        def stop():
            stops.append(clock())
            return "quit"
        return g.Guard(monitors, lambda: windows(clock()), stop, streaming=streaming, poll=0.05,
                       clock=clock, sleep=clock.sleep)

    def run_quiet(self, guard, **kw):
        out = io.StringIO()
        with redirect_stdout(out):
            code = guard.run(kw.get("own", set()), set(), kw.get("secs", 10), kw.get("appear", 5),
                             own_file=kw.get("own_file"))
        return code, out.getvalue()

    def test_a_window_already_there_at_the_first_poll_stops_at_once(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5], [D1, D5, VD]],
                        lambda t: [win("CASCADIA", "WindowsTerminal.exe", 50, ON_VD)], stops)
        code, out = self.run_quiet(gd)
        self.assertEqual((code, len(stops)), (4, 1))
        self.assertIn('"ms_after_screen": 0', out)

    def test_a_window_arriving_later_stops_within_one_poll(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5, VD]],
                        lambda t: [win("C", "chrome.exe", 60, ON_VD)] if t >= 1.0 else [], stops)
        code, _ = self.run_quiet(gd)
        self.assertEqual(code, 4)
        self.assertLess(stops[0] - 1.0, 0.05 + 1e-9)

    def test_a_screen_arriving_primary_is_watched(self):
        stops = []
        vd = dict(VD, x=0, primary=True)
        d1 = dict(D1, x=-1440, primary=False)
        gd = self.guard([[D1, D5], [d1, D5, vd]], lambda t: [win("C", "chrome.exe", 60, [10, 10, 800, 600])], stops)
        code, out = self.run_quiet(gd)
        self.assertEqual((code, len(stops)), (4, 1))
        self.assertIn('"primary": true', out)

    def test_a_screen_that_moves_is_watched_where_it_is_now(self):
        stops = []
        moved = dict(VD, x=0, primary=True)
        gd = self.guard([[D1, D5], [D1, D5, VD], [dict(D1, x=-1440, primary=False), D5, moved]],
                        lambda t: [win("C", "chrome.exe", 60, [10, 10, 800, 600])], stops)
        code, _ = self.run_quiet(gd)
        self.assertEqual((code, len(stops)), (4, 1))

    def test_no_new_screen_stops_the_dev(self):
        stops = []
        gd = self.guard([[D1, D5]], lambda t: [], stops)
        code, out = self.run_quiet(gd, appear=1)
        self.assertEqual((code, len(stops)), (2, 1))
        self.assertIn("no_screen", out)

    def test_the_time_running_out_stops_the_dev(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5, VD]], lambda t: [], stops)
        code, out = self.run_quiet(gd, secs=2)
        self.assertEqual((code, len(stops)), (0, 1))
        self.assertIn("time_up", out)

    def test_the_screen_going_away_with_nobody_streaming_ends_quietly(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5, VD], [D1, D5, VD], [D1, D5]], lambda t: [], stops,
                        streaming=lambda: False)
        code, out = self.run_quiet(gd)
        self.assertEqual((code, stops), (0, []))
        self.assertIn("screen_gone", out)

    def test_a_stream_that_outlives_its_screen_is_stopped(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5, VD], [D1, D5]], lambda t: [], stops, streaming=lambda: True)
        code, out = self.run_quiet(gd)
        self.assertEqual((code, len(stops)), (3, 1))
        self.assertIn("screen_gone_still_streaming", out)

    def test_an_error_once_armed_stops_the_dev(self):
        stops = []

        def windows(t):
            raise OSError("enum failed")
        gd = self.guard([[D1, D5], [D1, D5, VD]], windows, stops)
        code, out = self.run_quiet(gd)
        self.assertEqual((code, len(stops)), (3, 1))
        self.assertIn('"error": "OSError"', out)

    def test_an_error_before_armed_says_so_and_never_arms(self):
        stops = []
        gd = self.guard([OSError("no screens")], lambda t: [], stops)
        code, out = self.run_quiet(gd)
        self.assertEqual((code, stops), (2, []))
        self.assertNotIn("armed", out)

    def test_a_pid_written_to_the_file_later_is_ours(self):
        stops = []
        fd, path = tempfile.mkstemp(prefix="vdguard-", suffix=".txt")
        os.close(fd)
        try:
            def windows(t):
                if t >= 0.5:  # the bench names its window's pid as it opens it
                    with open(path, "w", encoding="utf-8") as f:
                        f.write("77\n")
                return [win("T48 anim", "powershell.exe", 77, ON_VD)] if t >= 0.5 else []
            gd = self.guard([[D1, D5], [D1, D5, VD], [D1, D5, VD]] + [[D1, D5, VD]] * 50 + [[D1, D5]],
                            windows, stops, streaming=lambda: False)
            code, _ = self.run_quiet(gd, secs=10, own_file=path)
            self.assertEqual((code, stops), (0, []))
        finally:
            os.remove(path)

    def test_an_unreadable_pid_file_adds_no_one(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5, VD]], lambda t: [win("T48 anim", "powershell.exe", 77, ON_VD)], stops)
        code, _ = self.run_quiet(gd, own_file=os.path.join(os.path.dirname(__file__), "no-such-own-file.txt"))
        self.assertEqual(code, 4)


class FakeTarget:
    def __init__(self, alive_for, same=True):
        self.calls = 0
        self.alive_for = alive_for  # how many alive() answers are True
        self.same = same
        self.killed = False

    def alive(self):
        self.calls += 1
        return self.calls <= self.alive_for

    def kill_tree(self):
        if not self.same:
            return False
        self.killed = True
        return True


class Stopper(unittest.TestCase):
    def stop(self, target):
        with mock.patch.object(g.mw_local, "local_request", return_value=(200, b"{}")) as req:
            how = g.stopper("dev.exe", True, target, grace=0.3)()
        return how, req

    def test_a_dev_already_gone_is_not_asked(self):
        how, req = self.stop(FakeTarget(alive_for=0))
        self.assertEqual(how, "gone")
        req.assert_not_called()

    def test_a_dev_that_quits_is_not_killed(self):
        t = FakeTarget(alive_for=2)
        how, _ = self.stop(t)
        self.assertEqual((how, t.killed), ("quit", False))

    def test_a_dev_still_there_after_the_grace_is_killed(self):
        t = FakeTarget(alive_for=10 ** 6)
        how, _ = self.stop(t)
        self.assertEqual((how, t.killed), ("killed", True))

    def test_a_process_no_longer_the_dev_is_not_killed(self):
        t = FakeTarget(alive_for=10 ** 6, same=False)
        how, _ = self.stop(t)
        self.assertEqual((how, t.killed), ("not killed: no longer the --dev", False))


if __name__ == "__main__":
    unittest.main()
