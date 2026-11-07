"""Markdown table of one combination for the wiki: one row per bitrate.

    py -E -X utf8 wikitable.py "runs/A/1920x1080p60-hevc-*"
"""
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from plot import scenes_from_csv  # noqa: E402


def used_mbps(run):
    m = json.load(open(os.path.join(run, "meta.json"), encoding="utf-8"))
    ch = m["chunks"]
    secs = (ch[-1]["ts"] - ch[0]["ts"]) / 1e6
    return sum(c["n"] for c in ch) * 8 / secs / 1e6 if secs > 0 else 0


def scroll_mbps(run, fps, scene="2"):
    """Bytes of the frames of one scene (decoded frame k = chunk k, one frame per chunk),
    over their duration at the stream's frame rate."""
    import csv
    m = json.load(open(os.path.join(run, "meta.json"), encoding="utf-8"))
    ch = m["chunks"]
    dec = [int(r["dec"]) for r in csv.DictReader(open(os.path.join(run, "frames.csv"), encoding="utf-8"))
           if r["scene"] == scene]
    if not dec:
        return None
    lo, hi = min(dec), max(dec)
    return sum(c["n"] for c in ch[lo:hi + 1]) * 8 / ((hi - lo + 1) / fps) / 1e6


def main_plan(pattern):
    """main bench: VMAF NEG per scene, plus the bitrate used while scrolling."""
    rows = []
    for run in sorted(glob.glob(pattern)):
        fc = os.path.join(run, "frames.csv")
        if not os.path.exists(fc):
            continue
        spec = json.load(open(os.path.join(run, "run.json"), encoding="utf-8"))
        sc = scenes_from_csv(fc)
        g = lambda s, k: sc.get(s, {}).get(("negnm", k))  # noqa: E731
        rows.append((spec["kbps"] / 1000, scroll_mbps(run, spec["fps"]), g("1", "arrival"), g("1", "settled"),
                     g("4", "settled"), g("6", "settled"), g("2", "mean"), g("3", "mean"), g("5", "mean"),
                     sc.get("2", {}).get(("psnr_y", "mean"))))
    rows.sort()
    cp = any(r[5] is not None for r in rows)
    hdr = ["Set (Mbit/s)", "Used while scrolling (Mbit/s)", "Junk Shop: first frame", "Junk Shop: settled",
           "Gears: settled"] + (["4K photo: settled"] if cp else []) + [
           "Search page scroll", "Scroll/stop", "Web page scroll", "Search page scroll PSNR-Y"]
    print("VMAF NEG unless stated.")
    print()
    print("| " + " | ".join(hdr) + " |")
    print("|" + "---:|" * len(hdr))
    f = lambda v, d=1: "–" if v is None else ("%." + str(d) + "f") % v  # noqa: E731
    for r in rows:
        cells = ["%g" % r[0], f(r[1], 2), f(r[2]), f(r[3]), f(r[4])] + ([f(r[5])] if cp else []) + [
            f(r[6]), f(r[7]), f(r[8]), f(r[9], 2)]
        print("| " + " | ".join(cells) + " |")


def main():
    if "--main" in sys.argv:
        sys.argv.remove("--main")
        return main_plan(sys.argv[1])
    rows = []
    for run in sorted(glob.glob(sys.argv[1])):
        fc = os.path.join(run, "frames.csv")
        if not os.path.exists(fc):
            continue
        spec = json.load(open(os.path.join(run, "run.json"), encoding="utf-8"))
        sc = scenes_from_csv(fc)
        g = lambda s, m, k: sc.get(s, {}).get((m, k))  # noqa: E731
        rows.append((spec["kbps"] / 1000, scroll_mbps(run, spec["fps"]),
                     g("1", "negnm", "arrival"), g("1", "negnm", "settled"), g("1", "psnr_y", "settled"),
                     g("2", "negnm", "mean"), g("2", "psnr_y", "mean"), g("2", "ms_ssim", "mean"),
                     g("3", "negnm", "mean")))
    rows.sort()
    print("| Set (Mbit/s) | Used while scrolling (Mbit/s) | Still: first frame VMAF | Still: settled VMAF | Still: settled PSNR-Y | "
          "Scroll VMAF | Scroll PSNR-Y | Scroll MS-SSIM | Scroll/stop VMAF |")
    print("|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    f = lambda v, d=1: "–" if v is None else ("%." + str(d) + "f") % v  # noqa: E731
    for r in rows:
        print("| %s | %s | %s | %s | %s | %s | %s | %s | %s |" % (
            ("%g" % r[0]), f(r[1], 2), f(r[2]), f(r[3]), f(r[4], 2), f(r[5]), f(r[6], 2), f(r[7], 4), f(r[8])))


if __name__ == "__main__":
    main()
