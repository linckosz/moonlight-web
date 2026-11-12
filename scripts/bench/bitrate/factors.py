"""H.264, 4:4:4 and HDR factors against HEVC 4:2:0 SDR, same bench, same scenes.
need(target) = set bitrate where a scene's VMAF NEG mean crosses target (log-x
interpolation, synth.need). Factor = need(variant) / need(HEVC 4:2:0)."""
import glob, json, math, os, sys
HERE = os.path.dirname(os.path.abspath(__file__))


def need(pts, target):
    """synth.need, copied (synth imports matplotlib): smallest set bitrate where
    the curve crosses target, log-x interpolation."""
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

RUNS = os.path.join(HERE, "runs")


def curve(pattern, summary="summary.json"):
    pts = []
    for r in sorted(glob.glob(os.path.join(RUNS, pattern))):
        s = os.path.join(r, summary)
        if not os.path.exists(s):
            continue
        kbps = int(r.rsplit("-", 1)[1])
        pts.append((kbps / 1000, json.load(open(s, encoding="utf-8"))["scenes"]))
    return sorted(pts)


def needs(pts, scene, metric, target):
    return need([(x, sc[scene][metric]) for x, sc in pts if scene in sc and metric in sc[scene]], target)


def fmt(n):
    if n is None:
        return "   -  "
    op, v = n
    return "%s%5.1f" % (op or " ", v)


base = {
    "1080": curve("M/1920x1080p60-hevc-[0-9]*"),
    "720": curve("M/1280x720p60-hevc-[0-9]*"),
}
variants = [
    ("H.264 1080p60", "1080", curve("M/1920x1080p60-h264-*")),
    ("H.264 720p60", "720", curve("M/1280x720p60-h264-*")),
    ("HEVC 4:4:4 1080p60", "1080", curve("F/1920x1080p60-hevc-444-*")),
    ("HEVC HDR 1080p60", "1080", curve("F/1920x1080p60-hevc-hdr-*")),
]
print("points:", {k: len(v) for k, v in base.items()}, {n: len(p) for n, _, p in variants})
SCENES = ["2", "3", "5", "1", "4"]  # 2 = the search page the rule uses; 1/4 still, 2/3/5 scroll
for target in (93, 95):
    print("\n== VMAF NEG mean >= %d : need (Mbps) and factor vs HEVC 4:2:0 SDR" % target)
    print("%-20s" % "" + "".join("   scene %s      " % s for s in SCENES))
    for name, b, pts in [("HEVC 1080p60 (base)", "1080", base["1080"]), ("HEVC 720p60 (base)", "720", base["720"])] + variants:
        cells = []
        for s in SCENES:
            n = needs(pts, s, "negnm_mean", target)
            nb = needs(base[b], s, "negnm_mean", target)
            f = ""
            if n and nb and n[0] == "" and nb[0] == "":
                f = "x%.2f" % (n[1] / nb[1])
            cells.append("%s %-7s" % (fmt(n), f))
        print("%-20s" % name + "  ".join(cells))

# Same-bitrate comparison of the colour metrics (scene 2), to read 4:4:4 and HDR.
print("\n== scene 2 per bitrate: VMAF NEG / PSNR-Y / PSNR-Cb / PSNR-Cr / extra")
for name, pts in [("HEVC 4:2:0 1080p60", base["1080"]), ("HEVC 4:4:4", variants[2][2]), ("HEVC HDR", variants[3][2]),
                  ("HEVC 4:2:0 scored as 444", curve("M/1920x1080p60-hevc-[0-9]*", "summary-444.json"))]:
    print(name)
    for x, sc in pts:
        s = sc.get("2", {})
        extra = " ".join("%s %.2f" % (k, s[k]) for k in ("ciede_mean", "itp_mean") if k in s)
        print("  %5.1f  %6.2f  %5.2f  %5.2f  %5.2f  %s" % (x, s.get("negnm_mean", float("nan")), s.get("psnr_y_mean", float("nan")),
                                                      s.get("psnr_cb_mean", float("nan")), s.get("psnr_cr_mean", float("nan")), extra))
