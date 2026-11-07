"""What a stream said of Auto's ceiling, and whether that is what it should say.

Pure: no browser, no host. Reads the `[MW] Auto ceiling: …` lines the page prints
(frontend/js/stream/AutoCeilingSession.js) and the page's `mw_auto_ceiling` key,
so that test_journal.py can check it with no machine at all.
"""
import re

RUNGS_M = (250, 330, 450, 500, 700, 1000)
BASE_M = RUNGS_M[0]
KEY = "mw_auto_ceiling"
# A stream that runs this far over its rung says nothing of it (RUNG_MARGIN).
RUNG_MARGIN = 1.02

PREFIX = "[MW] Auto ceiling: "

_LAUNCH = re.compile(r"^(\d+) M on (\S+?)(, on trial)?, asking (\d+) M(.*)$")
_HELD = re.compile(r"^(\d+) M held, (\d+) s loaded")
_EARNED = re.compile(r"^(\d+) M earned for the next launch, (\d+) s loaded")
_SICK = re.compile(r"^sick \(([^)]*)\), (\d+) s loaded, (at (\d+) M|not at its rung)"
                   r"(?: — down to (\d+) M)?")
_OVER = re.compile(r"^stream over, (\d+) s loaded at (\d+) M(?:, sick \(([^)]*)\))?")
_BROWSER = re.compile(r"^the browser refuses (\d+) M — this launch runs at (\d+) M")
_HOST = re.compile(r"^the host refused a launch at (\d+) M — trying again at (\d+) M")
_CRASH = re.compile(r"^the last stream at (\d+) M never came back(?: — down to (\d+) M)?")


def key_of(identifier=""):
    """The key a page keeps its memory under: through the rendezvous, its machine's."""
    return "%s:%s" % (KEY, identifier) if identifier else KEY


def identifier_of(url):
    """The machine's rendezvous identifier, from the address the page is opened at."""
    return url.split("#")[0].split("?")[0].rstrip("/").rsplit("/", 1)[-1]


def prepared_key(uuid, rung_m, now_ms, trial=False, codecs=("hevc", "av1", "h264")):
    """The key as a browser that had earned this rung would hold it.

    One entry per codec: Auto's codec is settled at the launch, and a key under
    the wrong one would run the pass at the base budget without a word.
    """
    if rung_m not in RUNGS_M:
        raise ValueError("no rung of %r M: %s" % (rung_m, RUNGS_M))
    hosts = {}
    for codec in codecs:
        hosts["%s|%s" % (uuid, codec)] = {
            "ceiling": rung_m * 1000000,
            "trial": trial,
            "openAt": 0,
            "fails": {},
            "at": now_ms,
        }
    return {"v": 1, "last": "%s|%s" % (uuid, codecs[0]), "hosts": hosts}


def parse(lines):
    """One launch's lines, in order, as what they say. Anything else is ignored."""
    s = {"lines": 0, "rung": None, "codec": None, "trial": None, "asking": None,
         "limited": None, "proves": None, "journalOnly": None,
         "held": [], "earned": [], "sick": None, "over": None,
         "browserRefused": [], "hostRefused": [], "crashed": None}
    for line in lines:
        if not line.startswith(PREFIX):
            continue
        text = line[len(PREFIX):]
        s["lines"] += 1
        m = _LAUNCH.match(text)
        if m:
            rest = m.group(5)
            s.update(rung=int(m.group(1)), codec=m.group(2), trial=bool(m.group(3)),
                     asking=int(m.group(4)), limited="held back by it" in rest,
                     proves="proves nothing of it" not in rest,
                     journalOnly="(journal only)" in rest)
            continue
        m = _HELD.match(text)
        if m:
            s["held"].append({"rung": int(m.group(1)), "loaded": int(m.group(2))})
            continue
        m = _EARNED.match(text)
        if m:
            s["earned"].append({"rung": int(m.group(1)), "loaded": int(m.group(2))})
            continue
        m = _SICK.match(text)
        if m:
            s["sick"] = {"reason": m.group(1), "loaded": int(m.group(2)),
                         "atRung": bool(m.group(4)),
                         "down": int(m.group(5)) if m.group(5) else None}
            continue
        m = _OVER.match(text)
        if m:
            s["over"] = {"loaded": int(m.group(1)), "rung": int(m.group(2)),
                         "sick": m.group(3)}
            continue
        m = _BROWSER.match(text)
        if m:
            s["browserRefused"].append({"asked": int(m.group(1)), "runs": int(m.group(2))})
            continue
        m = _HOST.match(text)
        if m:
            s["hostRefused"].append({"rung": int(m.group(1)), "retry": int(m.group(2))})
            continue
        m = _CRASH.match(text)
        if m:
            s["crashed"] = {"rung": int(m.group(1)),
                            "down": int(m.group(2)) if m.group(2) else None}
    return s


def entries(raw):
    """The key's entries, from its text; {} when it is absent or broken."""
    import json
    try:
        data = json.loads(raw) if raw else None
    except (TypeError, ValueError):
        return {}
    return (data or {}).get("hosts") or {}


def learned_m(raw, uuid, codec):
    """The rung the key holds for this host and codec, in M; None without an entry."""
    e = entries(raw).get("%s|%s" % (uuid, codec))
    return round(e["ceiling"] / 1000000) if e and e.get("ceiling") else None


def rung_above(rung_m):
    i = RUNGS_M.index(rung_m)
    return RUNGS_M[i + 1] if i + 1 < len(RUNGS_M) else None


def rung_below(rung_m):
    i = RUNGS_M.index(rung_m)
    return RUNGS_M[i - 1] if i > 0 else None


# What each codec is asked about (PROBE_CODECS in frontend/js/util/DecoderLimit.js).
_CODEC_STRINGS = {"hevc": "hev1", "av1": "av01", "h264": "avc1"}


def judge_guard(s, asked, refuse_above_m=0):
    """Whether the guard did its work on this launch. [] when it did.

    It stays silent on a "yes", so the pass listens to what the browser is asked.
    It asks when a rung is active and the launch goes past the rung below, which
    the launch line says. The last rung it reached is the one that line names.
    """
    bad = []
    if not s["lines"] or s["journalOnly"] or s["rung"] is None or s["asking"] is None:
        return bad
    below = rung_below(s["rung"])
    if below and s["asking"] > below:
        expected = _CODEC_STRINGS.get(s["codec"])
        if not asked:
            bad.append("%s M asked for %s M, and the browser was asked nothing" % (s["rung"], s["asking"]))
        elif expected and not any(expected in (a.get("contentType") or "") for a in asked):
            bad.append("on %s, and the browser was asked about %s" % (
                s["codec"], sorted({a.get("contentType") for a in asked})))
    if refuse_above_m and asked and not s["browserRefused"]:
        bad.append("the browser said no above %d M, and the launch did not come down" % refuse_above_m)
    return bad


def judge_rendezvous(after, identifier, witness_key, witness_value, learned=True):
    """Through the rendezvous, a page keeps to its machine's key. [] when it did."""
    bad = []
    if after.get(KEY):
        bad.append("through the rendezvous, and the plain key holds something")
    if learned and not entries(after.get(key_of(identifier))):
        bad.append("the machine's own key holds nothing")
    if after.get(witness_key) != witness_value:
        bad.append("another machine's entry changed")
    return bad


def judge(s, expect, raw_after, uuid, top_m=RUNGS_M[-1], prepared=None):
    """What is wrong with this launch against what it was run for. [] when nothing is.

    `expect`:
      "silent"  it must not learn at all: a TV, Power Saving, the switch off.
      "journal" it measures and writes nothing (mw_auto_learn=log).
      "hold"    an active rung, that the stream must run at and hold.
      "sick"    an active rung this client cannot hold: the signal at the rung, one
                rung down in the key, the failure counted, the entry closed.
      "any"     read only: the verdict is whatever the numbers say.

    `prepared` is the key as the pass itself left it: still word for word after the
    stream, it is none of the page's doing.
    """
    bad = []
    written = {} if raw_after == prepared else entries(raw_after)
    if expect == "silent":
        if s["lines"]:
            bad.append("it must learn nothing, and it printed %d line(s)" % s["lines"])
        if written:
            bad.append("it must learn nothing, and the key holds %s" % sorted(written))
        return bad
    if not s["lines"]:
        return ["not one line of the ceiling: did this launch reach the native host?"]
    if s["crashed"]:
        bad.append("the launch before this one never closed its stream")
    if expect == "journal":
        if not s["journalOnly"]:
            bad.append("journal mode, and the launch line does not say so")
        if written:
            bad.append("journal mode, and the key holds %s" % sorted(written))
        return bad
    if expect == "any":
        return bad
    entry = entries(raw_after).get("%s|%s" % (uuid, s["codec"]))
    if entry and entry.get("openAt"):
        bad.append("the stream ended and its entry still says it is open")
    if expect == "sick":
        return bad + _judge_sick(s, entry)
    if s["sick"]:
        bad.append("sick: %s, after %s s loaded" % (s["sick"]["reason"], s["sick"]["loaded"]))
    if s["browserRefused"]:
        bad.append("the browser refused it: %s" % s["browserRefused"])
    if s["hostRefused"]:
        bad.append("the host refused it: %s" % s["hostRefused"])
    if not s["proves"]:
        bad.append("asking %s M at %s M proves nothing of the rung" % (s["asking"], s["rung"]))
    if s["limited"] and s["rung"] < top_m and not s["earned"]:
        bad.append("held back by %s M and did not earn the next rung (%s s loaded)" % (
            s["rung"], (s["over"] or {}).get("loaded", "?")))
    return bad


def _judge_sick(s, entry):
    """A client that could not hold its rung, as the page must leave it.

    The rung is the one the launch line names: a browser that refused the prepared
    one has already brought the launch down, and the stream fell ill at that one.
    """
    sick = s["sick"]
    if not sick:
        return ["it was to fall ill at %s M, and no line says it did" % s["rung"]]
    if not sick["atRung"]:
        return ["ill (%s), but not at its rung: nothing of the rung was lost" % sick["reason"]]
    below = rung_below(s["rung"]) if s["rung"] in RUNGS_M else None
    if not below:
        return ["ill at %s M, a rung with none below it" % s["rung"]]
    bad = []
    if sick["down"] != below:
        bad.append("ill at %s M, and the line says down to %s M, not %s M" % (
            s["rung"], sick["down"], below))
    learned = round(entry["ceiling"] / 1000000) if entry and entry.get("ceiling") else None
    if learned != below:
        bad.append("ill at %s M, and the key holds %s M, not %s M" % (s["rung"], learned, below))
    if not ((entry or {}).get("fails") or {}).get(str(s["rung"] * 1000000)):
        bad.append("ill at %s M, and the key counts no failure there" % s["rung"])
    return bad
