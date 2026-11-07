"""Per combination: the set bitrate needed to reach each VMAF NEG target, per scene
(log-interpolated between the measured points), and that bitrate per megapixel-frame
(Mbit/s / (W*H*fps/1e6)) -- the unit an Auto rule scales with.

    py -E -X utf8 synth.py [--md]
"""
import csv, glob, json, math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import main_bench as mb
sys.path.insert(0, HERE)
from plot import scenes_from_csv  # noqa

TARGETS = [("1", "settled", 93.5), ("4", "settled", 94), ("2", "mean", 90), ("2", "mean", 93), ("2", "mean", 95),
           ("3", "mean", 93), ("3", "mean", 95), ("5", "mean", 90)]
NAMES = {"1": "Junk Shop", "4": "Gears", "2": "Google scroll", "3": "Google scroll/stop", "5": "Web scroll"}


def need(pts, target):
    """smallest set bitrate where the curve crosses target (log-x interpolation), None if never."""
    prev = None
    for x, v in pts:
        if v >= target:
            if prev is None:
                return ("<=", x)
            (x0, v0) = prev
            t = (target - v0) / (v - v0)
            return ("", math.exp(math.log(x0) + t * (math.log(x) - math.log(x0))))
        prev = (x, v)
    return (">", pts[-1][0]) if pts else None


def main():
    md = "--md" in sys.argv
    rows = []
    for w, h, fps, codec in mb.COMBOS:
        tag = "%dx%dp%d-%s" % (w, h, fps, codec)
        pts = {}
        for k in mb.rates(w, h, fps):
            r = os.path.join(HERE, "runs", "M", "%s-%05d" % (tag, k))
            if not os.path.exists(os.path.join(r, "summary.json")):
                continue
            sc = scenes_from_csv(os.path.join(r, "frames.csv"))
            for sid, kind in sorted({(s_, k_) for s_, k_, _ in TARGETS}):
                if sid in sc:
                    pts.setdefault((sid, kind), []).append((k / 1000, sc[sid][("negnm", kind)]))
        if not pts:
            continue
        mpf = w * h * fps / 1e6
        cells = []
        for sid, kind, tgt in TARGETS:
            n = need(sorted(pts.get((sid, kind), [])), tgt)
            if n is None:
                cells.append("-")
            else:
                op, x = n
                cells.append("%s%.1f (%s%.3f)" % (op, x, op, x / mpf))
        auto = mb.auto_mbps(w, h, fps)
        rows.append((tag, auto, "%.3f" % (auto / mpf), len(pts.get(("2", "mean"), [])), cells))
    hdr = ["Combination", "Auto", "Auto/MP·f", "pts"] + ["%s %s>=%g" % (NAMES[s], k[:3], t) for s, k, t in TARGETS]
    if md:
        print("| " + " | ".join(hdr) + " |")
        print("|" + "---|" * len(hdr))
        for t, a, apm, n, c in rows:
            print("| %s | %d | %s | %d | %s |" % (t, a, apm, n, " | ".join(c)))
    else:
        for t, a, apm, n, c in rows:
            print("%-22s auto %3d (%s/MPf) n=%2d  " % (t, a, apm, n) + "  ".join("%s=%s" % (h_.split(">=")[0][:10] + h_.split(">=")[1], v) for h_, v in zip(hdr[4:], c)))


if __name__ == "__main__":
    main()
