"""What vd_guard stops on, and what it lets be. No screen, window or process is touched.

    python scripts/bench/test_vd_guard.py
"""
import io
import os
import sys
import unittest
from contextlib import redirect_stdout

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

    def test_a_new_primary_screen_is_not_taken_for_the_virtual_one(self):
        self.assertIsNone(g.new_screen([D5], [D5, dict(D1, dev=r"\\.\DISPLAY9")]))

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

    def test_a_foreign_window_is_named_by_class_and_program_never_title(self):
        hit = g.foreign([dict(win("X", "WindowsTerminal.exe", 50, ON_VD), title="secret")], VD, set(), set())
        self.assertEqual(hit, [{"class": "X", "exe": "WindowsTerminal.exe", "pid": 50}])


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


class Run(unittest.TestCase):
    def guard(self, screens, windows, stops):
        clock = FakeClock()
        seq = iter(screens)
        last = [None]

        def monitors():
            last[0] = next(seq, last[0])
            return last[0]

        def stop():
            stops.append(clock())
            return "quit"
        return g.Guard(monitors, lambda: windows(clock()), stop, poll=0.05, clock=clock, sleep=clock.sleep)

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
        self.assertEqual(code, 4)
        self.assertEqual(len(stops), 1)
        self.assertIn('"ms_after_screen": 0', out)
        self.assertNotIn("secret", out)

    def test_a_window_arriving_later_stops_within_one_poll(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5, VD]],
                        lambda t: [win("C", "chrome.exe", 60, ON_VD)] if t >= 1.0 else [], stops)
        code, _ = self.run_quiet(gd)
        self.assertEqual(code, 4)
        self.assertLess(stops[0] - 1.0, 0.05 + 1e-9)

    def test_nothing_foreign_runs_out_the_time(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5, VD]], lambda t: [], stops)
        code, out = self.run_quiet(gd, secs=2)
        self.assertEqual((code, stops), (0, []))
        self.assertIn("time_up", out)

    def test_the_screen_going_away_ends_the_watch(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5, VD], [D1, D5, VD], [D1, D5]], lambda t: [], stops)
        code, out = self.run_quiet(gd)
        self.assertEqual((code, stops), (0, []))
        self.assertIn("screen_gone", out)

    def test_no_new_screen_gives_up_without_stopping(self):
        stops = []
        gd = self.guard([[D1, D5]], lambda t: [], stops)
        code, _ = self.run_quiet(gd, appear=1)
        self.assertEqual((code, stops), (2, []))

    def test_a_pid_written_to_the_file_later_is_ours(self):
        import tempfile
        stops = []
        fd, path = tempfile.mkstemp(prefix="vdguard-", suffix=".txt")
        os.close(fd)
        try:
            def windows(t):
                if t >= 0.5:  # the bench names its window's pid as it opens it
                    with open(path, "w", encoding="utf-8") as f:
                        f.write("77\n")
                return [win("T48 anim", "powershell.exe", 77, ON_VD)] if t >= 0.5 else []
            gd = self.guard([[D1, D5], [D1, D5, VD]], windows, stops)
            code, _ = self.run_quiet(gd, secs=2, own_file=path)
            self.assertEqual((code, stops), (0, []))
        finally:
            os.remove(path)

    def test_an_unreadable_pid_file_adds_no_one(self):
        stops = []
        gd = self.guard([[D1, D5], [D1, D5, VD]], lambda t: [win("T48 anim", "powershell.exe", 77, ON_VD)], stops)
        code, _ = self.run_quiet(gd, own_file=os.path.join(os.path.dirname(__file__), "no-such-own-file.txt"))
        self.assertEqual(code, 4)


if __name__ == "__main__":
    unittest.main()
