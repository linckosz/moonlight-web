"""The HDR factor with highlights (night of 11->12/11): runs/H (HDR, highlights at 3x the SDR
white) against runs/HS (the same renders in SDR 4:2:0), both scored with the same --cut-cols.
need(target) = set bitrate where a scene's VMAF NEG mean crosses target (log-x interpolation,
as factors.py). Factor = need(HDR) / need(SDR), scene by scene. Then the guard: repeated and
missing frames per run, against runs/F (HDR, 07/11, no background load).
    py -E -X utf8 hfactor.py"""
import glob
import json
import math
import os

HERE = os.path.dirname(os.path.abspath(__file__))
RUNS = os.path.join(HERE, "runs")


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


def curve(pattern):
    pts = []
    for r in sorted(glob.glob(os.path.join(RUNS, pattern))):
        s = os.path.join(r, "summary.json")
        if not os.path.exists(s):
            continue
        j = json.load(open(s, encoding="utf-8"))
        pts.append((int(r.rsplit("-", 1)[1]) / 1000, j, os.path.getsize(os.path.join(r, "bitstream.bin"))))
    return sorted(pts, key=lambda p: p[0])


def fmt(n):
    if n is None:
        return "   -  "
    op, v = n
    return "%s%5.1f" % (op or " ", v)


hdr, sdr = curve("H/1920x1080p60-hevc-hdr-*"), curve("HS/1920x1080p60-hevc-[0-9]*")
old = curve("F/1920x1080p60-hevc-hdr-*")
cuts = {json.dumps(j.get("cut_cols")) for _, j, _ in hdr + sdr}
print("points: HDR %d, SDR %d; cut_cols %s" % (len(hdr), len(sdr), " / ".join(sorted(cuts))))
if len(cuts) != 1:
    raise SystemExit("runs scored with different cuts: not comparable")
SCENES = ["2", "3", "1", "4"]  # 2/3 scroll (3 with pauses), 1/4 still
for target in (93, 95):
    print("\n== VMAF NEG mean >= %d : need (Mbit/s set) and factor HDR / SDR" % target)
    print("%-12s" % "" + "".join("   scene %s      " % s for s in SCENES))
    for name, pts in (("SDR", sdr), ("HDR 3x", hdr)):
        cells = []
        for s in SCENES:
            n = need([(x, j["scenes"][s]["negnm_mean"]) for x, j, _ in pts if s in j["scenes"]], target)
            nb = need([(x, j["scenes"][s]["negnm_mean"]) for x, j, _ in sdr if s in j["scenes"]], target)
            f = "x%.2f" % (n[1] / nb[1]) if n and nb and n[0] == "" and nb[0] == "" else ""
            cells.append("%s %-7s" % (fmt(n), f))
        print("%-12s" % name + "  ".join(cells))

print("\n== scene 2 per bitrate: VMAF NEG (negnm) / PSNR-Y / dE ITP (HDR) ; bytes recorded")
for name, pts in (("SDR", sdr), ("HDR 3x", hdr)):
    print(name)
    for x, j, size in pts:
        s = j["scenes"].get("2", {})
        print("  %5.1f  negnm %6.2f  p5 %6.2f  psnr_y %6.2f  %s  %8d bytes" % (
            x, s.get("negnm_mean", float("nan")), s.get("negnm_p5", float("nan")), s.get("psnr_y_mean", float("nan")),
            ("itp %.3f" % s["itp_mean"]) if "itp_mean" in s else "         ", size))

print("\n== guard: repeated / missing frames (scene 2: frames scored vs n_max - first_n + 1)")
for name, pts in (("SDR", sdr), ("HDR 3x", hdr), ("F HDR 07/11", old)):
    rows = []
    for x, j, _ in pts:
        s = j["scenes"].get("2", {})
        miss = (s["n_max"] - s.get("first_n", 0) + 1 - s["frames"]) if s else None
        rows.append("%g:%d/%s" % (x, j.get("duplicates", -1), miss))
    print("%-12s dup/missing  %s" % (name, " ".join(rows)))
