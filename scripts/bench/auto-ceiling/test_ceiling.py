"""python -m unittest test_ceiling  (no machine, no browser)

ceiling.py against stand-ins for the browser and the bench's own modules: a pass
only runs on a bench slot, and that is a poor place to find out about a typo.
"""
import argparse
import json
import os
import shutil
import sys
import tempfile
import time
import types
import unittest
from unittest import mock

import ceiling
import journal

UUID = "00000000-0000-4000-8000-000000000000"
P = journal.PREFIX
BASE = [["\\\\.\\DISPLAY5", "-2560,0", "2560x1440"], ["\\\\.\\DISPLAY1", "0,0", "2560x1440", "primary"]]
VIRTUAL = ["\\\\.\\DISPLAY77", "0,0", "3840x2160", "primary"]


class FakeDriver:
    """A page whose console says what a test wants, one script per launch."""

    def __init__(self, scripts, stats_end=None):
        self.scripts = list(scripts)
        self.asked = [{"contentType": 'video/mp4; codecs="hev1.1.6.L153.90"', "answer": {"smooth": True}}]
        self.visited = []
        self.store = {}
        self.lines = []
        self.current = []
        self.evals = []
        self.launches = 0
        self.stopped = 0
        self.stats_end = stats_end
        self.stats_calls = 0

    def navigate(self, url):
        self.visited.append(url)

    def apply_settings(self, settings):
        self.settings = settings

    def call(self, method, **params):
        return {"windowId": 1}

    def eval(self, js):
        self.evals.append(js)
        if js.startswith("localStorage.removeItem"):
            name = js[js.index("(") + 1:js.rindex(")")].strip("'\"")
            self.store.pop(name, None)
        elif js.startswith("localStorage.setItem"):
            inner = js[js.index("(") + 1:js.rindex(")")].replace("'", '"', 2)
            k, v = json.loads("[" + inner + "]")
            self.store[k] = v
        elif "length = 0" in js:
            self.lines = []
        return ""

    def json_eval(self, js):
        if "__mwDecodingAsked" in js:
            return list(self.asked)
        if "__mwCeilingLog" in js:
            # A line more each time it is read, as a stream would say them.
            if self.current:
                self.lines.append(self.current.pop(0))
            return list(self.lines)
        if "Object.fromEntries" in js:
            return {k: self.store.get(k) for k in json.loads(js[js.index("["):js.index("]") + 1])}
        if "Screen.prototype" in js:
            return {"width": 3072, "height": 1728, "dpr": 1.25}
        raise AssertionError(js)

    def pick_tile(self, target, index=None):
        return ({"uuid": UUID, "name": "host"},
                {"name": "Virtual Display" if target == "vdisplay" else "Display %d" % (index or 0)})

    def launch(self, card, app):
        self.launches += 1
        self.lines = []
        self.current = list(self.scripts.pop(0))

    def wait_picture(self, timeout=45):
        return True

    def stats(self):
        self.stats_calls += 1
        if self.stats_end and self.stats_calls >= self.stats_end:
            return {"rows": {}, "canvas": None, "visibility": "visible"}
        return {"rows": {"Resolution:": "3840x2160", "Framerate:": "120 fps",
                         "Auto:": "2160p120, learned"},
                "canvas": {"w": 1, "h": 1}, "visibility": "visible"}

    def screenshot(self, path):
        return None

    def stop(self):
        self.stopped += 1


class Rig(unittest.TestCase):
    def setUp(self):
        self.out = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.out, True)
        self.clock = [1000.0]
        self.contents = []
        self.closed = []
        self.screens = [list(BASE)]

        def monitors():
            return self.screens[0]

        def content_start(page, probe=True, debug_port=0):
            self.contents.append((page, os.environ.get("MW_BENCH_CONTENT_RECT")))

        run = types.SimpleNamespace(
            DEBUG_PORT=9333, access_map=lambda: {"local": {}},
            kiosk_start=lambda url: None, kiosk_stop=lambda: self.closed.append("kiosk"),
            content_start=content_start, content_stop=lambda: self.closed.append("content"))
        self.line = "https://stream.dev.example/abc123"
        fleet = types.SimpleNamespace(probe=lambda m: {}, lan_url=lambda m, p: "https://10.0.0.2/",
                                      rendezvous_url=lambda p: self.line)
        self.driver = None
        drive = types.SimpleNamespace(Driver=lambda port: self.driver)
        cadence = types.SimpleNamespace(monitors=monitors)
        page = types.SimpleNamespace(library=lambda d, access: True)
        env = {"run": run, "fleet": fleet, "drive": drive, "cadence": cadence, "pass": page}
        patches = [
            mock.patch.dict(sys.modules, env),
            mock.patch.object(ceiling, "OUT", self.out),
            mock.patch.object(ceiling, "nvidia_load", lambda: []),
            mock.patch.object(ceiling.time, "sleep", self.tick),
            mock.patch.object(ceiling.time, "time", lambda: self.clock[0]),
            mock.patch.dict(os.environ, {}, clear=False),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tick(self, secs):
        self.clock[0] += secs

    def args(self, **over):
        base = dict(mode="rung", tag="t", rung=500, keep=False, secs=30, every=2, expect="hold",
                    learn="1", target="vdisplay", tile="", gdi="", screen="", enhance=False,
                    page="p.html",
                    lang="", client_port=0, client_url="", max_launches=7, plan=False,
                    load_gpu="", load_at=30, until_earned=False, autostep="",
                    rendezvous=False, refuse_above=0)
        base.update(over)
        return argparse.Namespace(**base)

    def launch_appears(self):
        """The host makes its virtual display as the launch starts."""
        inner = self.driver.launch

        def launch(card, app):
            self.screens[0] = BASE + [VIRTUAL]
            inner(card, app)

        self.driver.launch = launch
        inner_stop = self.driver.stop

        def stop():
            self.screens[0] = list(BASE)
            inner_stop()

        self.driver.stop = stop


class OneRung(Rig):
    CLEAN = [P + "500 M on hevc, asking 498 M, held back by it",
             P + "500 M held, 300 s loaded",
             P + "700 M earned for the next launch, 300 s loaded",
             P + "stream over, 590 s loaded at 500 M"]

    def test_a_clean_rung_on_the_virtual_display(self):
        self.driver = FakeDriver([self.CLEAN])
        self.launch_appears()
        b = ceiling.Bench(self.args())
        s, uuid, result = b.one_launch("t", 500, False, "hold")
        b.close()
        self.assertEqual(result["defects"], [])
        self.assertEqual(s["rung"], 500)
        self.assertEqual(uuid, UUID)
        self.assertEqual(self.driver.store["mw_auto_learn"], "1")
        # The Driver, which holds Auto at its base for other benches, says the same.
        self.assertEqual(self.driver.auto_learn, "1")
        key = json.loads(self.driver.store[journal.KEY])
        self.assertEqual(key["hosts"][UUID + "|hevc"]["ceiling"], 500000000)
        # The page went on the screen the launch made, and came off.
        self.assertEqual(self.contents, [("p.html", "0,0,3840,2160")])
        self.assertIn("content", self.closed)
        self.assertEqual(self.driver.stopped, 1)
        with open(os.path.join(self.out, "t.json"), encoding="utf-8") as f:
            self.assertEqual(json.load(f)["defects"], [])

    def test_memories_of_a_pass_before_are_cleared_and_said(self):
        self.driver = FakeDriver([self.CLEAN])
        self.driver.store["mw_enhancer_rung"] = '{"x":1}'
        self.driver.store["mw_autostep"] = "0"
        self.launch_appears()
        _, _, result = ceiling.Bench(self.args()).one_launch("t", 500, False, "hold")
        self.assertEqual(result["before"]["mw_enhancer_rung"], '{"x":1}')
        self.assertNotIn("mw_enhancer_rung", self.driver.store)
        self.assertNotIn("mw_autostep", self.driver.store)

    def test_keep_leaves_the_key_alone(self):
        self.driver = FakeDriver([self.CLEAN])
        self.driver.store[journal.KEY] = "theirs"
        self.launch_appears()
        ceiling.Bench(self.args(keep=True, rung=0)).one_launch("t", 0, True, "any")
        self.assertEqual(self.driver.store[journal.KEY], "theirs")

    def test_a_display_tile_goes_by_its_name_and_the_picture_on_its_screen(self):
        self.driver = FakeDriver([self.CLEAN])
        a = self.args(target="display", tile="Display 1", gdi="DISPLAY1")
        ceiling.Bench(a).one_launch("t", 500, False, "any")
        self.assertEqual(self.contents, [("p.html", "0,0,2560,1440")])

    def test_a_display_not_on_the_desktop_stops_the_pass(self):
        self.driver = FakeDriver([self.CLEAN])
        a = self.args(target="display", tile="Display 1", gdi="DISPLAY9")
        with self.assertRaises(SystemExit):
            ceiling.Bench(a).one_launch("t", 500, False, "any")
        self.assertEqual(self.contents, [])

    def test_a_tile_nobody_has_stops_the_pass(self):
        self.driver = FakeDriver([self.CLEAN])
        a = self.args(target="display", tile="Display 42", gdi="DISPLAY1")
        with self.assertRaises(SystemExit):
            ceiling.Bench(a).one_launch("t", 500, False, "any")
        self.assertEqual(self.driver.launches, 0)

    def test_the_fake_screen_is_checked(self):
        self.driver = FakeDriver([self.CLEAN])
        self.launch_appears()
        ceiling.Bench(self.args(screen="3840x2160")).one_launch("t", 500, False, "hold")
        self.driver = FakeDriver([self.CLEAN])
        self.launch_appears()
        with self.assertRaises(SystemExit):
            ceiling.Bench(self.args(screen="2560x1440")).one_launch("t", 500, False, "hold")

    def test_a_stream_that_goes_away_is_a_defect(self):
        self.driver = FakeDriver([[P + "500 M on hevc, asking 498 M, held back by it"]],
                                 stats_end=2)
        self.launch_appears()
        b = ceiling.Bench(self.args())
        _, _, result = b.one_launch("t", 500, False, "any")
        self.assertTrue(result["endedEarly"])
        self.assertIn("the stream ended by itself, and no line says why", result["defects"])
        self.assertEqual(self.driver.stopped, 0)

    def test_the_line_on_the_card_is_required_above_the_base(self):
        self.driver = FakeDriver([self.CLEAN])
        self.launch_appears()
        plain = self.driver.stats
        self.driver.stats = lambda: {**plain(), "rows": {"Resolution:": "x"}}
        _, _, result = ceiling.Bench(self.args()).one_launch("t", 500, False, "hold")
        self.assertIn("above the base budget and the stats card never showed its line",
                      result["defects"])

    def test_the_same_gpu_encoding_and_decoding_is_a_defect(self):
        self.driver = FakeDriver([self.CLEAN])
        self.launch_appears()
        both = [{"bus": "1", "enc": 30, "dec": 40}, {"bus": "3", "enc": 0, "dec": 0}]
        with mock.patch.object(ceiling, "nvidia_load", lambda: both):
            _, _, result = ceiling.Bench(self.args()).one_launch("t", 500, False, "hold")
        self.assertIn("the same GPU encoded and decoded: ['1']", result["defects"])

    def test_a_weak_client_elsewhere_ends_at_its_signal(self):
        self.driver = FakeDriver([[P + "1000 M on hevc, asking 995 M",
                                   P + "sick (decode_rate), 40 s loaded, at 1000 M — down to 700 M",
                                   P + "stream over, 40 s loaded at 1000 M, sick (decode_rate)"]])
        self.launch_appears()
        inner = self.driver.launch

        def launch(card, app):
            inner(card, app)
            # What the page writes as it falls ill: one rung down, the failure counted.
            lowered = journal.prepared_key(UUID, 700, 1)
            lowered["hosts"][UUID + "|hevc"]["fails"] = {"1000000000": {"n": 1, "at": 1}}
            self.driver.store[journal.KEY] = json.dumps(lowered)

        self.driver.launch = launch
        a = self.args(rung=1000, secs=600, expect="sick", client_port=9423,
                      client_url="https://10.0.0.2:8443/")
        started = self.clock[0]
        _, _, result = ceiling.Bench(a).one_launch("weak", 1000, False, "sick")
        self.assertEqual(result["defects"], [])
        self.assertEqual(result["journal"]["sick"]["down"], 700)
        self.assertLess(self.clock[0] - started, 300)
        self.assertEqual(self.driver.stopped, 1)

    def test_a_client_elsewhere_is_never_sent_to_its_own_loopback(self):
        self.driver = FakeDriver([])
        # The fleet knows the desk the bench runs on by its loopback.
        sys.modules["fleet"].lan_url = lambda m, p: "https://127.0.0.1:8443/"
        for url in ("", "https://localhost:8443/", "https://127.0.0.1:8443/"):
            with self.assertRaises(SystemExit) as said:
                ceiling.Bench(self.args(client_port=9423, client_url=url))
            self.assertIn("--client-url", str(said.exception))
        # The host's LAN address is what it takes; a browser on this desk keeps the loopback.
        ceiling.Bench(self.args(client_port=9423, client_url="https://10.0.0.2:8443/"))
        ceiling.Bench(self.args())


class TheGuard(Rig):
    LINES = OneRung.CLEAN

    def test_a_browser_never_asked_is_a_defect(self):
        self.driver = FakeDriver([self.LINES])
        self.driver.asked = []
        self.launch_appears()
        _, _, result = ceiling.Bench(self.args()).one_launch("t", 500, False, "any")
        self.assertIn("the browser was asked nothing", result["defects"][0])

    def test_what_it_was_asked_stays_in_the_results(self):
        self.driver = FakeDriver([self.LINES])
        self.launch_appears()
        _, _, result = ceiling.Bench(self.args()).one_launch("t", 500, False, "any")
        self.assertEqual(result["defects"], [])
        self.assertEqual(result["decodingAsked"], self.driver.asked)

    def test_the_limit_reaches_the_page_in_pixels_a_second(self):
        self.driver = FakeDriver([[P + "the browser refuses 498 M — this launch runs at 450 M",
                                   P + "450 M on hevc, asking 442 M, held back by it"]])
        self.launch_appears()
        a = self.args(refuse_above=400)
        _, _, result = ceiling.Bench(a).one_launch("t", 500, False, "any")
        self.assertEqual(result["defects"], [])
        self.assertTrue(any("__mwDecodingLimit = 400000000" in e for e in self.driver.evals))


class ThroughTheRendezvous(Rig):
    OWN = journal.key_of("abc123")

    def run_one(self, writes=None, **over):
        self.driver = FakeDriver([OneRung.CLEAN])
        self.launch_appears()
        inner = self.driver.stop

        def stop():
            self.driver.store.update(writes or {})
            inner()

        self.driver.stop = stop
        b = ceiling.Bench(self.args(rendezvous=True, **over))
        return b, b.one_launch("t", 500, False, "any")[2]

    def test_the_page_opens_at_the_line_and_learns_under_its_identifier(self):
        b, result = self.run_one()
        self.assertEqual(result["defects"], [])
        self.assertEqual(set(self.driver.visited), {self.line})
        self.assertIn(UUID + "|hevc", json.loads(self.driver.store[self.OWN])["hosts"])
        self.assertNotIn(journal.KEY, self.driver.store)
        self.assertEqual(b.key, self.OWN)

    def test_a_plain_key_written_is_a_defect(self):
        _, result = self.run_one({journal.KEY: '{"v":1,"hosts":{"x|hevc":{}}}'})
        self.assertIn("through the rendezvous, and the plain key holds something", result["defects"])

    def test_another_machine_touched_is_a_defect(self):
        _, result = self.run_one({ceiling.WITNESS_KEY: "gone"})
        self.assertIn("another machine's entry changed", result["defects"])

    def test_the_plain_key_left_by_an_older_pass_is_cleared_first(self):
        self.driver = FakeDriver([OneRung.CLEAN])
        self.driver.store[journal.KEY] = "old"
        self.launch_appears()
        _, _, result = ceiling.Bench(self.args(rendezvous=True)).one_launch("t", 500, False, "any")
        self.assertEqual(result["defects"], [])

    def test_only_the_staging_server(self):
        for line in ("https://stream.example/abc123", "", None):
            self.line = line
            self.driver = FakeDriver([])
            with self.assertRaises(SystemExit):
                ceiling.Bench(self.args(rendezvous=True))
            self.assertEqual(self.driver.visited, [])

    def test_the_identifier_stays_out_of_the_log(self):
        printed = []
        with mock.patch.object(ceiling, "say", printed.append):
            self.run_one()
        self.assertTrue(printed)
        self.assertFalse([line for line in printed if "abc123" in line])

    def test_without_it_nothing_changes(self):
        self.driver = FakeDriver([OneRung.CLEAN])
        self.launch_appears()
        b = ceiling.Bench(self.args())
        self.assertEqual((b.key, b.memories), (journal.KEY, ceiling.MEMORIES))
        _, _, result = b.one_launch("t", 500, False, "any")
        self.assertNotIn(ceiling.WITNESS_KEY, result["after"])
        self.assertEqual(self.driver.visited, ["https://10.0.0.2/"] * 2)


class APreparedKey(Rig):
    def launch_at_the_base(self, lines, learn, expect):
        self.driver = FakeDriver([lines])
        self.driver.asked = []
        self.launch_appears()
        a = self.args(rung=250, secs=10, learn=learn, expect=expect)
        return ceiling.Bench(a).one_launch("t", 250, False, expect)[2]

    def test_the_switch_off_is_not_blamed_for_the_pass_s_own_key(self):
        self.assertEqual(self.launch_at_the_base([], "0", "silent")["defects"], [])

    def test_the_journal_is_not_blamed_for_the_pass_s_own_key(self):
        line = P + "250 M on hevc, asking 248 M, held back by it (journal only)"
        self.assertEqual(self.launch_at_the_base([line], "log", "journal")["defects"], [])


class TheEnhancer(Rig):
    HELD = [P + "330 M on hevc, on trial, asking 328 M, held back by it",
            P + "330 M held, 300 s loaded",
            P + "450 M earned for the next launch, 300 s loaded",
            P + "stream over, 400 s loaded at 330 M"]

    def launch_showing(self, enhancer, enhance=True):
        """A 330 M rung held, the card's Enhancer row saying `enhancer`."""
        self.driver = FakeDriver([self.HELD])
        inner = self.driver.stats

        def stats():
            out = inner()
            if out.get("canvas") and enhancer:
                out["rows"]["Enhancer:"] = enhancer
            return out

        self.driver.stats = stats
        self.launch_appears()
        b = ceiling.Bench(self.args(rung=330, enhance=enhance))
        result = b.one_launch("t", 330, False, "hold")[2]
        b.close()
        return result

    def test_turns_it_on_and_sees_mwsr_at_work(self):
        result = self.launch_showing("WebGL MWSR")
        self.assertEqual(self.driver.settings["video_enhancement"], "on")
        self.assertEqual(self.driver.settings["video_enhancement_algo"], "auto")
        self.assertEqual(result["samples"][0]["enhancer"], "WebGL MWSR")
        self.assertEqual(result["defects"], [])

    def test_takes_a_stream_that_fills_the_screen_for_mwsr_idle(self):
        self.assertEqual(self.launch_showing("Off (1:1)")["defects"], [])

    def test_says_when_mwsr_never_ran(self):
        self.assertIn("the Enhancer was asked for and never ran MWSR: WebGL SGSR",
                      self.launch_showing("WebGL SGSR")["defects"])

    def test_says_when_the_card_never_showed_it(self):
        self.assertIn("the Enhancer was asked for and the card never showed it",
                      self.launch_showing(None)["defects"])

    def test_leaves_it_alone_otherwise(self):
        result = self.launch_showing(None, enhance=False)
        self.assertNotIn("video_enhancement", self.driver.settings)
        self.assertEqual(result["defects"], [])

    def test_reads_its_row_in_every_language(self):
        for label in ("Enhancer:", "Amélioration :", "增强器："):
            self.assertEqual(ceiling.enhancer_of({"Resolution:": "1", label: "MWSR 2"}), "MWSR 2")


class Silence(unittest.TestCase):
    def test_the_tool_plays_no_music(self):
        class Base:
            pass

        fake = types.SimpleNamespace(Load=Base, _tool=lambda: "tool.exe")
        with mock.patch.object(ceiling.subprocess, "Popen") as popen, mock.patch.object(ceiling.os, "makedirs"):
            ceiling.silent_load(fake, "0:1", "out/x.jsonl", 88)
        argv = popen.call_args[0][0]
        self.assertIn("--no-music", argv)
        self.assertEqual(argv[argv.index("--level") + 1], "88")


class Load(Rig):
    def setUp(self):
        super().setUp()
        # A fake takes the tool's place here: what starts the tool is Silence's.
        started = mock.patch.object(ceiling, "silent_load", lambda tool, gpu, path, level: tool.Load(gpu, path, level))
        started.start()
        self.addCleanup(started.stop)

    def test_a_tool_that_says_no_is_not_started_again(self):
        a = self.args(load_gpu="0:1", load_at=0)
        runs = []

        class Refused:
            path = "x"

            def __init__(self, gpu, path, level=None):
                runs.append(level)

            def snapshot(self, seconds):
                return {"running": False, "fps": None, "tempC": None, "level": 0, "end": None}

            def stop(self):
                pass

        said = [{"event": "refused", "reason": "no-gpu", "detail": "No GPU matches"}]
        fake = types.SimpleNamespace(Load=Refused, read=lambda path: said)
        with mock.patch.dict(sys.modules, {"gpu_load": fake}):
            cycles = ceiling.LoadCycles(a, "t")
            cycles.keep_going(1)
            cycles.keep_going(3)
            cycles.keep_going(5)
        self.assertEqual(len(runs), 1)
        self.assertIn("no-gpu", cycles.stop()["stoppedBy"])

    def test_a_load_stops_when_the_stream_falls_ill(self):
        self.driver = FakeDriver([[P + "700 M on hevc, asking 695 M, held back by it",
                                   P + "sick (decode_rate), 90 s loaded, at 700 M — down to 500 M",
                                   P + "stream over, 90 s loaded at 700 M, sick (decode_rate)"]])
        self.launch_appears()
        made = []

        class FakeLoad:
            def __init__(self, gpu, path, level=None):
                made.append((gpu, level))
                self.stopped = False

            def snapshot(self, seconds):
                return {"running": True, "fps": 45, "tempC": 60, "level": 90, "end": None}

            def stop(self):
                self.stopped = True

        fake = types.SimpleNamespace(Load=FakeLoad, read=lambda path: [])
        with mock.patch.dict(sys.modules, {"gpu_load": fake}):
            a = self.args(rung=700, secs=120, load_gpu="0:1", load_at=0, expect="any")
            _, _, result = ceiling.Bench(a).one_launch("ill", 700, False, "any")
        self.assertEqual(made, [("0:1", None)])
        self.assertEqual(result["journal"]["sick"]["reason"], "decode_rate")
        self.assertEqual(result["load"]["gpu"], "0:1")

    def test_each_run_after_the_first_replays_its_level(self):
        a = self.args(load_gpu="0:1", load_at=5)
        runs = []

        class Ended:
            path = "x"

            def __init__(self, gpu, path, level=None):
                runs.append(level)

            def snapshot(self, seconds):
                return {"running": False, "fps": 45, "tempC": 60, "level": 88.5,
                        "end": {"reason": "duration"}}

            def stop(self):
                pass

        with mock.patch.dict(sys.modules, {"gpu_load": types.SimpleNamespace(Load=Ended, read=lambda path: [])}):
            cycles = ceiling.LoadCycles(a, "t")
            cycles.keep_going(1)
            self.assertEqual(runs, [])
            cycles.keep_going(10)
            cycles.keep_going(70)
        self.assertEqual(runs, [None, 88.5])

    def test_a_run_stopped_by_the_heat_is_not_started_again(self):
        a = self.args(load_gpu="0:1", load_at=0)
        runs = []

        class Hot:
            path = "x"

            def __init__(self, gpu, path, level=None):
                runs.append(level)

            def snapshot(self, seconds):
                return {"running": False, "fps": 45, "tempC": 84, "level": 88,
                        "end": {"reason": "thermal"}}

            def stop(self):
                pass

        with mock.patch.dict(sys.modules, {"gpu_load": types.SimpleNamespace(Load=Hot, read=lambda path: [])}):
            cycles = ceiling.LoadCycles(a, "t")
            cycles.keep_going(1)
            cycles.keep_going(70)
            cycles.keep_going(140)
        self.assertEqual(len(runs), 1)
        self.assertEqual(cycles.stop()["stoppedBy"], "thermal")


class Climb(Rig):
    def launches(self, *rungs):
        out = []
        for rung in rungs:
            next_up = journal.rung_above(rung)
            if next_up:
                out.append([P + "%d M on hevc, asking %d M, held back by it" % (rung, rung - 2),
                            P + "%d M earned for the next launch, 300 s loaded" % next_up])
            else:
                out.append([P + "%d M on hevc, asking %d M" % (rung, rung - 5),
                            P + "%d M held, 300 s loaded" % rung])
        return out

    def run_climb(self, scripts, **over):
        self.driver = FakeDriver(scripts)
        self.launch_appears()
        argv = ["ceiling.py", "climb", "--tag", "c", "--target", "vdisplay"]
        for k, v in over.items():
            argv += ["--" + k.replace("_", "-"), str(v)]
        with mock.patch.object(sys, "argv", argv):
            ceiling.main()

    def test_it_stops_when_the_top_has_held(self):
        self.run_climb(self.launches(*journal.RUNGS_M))
        self.assertEqual(self.driver.launches, len(journal.RUNGS_M))
        self.assertEqual(self.driver.scripts, [])
        # Nothing prepared at the start, nothing cleared after that.
        cleared = [e for e in self.driver.evals if "removeItem" in e and journal.KEY in e]
        self.assertEqual(len(cleared), 1)
        self.assertIn("kiosk", self.closed)

    def test_it_stops_at_a_rung_that_holds_this_screen_back_no_more(self):
        scripts = [[P + "250 M on hevc, asking 124 M, proves nothing of it"]]
        self.run_climb(scripts, secs=10)
        self.assertEqual(self.driver.launches, 1)

    def test_no_launch_starts_that_could_end_after_the_deadline(self):
        room = 420 + ceiling.LAUNCH_EXTRA_S + 1
        with mock.patch.object(ceiling, "deadline_at", lambda text: self.clock[0] + room):
            self.run_climb(self.launches(*journal.RUNGS_M), deadline="21:45")
        self.assertEqual(self.driver.launches, 1)
        self.assertIn("kiosk", self.closed)

    def test_the_deadline_is_today_at_that_time(self):
        when = time.localtime(ceiling.deadline_at("21:45"))
        self.assertEqual((when.tm_hour, when.tm_min, when.tm_sec), (21, 45, 0))
        self.assertEqual(when.tm_yday, time.localtime().tm_yday)
        self.assertEqual(ceiling.deadline_at(""), 0)

    def test_the_cadence_switch_is_put_after_the_memories_are_cleared(self):
        self.run_climb(self.launches(*journal.RUNGS_M[4:]), rung=700, autostep=0)
        evals = self.driver.evals
        cleared = max(i for i, e in enumerate(evals) if "removeItem" in e and "mw_autostep'" in e)
        put = max(i for i, e in enumerate(evals) if "setItem" in e and "mw_autostep'" in e)
        self.assertGreater(put, cleared)

    def test_it_can_go_on_from_a_rung_reached_before(self):
        self.run_climb(self.launches(*journal.RUNGS_M[3:]), rung=500)
        self.assertEqual(self.driver.launches, 3)
        prepared = [e for e in self.driver.evals if "setItem" in e and journal.KEY in e]
        self.assertEqual(len(prepared), 1)

    def test_a_launch_that_falls_ill_ends_it(self):
        scripts = self.launches(250) + [[P + "330 M on hevc, asking 328 M, held back by it",
                                         P + "sick (drops), 90 s loaded, at 330 M — down to 250 M"]]
        with self.assertRaises(SystemExit) as stopped:
            self.run_climb(scripts)
        self.assertEqual(stopped.exception.code, 1)
        self.assertEqual(self.driver.launches, 2)


class Picture(Rig):
    def test_each_virtual_display_gets_the_page(self):
        seq = [list(BASE), BASE + [VIRTUAL], BASE + [VIRTUAL], list(BASE),
               BASE + [VIRTUAL], list(BASE)]
        run = sys.modules["run"]
        real = sys.modules["cadence"].monitors

        def monitors():
            return seq.pop(0) if len(seq) > 1 else seq[0]

        sys.modules["cadence"].monitors = monitors
        self.addCleanup(setattr, sys.modules["cadence"], "monitors", real)
        a = self.args(mode="picture", secs=600, max_launches=2, page="p.html")
        ceiling.picture(a)
        self.assertEqual(self.contents, [("p.html", "0,0,3840,2160")] * 2)
        self.assertEqual(self.closed.count("content"), 2)
        self.assertIs(run, sys.modules["run"])


if __name__ == "__main__":
    unittest.main()
