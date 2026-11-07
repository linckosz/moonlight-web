"""Quality-vs-bitrate curves for one combination, small multiples (one y-axis per panel).

    plotenv\\Scripts\\python.exe plot.py --title "1080p60 HEVC" --out curves.png \
        --auto 20 "NVENC (RTX)=runs/A/1920x1080p60-hevc-*" "AMF (780M)=runs/B/1920x1080p60-hevc-*"

Rows = metrics, columns = scenes. x = bitrate (Mbit/s, log). Each series is one run per
bitrate (its summary.json). --auto draws today's Auto value as a recessive dashed line.
"""
import argparse
import glob
import json
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.ticker import FixedLocator, NullFormatter, NullLocator  # noqa: E402

SERIES = ["#2a78d6", "#eb6834", "#1baf7a"]
MARK = ["o", "s", "D"]
SURF, INK, INK2, MUTED, GRID = "#fcfcfb", "#0b0b0b", "#52514e", "#8a8984", "#e4e3df"

# (column title, scene id, summary key prefix chooser)
COLS = [("Junk Shop : 1re image", "1", "arrival"),
        ("Junk Shop : image posée", "1", "settled"),
        ("Google qui défile", "2", "mean"),
        ("Google : défile 0,5 s, s'arrête 0,5 s", "3", "mean")]
COLS_MAIN = [("Junk Shop : 1re image", "1", "arrival"),
             ("Junk Shop : image posée", "1", "settled"),
             ("Gears : image posée", "4", "settled"),
             ("Google qui défile", "2", "mean"),
             ("Google : défile 0,5 s, s'arrête 0,5 s", "3", "mean"),
             ("Page web qui défile", "5", "mean")]
ROWS = [("VMAF NEG", "negnm"), ("PSNR-Y (dB)", "psnr_y"), ("MS-SSIM", "ms_ssim")]
EN = {"Junk Shop : 1re image": "Junk Shop: first frame", "Junk Shop : image posée": "Junk Shop: settled",
      "Google qui défile": "Search page scrolling", "Google : défile 0,5 s, s'arrête 0,5 s": "Search page: scroll 0.5 s, stop 0.5 s",
      "Gears : image posée": "Gears: settled", "Page web qui défile": "Web page scrolling",
      "Auto actuel": "Auto today", "débit (Mbit/s)": "bitrate (Mbit/s)",
      "débit utilisé par la scène (Mbit/s)": "bitrate the scene used (Mbit/s)",
      "foot": "Raw picture (no shader), decoded by ffmpeg from the exact stream the client received. "
              "VMAF NEG with motion forced to 0 (the published model saturates at 100 on scrolling)."}
LANG = ["fr"]


def T(s):
    return EN.get(s, s) if LANG[0] == "en" else s


def value(d, metric, kind):
    return d.get((metric, kind))


def scenes_from_csv(path):
    """Per scene: arrival = first scored frame, settled = mean of the last 30, mean = all."""
    import csv
    by = {}
    for row in csv.DictReader(open(path, encoding="utf-8")):
        by.setdefault(row["scene"], []).append(row)
    out = {}
    for sid, rows in by.items():
        rows.sort(key=lambda r: int(r["n"]))
        d = {}
        for m, col in (("negnm", "vmaf_negnm"), ("psnr_y", "psnr_y"), ("ms_ssim", "ms_ssim")):
            v = [float(r[col]) for r in rows]
            d[(m, "arrival")] = v[0]
            d[(m, "settled")] = sum(v[-30:]) / len(v[-30:])
            d[(m, "mean")] = sum(v) / len(v)
        out[sid] = d
    return out


def load(pattern):
    pts = []
    for run in sorted(glob.glob(pattern)):
        sp, fc = os.path.join(run, "run.json"), os.path.join(run, "frames.csv")
        if not (os.path.exists(sp) and os.path.exists(fc)):
            continue
        spec = json.load(open(sp, encoding="utf-8"))
        sc = scenes_from_csv(fc)
        # bitrate each scene actually used: its frames' bytes over their duration
        # (decoded frame k = chunk k, one frame per chunk)
        import csv
        ch = json.load(open(os.path.join(run, "meta.json"), encoding="utf-8"))["chunks"]
        decs = {}
        for row in csv.DictReader(open(fc, encoding="utf-8")):
            decs.setdefault(row["scene"], []).append(int(row["dec"]))
        for sid, ds in decs.items():
            lo_, hi_ = min(ds), max(ds)
            sc[sid]["used"] = sum(c["n"] for c in ch[lo_:hi_ + 1]) * 8 / ((hi_ - lo_ + 1) / spec["fps"]) / 1e6
        pts.append((spec["kbps"] / 1000.0, sc))
    return sorted(pts, key=lambda p: p[0])


XMODE = ["set"]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--title", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--auto", type=float, default=0)
    ap.add_argument("--lang", default="fr")
    ap.add_argument("--x", default="set", choices=["set", "used"])
    ap.add_argument("--plan", default="phase1", choices=["phase1", "main"])
    ap.add_argument("series", nargs="+")
    a = ap.parse_args()
    LANG[0] = a.lang
    XMODE[0] = a.x
    if a.x == "used":
        a.auto = 0
    data = []
    for s in a.series:
        label, pat = s.split("=", 1)
        data.append((label, load(pat)))
    plt.rcParams.update({"font.family": "Segoe UI", "font.size": 10, "axes.edgecolor": GRID,
                         "axes.labelcolor": INK2, "xtick.color": INK2, "ytick.color": INK2,
                         "text.color": INK, "figure.facecolor": SURF, "axes.facecolor": SURF})
    base = COLS_MAIN if a.plan == "main" else COLS
    cols = base if XMODE[0] == "set" else [c for c in base if c[2] == "mean"]
    fig, axs = plt.subplots(len(ROWS), len(cols), figsize=(3.6 * len(cols) + 0.6 if len(cols) > 2 else 9, 9.5), squeeze=False, sharex=True)
    allx = sorted({x for _, pts in data for x, _ in pts})
    if XMODE[0] == "used":
        us = [d["used"] for _, pts in data for _, sc in pts for d in sc.values() if "used" in d]
        allx = [t for t in (0.3, 0.5, 1, 2, 3, 5, 8, 12, 20, 30, 50, 80) if min(us) / 1.3 <= t <= max(us) * 1.3]
    lo, hi = min(allx + ([a.auto] if a.auto else [])), max(allx + ([a.auto] if a.auto else []))
    for r, (rlabel, metric) in enumerate(ROWS):
        for c, (ctitle, sid, kind) in enumerate(cols):
            ax = axs[r][c]
            ax.set_xscale("log")
            ax.grid(True, color=GRID, linewidth=0.8)
            ax.set_axisbelow(True)
            for sp in ("top", "right"):
                ax.spines[sp].set_visible(False)
            if a.auto:
                ax.axvline(a.auto, color=MUTED, linewidth=1, linestyle=(0, (4, 3)))
                if r == len(ROWS) - 1:
                    ax.annotate(T("Auto actuel"), (a.auto, 0), xycoords=("data", "axes fraction"),
                                xytext=(3, 4), textcoords="offset points", color=INK2, fontsize=8)
            drawn = False
            ends = []
            for k, (label, pts) in enumerate(data):
                xs, ys = [], []
                for x, sc in pts:
                    v = value(sc.get(sid, {}), metric, kind)
                    if v is not None:
                        xs.append(x if XMODE[0] == "set" else sc[sid]["used"])
                        ys.append(v)
                if not xs:
                    continue
                drawn = True
                ax.plot(xs, ys, color=SERIES[k], linewidth=2, marker=MARK[k], markersize=6,
                        markeredgecolor=SURF, markeredgewidth=1.5, label=label, solid_capstyle="round")
                ends.append((ys[-1], xs[-1], label.split(" ")[0]))
            if len(data) > 1 and ends:  # direct labels at the lines' ends, pushed apart when close
                lo_y, hi_y = ax.get_ylim()
                gap = (hi_y - lo_y) * 0.07
                ends.sort()
                placed = []
                for y, x, lab in ends:
                    ty = y if not placed or y - placed[-1] >= gap else placed[-1] + gap
                    placed.append(ty)
                    ax.annotate(lab, (x, y), xytext=(x, ty), textcoords="data", va="center", fontsize=8,
                                color=INK2, ha="left", xycoords="data",
                                arrowprops=None).set_position((x * 1.04, ty))
            if not drawn:
                ax.text(0.5, 0.5, "VMAF seul" if kind == "arrival" else "—", transform=ax.transAxes,
                        ha="center", va="center", color=MUTED)
                ax.set_yticks([])
            ax.set_xlim(lo / 1.25, hi * 1.25)
            ax.xaxis.set_major_locator(FixedLocator(allx or [1]))
            ax.xaxis.set_minor_locator(NullLocator())
            ax.set_xticklabels([("%g" % x) for x in (allx or [1])], fontsize=8)
            if r == 0:
                ax.set_title(T(ctitle), fontsize=10.5, color=INK, loc="left")
            if c == 0:
                ax.set_ylabel(rlabel)
            if r == len(ROWS) - 1:
                ax.set_xlabel(T("débit (Mbit/s)") if XMODE[0] == "set" else T("débit utilisé par la scène (Mbit/s)"))
            else:
                ax.xaxis.set_major_formatter(NullFormatter())
    handles, labels = axs[0][0].get_legend_handles_labels()
    if not handles:
        for row in axs:
            for ax in row:
                handles, labels = ax.get_legend_handles_labels()
                if handles:
                    break
            if handles:
                break
    fig.suptitle(a.title, x=0.01, ha="left", fontsize=14, color=INK)
    if len(data) > 1 and handles:
        fig.legend(handles, labels, loc="upper right", ncol=len(handles), frameon=False)
    fig.text(0.01, 0.005, EN["foot"] if LANG[0] == "en" else "Image brute (sans shader), décodée par ffmpeg depuis le flux exact reçu par le client. "
             "VMAF NEG avec le mouvement à 0 (le modèle publié sature à 100 sur un défilement).",
             fontsize=8, color=MUTED)
    fig.tight_layout(rect=(0, 0.02, 1, 0.95))
    fig.savefig(a.out, dpi=110)
    print("wrote", a.out)


if __name__ == "__main__":
    main()
