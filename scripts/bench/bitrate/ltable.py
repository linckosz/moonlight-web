"""Markdown table of the latency runs (runs/L): per bitrate, each pass and their mean.

    py -E -X utf8 ltable.py runs/L 1920x1080p60
"""
import glob
import json
import os
import sys


def main():
    root, combo = sys.argv[1], sys.argv[2]
    by = {}
    for p in sorted(glob.glob(os.path.join(root, combo + "-*-p*.json"))):
        d = json.load(open(p, encoding="utf-8"))
        by.setdefault(d["kbps"], []).append(d)
    print("| Set (Mbit/s) | Passes | Shown: median (ms) | Shown: p99 (ms) | Decoded: median (ms) | Host capture: median (ms) |")
    print("|---:|---:|---:|---:|---:|---:|")
    for k in sorted(by):
        ds = by[k]
        m = lambda f: sum(f(d) for d in ds) / len(ds)  # noqa: E731
        each = lambda f: " / ".join("%.1f" % f(d) for d in ds)  # noqa: E731
        print("| %g | %d | %.1f (%s) | %.1f | %.1f | %.1f |" % (
            k / 1000, len(ds), m(lambda d: d["shown"]["medianMs"]), each(lambda d: d["shown"]["medianMs"]),
            m(lambda d: d["shown"]["p99Ms"]), m(lambda d: d["medianMs"]), m(lambda d: d["capture"]["medianMs"])))


if __name__ == "__main__":
    main()
