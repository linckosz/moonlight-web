"""The 4:4:4 factor (12/11 slot, plan color444): runs/C 4:4:4 (summary.json, scored on full chroma)
against runs/C 4:2:0 (summary-444.json, score.py --as 444: the same full-chroma reference).
need(target) = set bitrate where a scene's VMAF NEG mean crosses target (log-x interpolation, as
factors.py and hfactor.py). Factor = need(4:4:4) / need(4:2:0), scene by scene. Then the colour at
equal set bitrate: PSNR-Cb / PSNR-Cr / CIEDE2000 (the bitrate really used: export_viz3.run_point).
    py -E -X utf8 cfactor.py"""
import glob
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
import sys
FAST = "--runs" in sys.argv and sys.argv[sys.argv.index("--runs") + 1] == "E"
RUNS = os.path.join(HERE, "runs", "E" if FAST else "C")
if FAST:  # 13/11 plan fast444: content that fills the bitrate
    SCENES = ["2", "3", "5"]
    NAMES = {"2": "fast pan 48px/f", "3": "jumps 234px/f", "5": "code 48px/f"}
else:
    SCENES = ["2", "3", "5", "1", "4"]  # 2/3 renders scrolling (3 with pauses), 5 coloured code, 1/4 still
    NAMES = {"1": "Junk Shop still", "2": "renders scroll", "3": "renders scroll+pauses", "4": "Gears still",
             "5": "coloured code scroll"}


def need(pts, target):
    prev = None
    for x, v in pts:
        if v >= target:
            if prev is None:
                return ("<=", x)
            x0, v0 = prev
            t = (target - v0) / (v - v0)
            return ("", math.exp(math.log(x0) + t * (math.log(x) - math.log(x0))))
        prev = (x, v)
    return (">", pts[-1][0]) if pts else None


def curve(pattern, summ):
    pts = []
    for r in sorted(glob.glob(os.path.join(RUNS, pattern))):
        s = os.path.join(r, summ)
        if not os.path.exists(s):
            continue
        pts.append((int(r.rsplit("-", 1)[1]) / 1000, json.load(open(s, encoding="utf-8"))))
    return sorted(pts, key=lambda p: p[0])


def fmt(n):
    if n is None:
        return "   -  "
    op, v = n
    return "%s%5.1f" % (op or " ", v)


def bd_factor(a, b, steps=200):
    """Mean set-bitrate ratio a/b at equal quality, over the quality both curves cover
    (Bjontegaard's idea, piecewise linear: log-rate as a function of the metric). a, b: [(mbps, q)].
    The metric is made non-decreasing first (cumulative max: a noisy dip is the curve's floor, not
    a cheaper point). None when the ranges do not overlap. Returns (factor, q_low, q_high)."""
    def prep(pts):
        out, best = [], -1e9
        for x, q in sorted(pts):
            best = max(best, q)
            if not out or best > out[-1][1]:
                out.append((math.log(x), best))
        return out

    def lograte(c, q):
        for (x0, q0), (x1, q1) in zip(c, c[1:]):
            if q0 <= q <= q1:
                return x0 + (q - q0) / (q1 - q0) * (x1 - x0) if q1 > q0 else x0
        return None

    ca, cb = prep(a), prep(b)
    if len(ca) < 2 or len(cb) < 2:
        return None
    lo, hi = max(ca[0][1], cb[0][1]), min(ca[-1][1], cb[-1][1])
    if hi <= lo:
        return None
    diffs = []
    for i in range(steps + 1):
        q = lo + (hi - lo) * i / steps
        ra, rb = lograte(ca, q), lograte(cb, q)
        if ra is not None and rb is not None:
            diffs.append(ra - rb)
    return (math.exp(sum(diffs) / len(diffs)), lo, hi) if diffs else None


full = curve("1920x1080p60-hevc-444-*", "summary.json")
sub = curve("1920x1080p60-hevc-[0-9]*", "summary-444.json")
print("points: 4:4:4 %d, 4:2:0 %d" % (len(full), len(sub)))

# VMAF NEG reads luma only (12/11 test_fmt_444: lossless 4:2:0 = 4:4:4 at 96.8 while PSNR-Cb/Cr
# lose 14-17 dB): its factor answers "how much more for 4:4:4 not to lose on luma"; the chroma
# metrics' factor answers the other way round, what 4:4:4's colour costs at equal colour.
print("\n== mean factor 4:4:4 / 4:2:0 at equal quality, over the range both reach (Bjontegaard, linear)")
for key, label in (("negnm_mean", "VMAF NEG"), ("psnr_y_mean", "PSNR-Y"), ("psnr_cb_mean", "PSNR-Cb"),
                   ("psnr_cr_mean", "PSNR-Cr"), ("ciede_mean", "CIEDE2000")):
    cells = []
    for s in SCENES:
        a = [(x, j["scenes"][s][key]) for x, j in full if s in j["scenes"] and key in j["scenes"][s]]
        b = [(x, j["scenes"][s][key]) for x, j in sub if s in j["scenes"] and key in j["scenes"][s]]
        r = bd_factor(a, b)
        cells.append("  %s %-12s" % (s, "x%.2f [%.1f-%.1f]" % r if r else "-"))
    print("%-10s" % label + "".join("%-26s" % c for c in cells))
for target in (93, 95):
    print("\n== VMAF NEG mean >= %d : need (Mbit/s set) and factor 4:4:4 / 4:2:0" % target)
    print("%-8s" % "" + "".join("  %-22s" % ("%s %s" % (s, NAMES[s][:18])) for s in SCENES))
    for name, pts in (("4:2:0", sub), ("4:4:4", full)):
        cells = []
        for s in SCENES:
            n = need([(x, j["scenes"][s]["negnm_mean"]) for x, j in pts if s in j["scenes"]], target)
            nb = need([(x, j["scenes"][s]["negnm_mean"]) for x, j in sub if s in j["scenes"]], target)
            f = "x%.2f" % (n[1] / nb[1]) if n and nb and n[0] == "" and nb[0] == "" else ""
            cells.append("  %s %-15s" % (fmt(n), f))
        print("%-8s" % name + "".join(cells))

for s in SCENES:
    print("\n== scene %s (%s) per set bitrate: VMAF NEG / PSNR-Y / PSNR-Cb / PSNR-Cr / CIEDE2000" % (s, NAMES[s]))
    for name, pts in (("4:2:0", sub), ("4:4:4", full)):
        for x, j in pts:
            d = j["scenes"].get(s)
            if not d:
                continue
            print("  %-5s %5.1f  negnm %6.2f  psnr_y %6.2f  cb %6.2f  cr %6.2f  ciede %6.2f" % (
                name, x, d["negnm_mean"], d["psnr_y_mean"], d.get("psnr_cb_mean", float("nan")),
                d.get("psnr_cr_mean", float("nan")), d.get("ciede_mean", float("nan"))))

print("\n== guard: scored / decoded, duplicates, unreadable")
for name, pts in (("4:2:0", sub), ("4:4:4", full)):
    print("%-6s %s" % (name, " ".join("%g:%d/%d d%d u%d" % (x, j["scored"], j["decoded"], j.get("duplicates", -1),
                                                             j.get("unreadable", -1)) for x, j in pts)))
