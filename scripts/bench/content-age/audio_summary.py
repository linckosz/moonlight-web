#!/usr/bin/env python3
"""One line per pass from the page's audio log (`<tag>.audio.csv`, mwAudio.csv()).

usage: audio_summary.py [--skip S] <file or glob>...

Per pass: seconds kept; NetEq's buffer (median, p90), its target and minimum
(medians); the arrival jitter (median, p90); packets and losses; the share of
the sound NetEq invented (concealed, not counting the silent kind), its
concealment events, and what it stretched (inserted) and cut (removed) to
follow its target; where the page logged them (06/10), the packets NetEq
discarded although they arrived, its buffer flushes (Chrome) and the energy
of the sound played (totalAudioEnergy, x1000); where it logged them (10/10),
what the audio device took (media-playout, Chrome): the lowest second's
played ms (under 1000 = the output stopped asking), the ms and the events the
browser made up at the output, and the output's delay. --skip drops the first S
seconds (the stream's start, where NetEq still starts from its own 80 ms).

The buffer against its target and minimum is the A2 question: a buffer that
sits on the page's floor (60 ms) while NetEq alone would want less is the
gain the plan is after (mw_audio_target=off on the same client says how much).
"""
import csv
import glob
import os
import sys


def pct(values, q):
    v = sorted(x for x in values if x >= 0)
    if not v:
        return float("nan")
    return v[min(len(v) - 1, int(q * (len(v) - 1) + 0.5))]


def opt(values):
    """The sum of a column the page may not have logged (-1 when absent)."""
    v = [x for x in values if x >= 0]
    return sum(v) if v else -1


def summary(path, skip_s):
    with open(path, newline="") as f:
        rows = [{k: float(v) for k, v in r.items() if v not in ("", None)} for r in csv.DictReader(f)]
    rows = [r for r in rows if r.get("t", 0) >= skip_s * 1000]
    if not rows:
        return None
    col = lambda c: [r[c] for r in rows if c in r]
    samples = sum(col("samples")) or 1
    audible = sum(col("concealed")) - sum(col("silentConcealed"))
    return {
        "s": len(rows),
        "buf": pct(col("bufferMs"), 0.5), "buf90": pct(col("bufferMs"), 0.9),
        "target": pct(col("targetMs"), 0.5), "min": pct(col("minimumMs"), 0.5),
        "jit": pct(col("jitterMs"), 0.5), "jit90": pct(col("jitterMs"), 0.9),
        "pkts": int(sum(col("packets"))), "lost": int(sum(col("lost"))),
        "conc": 100.0 * audible / samples, "events": int(sum(col("concealmentEvents"))),
        "ins": 100.0 * sum(col("inserted")) / samples, "rem": 100.0 * sum(col("removed")) / samples,
        "disc": opt(col("discarded")), "flush": opt(col("flushes")),
        "energy": 1000.0 * opt(col("energy")) if opt(col("energy")) >= 0 else -1.0,
        "played": pct(col("playedMs"), 0.0), "synth": opt(col("synthMs")),
        "synthev": opt(col("synthEvents")), "out": pct(col("outputMs"), 0.5),
    }


def main():
    args = sys.argv[1:]
    skip = 0.0
    if "--skip" in args:
        i = args.index("--skip")
        skip = float(args[i + 1])
        del args[i:i + 2]
    paths = sorted({p for a in args for p in (glob.glob(a) or ([a] if os.path.exists(a) else []))})
    print("%-58s %4s %6s %6s %6s %6s %5s %5s %6s %4s %6s %4s %5s %5s %5s %5s %7s %6s %6s %5s %5s" % (
        "pass", "s", "buf", "p90", "target", "min", "jit", "p90", "pkts", "lost",
        "conc%", "ev", "ins%", "rem%", "disc", "flush", "energy", "played", "synth", "sev", "out"))
    for p in paths:
        s = summary(p, skip)
        if not s:
            continue
        name = os.path.basename(p).replace(".audio.csv", "")
        print("%-58s %4d %6.1f %6.1f %6.1f %6.1f %5.1f %5.1f %6d %4d %6.2f %4d %5.2f %5.2f %5d %5d %7.2f %6.0f %6.1f %5d %5.1f" % (
            name[:58], s["s"], s["buf"], s["buf90"], s["target"], s["min"], s["jit"], s["jit90"],
            s["pkts"], s["lost"], s["conc"], s["events"], s["ins"], s["rem"], s["disc"], s["flush"],
            s["energy"], s["played"], s["synth"], s["synthev"], s["out"]))


if __name__ == "__main__":
    main()
