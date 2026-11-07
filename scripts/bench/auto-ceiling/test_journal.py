"""python -m unittest test_journal  (no machine, no browser)

The lines below are the page's own, from frontend/js/stream/AutoCeilingSession.js.
test_lines_match_the_page keeps it so: it fails when a sentence changes there.
"""
import json
import os
import re
import unittest

import journal

HERE = os.path.dirname(os.path.abspath(__file__))
UUID = "00000000-0000-4000-8000-000000000000"
P = journal.PREFIX


def key(ceiling, open_at=0, codec="hevc", failed=()):
    fails = {str(m * 1000000): {"n": 1, "at": 1} for m in failed}
    return json.dumps({"v": 1, "last": "", "hosts": {
        "%s|%s" % (UUID, codec): {"ceiling": ceiling * 1000000, "trial": False,
                                  "openAt": open_at, "fails": fails, "at": 1}}})


class Parse(unittest.TestCase):
    def test_a_launch_held_back_that_earns_the_next_rung(self):
        s = journal.parse([
            P + "500 M on hevc, on trial, asking 498 M, held back by it",
            P + "500 M held, 300 s loaded",
            P + "700 M earned for the next launch, 300 s loaded",
            P + "stream over, 590 s loaded at 500 M",
        ])
        self.assertEqual(s["rung"], 500)
        self.assertEqual(s["codec"], "hevc")
        self.assertTrue(s["trial"])
        self.assertEqual(s["asking"], 498)
        self.assertTrue(s["limited"])
        self.assertTrue(s["proves"])
        self.assertFalse(s["journalOnly"])
        self.assertEqual(s["held"], [{"rung": 500, "loaded": 300}])
        self.assertEqual(s["earned"], [{"rung": 700, "loaded": 300}])
        self.assertEqual(s["over"], {"loaded": 590, "rung": 500, "sick": None})

    def test_a_launch_that_proves_nothing_in_the_journal(self):
        s = journal.parse([P + "250 M on av1, asking 124 M, proves nothing of it (journal only)"])
        self.assertFalse(s["limited"])
        self.assertFalse(s["proves"])
        self.assertTrue(s["journalOnly"])
        self.assertFalse(s["trial"])

    def test_a_stream_that_falls_ill_at_its_rung(self):
        s = journal.parse([
            P + "sick (drops), 75 s loaded, at 700 M — down to 500 M",
            P + "stream over, 75 s loaded at 700 M, sick (drops)",
        ])
        self.assertEqual(s["sick"], {"reason": "drops", "loaded": 75, "atRung": True, "down": 500})
        self.assertEqual(s["over"]["sick"], "drops")

    def test_a_stream_that_falls_ill_away_from_its_rung(self):
        s = journal.parse([P + "sick (congestion), 12 s loaded, not at its rung"])
        self.assertEqual(s["sick"], {"reason": "congestion", "loaded": 12, "atRung": False,
                                     "down": None})

    def test_the_refusals_and_the_crash(self):
        s = journal.parse([
            P + "the last stream at 700 M never came back — down to 500 M",
            P + "the browser refuses 995 M — this launch runs at 700 M",
            P + "the host refused a launch at 700 M — trying again at 500 M",
        ])
        self.assertEqual(s["crashed"], {"rung": 700, "down": 500})
        self.assertEqual(s["browserRefused"], [{"asked": 995, "runs": 700}])
        self.assertEqual(s["hostRefused"], [{"rung": 700, "retry": 500}])

    def test_other_lines_do_not_count(self):
        s = journal.parse(["[MW] Pixel budget: 1440 lines at 120 fps → 1080 lines at 120 fps"])
        self.assertEqual(s["lines"], 0)

    def test_lines_match_the_page(self):
        """Every sentence the page can print, read from its source, is one parse() knows."""
        path = os.path.join(HERE, "..", "..", "..", "frontend", "js", "stream",
                            "AutoCeilingSession.js")
        with open(path, encoding="utf-8") as f:
            src = f.read()
        for words in ("the last stream at ", " never came back", " — down to ",
                      "the browser refuses ", " — this launch runs at ",
                      "the host refused a launch at ", " — trying again at ",
                      " on ", ", on trial", ", asking ", ", held back by it",
                      ", proves nothing of it", " (journal only)", " held, ", " s loaded",
                      " earned for the next launch, ", "sick (", "not at its rung", "at ",
                      "stream over, ", " s loaded at ", ", sick ("):
            self.assertIn(words, src, "the page no longer says %r" % words)
        self.assertIn("'[MW] Auto ceiling: ", src)
        self.assertTrue(re.search(r"const millions = .*' M'", src))


class Guard(unittest.TestCase):
    ASKED = [{"contentType": 'video/mp4; codecs="hev1.1.6.L153.90"', "answer": {"smooth": True}}]

    def launch(self, *lines):
        return journal.parse([P + line for line in lines])

    def test_a_rung_past_the_one_below_is_asked_about(self):
        s = self.launch("450 M on hevc, on trial, asking 442 M, held back by it")
        self.assertEqual(journal.judge_guard(s, self.ASKED), [])
        self.assertIn("the browser was asked nothing", journal.judge_guard(s, [])[0])

    def test_about_the_codec_launched(self):
        s = self.launch("450 M on av1, on trial, asking 442 M, held back by it")
        self.assertIn("asked about", journal.judge_guard(s, self.ASKED)[0])

    def test_nothing_to_ask_at_the_base_or_under_the_rung_below(self):
        self.assertEqual(journal.judge_guard(self.launch("250 M on hevc, asking 249 M"), []), [])
        self.assertEqual(journal.judge_guard(self.launch("450 M on hevc, asking 300 M"), []), [])

    def test_a_journal_only_or_a_silent_launch_asks_nothing(self):
        s = self.launch("250 M on hevc, asking 249 M, held back by it (journal only)")
        self.assertEqual(journal.judge_guard(s, []), [])
        self.assertEqual(journal.judge_guard(journal.parse([]), []), [])

    def test_a_no_brings_the_launch_down(self):
        down = self.launch("the browser refuses 442 M — this launch runs at 330 M",
                           "330 M on hevc, asking 328 M, held back by it")
        self.assertEqual(journal.judge_guard(down, self.ASKED, 400), [])
        stayed = self.launch("450 M on hevc, on trial, asking 442 M, held back by it")
        self.assertIn("did not come down", journal.judge_guard(stayed, self.ASKED, 400)[0])


class Rendezvous(unittest.TestCase):
    OWN = '{"v":1,"hosts":{"x|hevc":{"ceiling":330000000}}}'

    def after(self, **over):
        base = {journal.KEY: None, journal.key_of("abc"): self.OWN, "witness": "w"}
        base.update(over)
        return base

    def test_the_key_of_a_machine(self):
        self.assertEqual(journal.key_of(), journal.KEY)
        self.assertEqual(journal.key_of("abc"), journal.KEY + ":abc")

    def test_the_identifier_is_the_last_of_the_address(self):
        for url in ("https://stream.dev.example/abc", "https://stream.dev.example/abc/",
                    "https://stream.dev.example/abc?v=1", "https://stream.dev.example/abc#k=z"):
            self.assertEqual(journal.identifier_of(url), "abc")

    def test_its_own_key_and_no_other(self):
        self.assertEqual(journal.judge_rendezvous(self.after(), "abc", "witness", "w"), [])

    def test_the_plain_key_written_is_the_defect_this_is_about(self):
        bad = journal.judge_rendezvous(self.after(**{journal.KEY: self.OWN}), "abc", "witness", "w")
        self.assertIn("the plain key holds something", bad[0])

    def test_another_machine_is_left_alone(self):
        bad = journal.judge_rendezvous(self.after(witness="changed"), "abc", "witness", "w")
        self.assertEqual(bad, ["another machine's entry changed"])

    def test_its_own_key_empty_only_when_nothing_could_be_learned(self):
        empty = self.after(**{journal.key_of("abc"): None})
        self.assertIn("holds nothing", journal.judge_rendezvous(empty, "abc", "witness", "w")[0])
        self.assertEqual(journal.judge_rendezvous(empty, "abc", "witness", "w", learned=False), [])


class Prepared(unittest.TestCase):
    def test_a_key_the_page_reads_as_held(self):
        k = journal.prepared_key(UUID, 500, 1234)
        self.assertEqual(k["v"], 1)
        e = k["hosts"][UUID + "|hevc"]
        self.assertEqual(e["ceiling"], 500000000)
        self.assertFalse(e["trial"])
        self.assertEqual(e["openAt"], 0)
        self.assertEqual(sorted(k["hosts"]), sorted(UUID + "|" + c
                                                    for c in ("hevc", "av1", "h264")))

    def test_only_a_real_rung(self):
        with self.assertRaises(ValueError):
            journal.prepared_key(UUID, 600, 1)

    def test_rungs_as_the_page_has_them(self):
        path = os.path.join(HERE, "..", "..", "..", "frontend", "js", "util", "AutoCeiling.js")
        with open(path, encoding="utf-8") as f:
            src = f.read()
        block = re.search(r"CEILING_RUNGS = Object\.freeze\(\[(.*?)\]\)", src, re.S).group(1)
        found = [int(n) // 1000000 for n in re.findall(r"\b(\d{9,})\b", block)]
        self.assertEqual(tuple(found), journal.RUNGS_M[1:])
        self.assertIn('AUTO_CEILING_KEY = \'%s\'' % journal.KEY, src)


class Judge(unittest.TestCase):
    def good(self):
        return journal.parse([
            P + "500 M on hevc, asking 498 M, held back by it",
            P + "500 M held, 300 s loaded",
            P + "700 M earned for the next launch, 300 s loaded",
            P + "stream over, 590 s loaded at 500 M",
        ])

    def test_a_clean_rung(self):
        self.assertEqual(journal.judge(self.good(), "hold", key(700), UUID), [])

    def test_a_rung_that_did_not_earn(self):
        s = journal.parse([P + "500 M on hevc, asking 498 M, held back by it",
                           P + "stream over, 40 s loaded at 500 M"])
        bad = journal.judge(s, "hold", key(500), UUID)
        self.assertEqual(len(bad), 1)
        self.assertIn("did not earn", bad[0])
        self.assertIn("40 s", bad[0])

    def test_the_top_rung_has_nothing_to_earn(self):
        s = journal.parse([P + "1000 M on hevc, asking 995 M",
                           P + "stream over, 590 s loaded at 1000 M"])
        self.assertEqual(journal.judge(s, "hold", key(1000), UUID), [])

    def test_an_entry_left_open_is_a_defect(self):
        bad = journal.judge(self.good(), "hold", key(700, open_at=99), UUID)
        self.assertEqual(len(bad), 1)
        self.assertIn("still says it is open", bad[0])

    def test_sick_and_refused_are_said(self):
        s = journal.parse([
            P + "the browser refuses 995 M — this launch runs at 700 M",
            P + "700 M on hevc, asking 695 M, held back by it",
            P + "sick (decode_rate), 90 s loaded, at 700 M — down to 500 M",
        ])
        text = " / ".join(journal.judge(s, "hold", key(500), UUID))
        self.assertIn("sick: decode_rate", text)
        self.assertIn("browser refused", text)

    def test_a_stream_too_small_proves_nothing(self):
        s = journal.parse([P + "500 M on hevc, asking 124 M, proves nothing of it"])
        bad = journal.judge(s, "hold", key(500), UUID)
        self.assertEqual(len(bad), 1)
        self.assertIn("proves nothing", bad[0])

    def test_silent_means_no_line_and_no_key(self):
        self.assertEqual(journal.judge(journal.parse([]), "silent", None, UUID), [])
        self.assertEqual(journal.judge(journal.parse([]), "silent", "not json", UUID), [])
        self.assertEqual(len(journal.judge(journal.parse([]), "silent", key(250), UUID)), 1)
        s = journal.parse([P + "250 M on hevc, asking 27 M, proves nothing of it"])
        self.assertEqual(len(journal.judge(s, "silent", None, UUID)), 1)

    def test_no_line_at_all_is_never_a_pass(self):
        for expect in ("hold", "journal", "any"):
            self.assertEqual(len(journal.judge(journal.parse([]), expect, None, UUID)), 1)

    def test_journal_mode_writes_nothing(self):
        s = journal.parse([P + "250 M on hevc, asking 248 M, held back by it (journal only)"])
        self.assertEqual(journal.judge(s, "journal", None, UUID), [])
        self.assertEqual(len(journal.judge(s, "journal", key(330), UUID)), 1)

    def test_a_key_the_pass_put_there_is_none_of_the_page_s_doing(self):
        quiet = journal.parse([])
        said = journal.parse([P + "250 M on hevc, asking 248 M, held back by it (journal only)"])
        prepared = key(250)
        self.assertEqual(journal.judge(quiet, "silent", prepared, UUID, prepared=prepared), [])
        self.assertEqual(journal.judge(said, "journal", prepared, UUID, prepared=prepared), [])
        # One word changed, and it is the page's.
        self.assertEqual(len(journal.judge(quiet, "silent", key(330), UUID, prepared=prepared)), 1)
        self.assertEqual(len(journal.judge(said, "journal", key(330), UUID, prepared=prepared)), 1)

    def ill(self, *more):
        return journal.parse([P + "1000 M on hevc, asking 995 M",
                              P + "sick (decode_rate), 40 s loaded, at 1000 M — down to 700 M"] + list(more))

    def test_a_client_too_weak_for_its_rung(self):
        self.assertEqual(journal.judge(self.ill(), "sick", key(700, failed=[1000]), UUID), [])

    def test_a_weak_client_that_held_is_a_defect(self):
        s = journal.parse([P + "1000 M on hevc, asking 995 M", P + "1000 M held, 300 s loaded"])
        bad = journal.judge(s, "sick", key(1000), UUID)
        self.assertEqual(len(bad), 1)
        self.assertIn("no line says it did", bad[0])

    def test_ill_away_from_its_rung_loses_nothing(self):
        s = journal.parse([P + "1000 M on hevc, asking 995 M",
                           P + "sick (congestion), 12 s loaded, not at its rung"])
        bad = journal.judge(s, "sick", key(1000), UUID)
        self.assertEqual(len(bad), 1)
        self.assertIn("not at its rung", bad[0])

    def test_the_key_must_come_down_one_rung_and_count_it(self):
        text = " / ".join(journal.judge(self.ill(), "sick", key(1000), UUID))
        self.assertIn("holds 1000 M, not 700 M", text)
        self.assertIn("counts no failure", text)
        self.assertEqual(len(journal.judge(self.ill(), "sick", key(500, failed=[1000]), UUID)), 1)

    def test_a_weak_client_s_entry_left_open(self):
        bad = journal.judge(self.ill(), "sick", key(700, open_at=99, failed=[1000]), UUID)
        self.assertEqual(len(bad), 1)
        self.assertIn("still says it is open", bad[0])

    def test_a_browser_that_said_no_first(self):
        # The guard brought the launch to 700 M: that is the rung the stream fell ill at.
        s = journal.parse([P + "the browser refuses 995 M — this launch runs at 700 M",
                           P + "700 M on hevc, asking 695 M",
                           P + "sick (drops), 30 s loaded, at 700 M — down to 500 M"])
        self.assertEqual(journal.judge(s, "sick", key(500, failed=[700]), UUID), [])

    def test_learned_m(self):
        self.assertEqual(journal.learned_m(key(700), UUID, "hevc"), 700)
        self.assertIsNone(journal.learned_m(key(700), UUID, "av1"))
        self.assertIsNone(journal.learned_m(None, UUID, "hevc"))


if __name__ == "__main__":
    unittest.main()
