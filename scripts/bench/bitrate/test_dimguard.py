"""The "dimmed screen" guard (driver.check_white -> common.bar_white / dimmed), 12/11.
1. synthetic frames: NV12 (limited) and RGBA (full range) through driver.grabbed_lumas, white
   kept, dimmed x0.54 like the night's prompt backdrop, and both sides of the threshold;
2. real streams, the frames the tap grabs (decoded #0, 100, 300 ... 1300): the 11 SDR points of
   runs/HS-dimmed (the dimmed runs, set aside) must all be refused; valid SDR points of runs/B and runs/M, lowest bitrates
   included (compression noise on the cells), must all pass.
    py -E -X utf8 test_dimguard.py"""
import glob
import json
import os
import shutil
import sys
import tempfile

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as cm  # noqa: E402
import driver  # noqa: E402

fails = []


def expect(name, cond, detail=""):
    print("%-4s %s %s" % ("ok" if cond else "FAIL", name, detail))
    if not cond:
        fails.append(name)


# ── 1. synthetic ─────────────────────────────────────────────────────────────
def page(w, h, v, white=255.0):
    """A grey page with its barcode, every value scaled by white/255 (a dimmed screen)."""
    rgb = np.full((h, w, 3), 128, np.uint8)
    cm.draw_bar(rgb, v)
    return np.clip(rgb.astype(np.float64) * white / 255.0 + 0.5, 0, 255).astype(np.uint8)


def fake_run(frames_rgb, fmt):
    """A run dir as pull() leaves it: meta.json + frame-NNNNNN.raw, NV12 or RGBA."""
    d = tempfile.mkdtemp(prefix="dimg-", dir=HERE)
    meta = {"frames": []}
    for idx, rgb in frames_rgb:
        h, w = rgb.shape[:2]
        if fmt == "NV12":
            y = (16 + 219 * (0.2126 * rgb[..., 0] + 0.7152 * rgb[..., 1] + 0.0722 * rgb[..., 2]) / 255 + 0.5).astype(np.uint8)
            buf = np.concatenate([y.ravel(), np.full(w * h // 2, 128, np.uint8)])
            layout = [{"offset": 0, "stride": w}, {"offset": w * h, "stride": w}]
        else:
            buf = np.concatenate([rgb, np.full((h, w, 1), 255, np.uint8)], axis=2).ravel()
            layout = [{"offset": 0, "stride": 4 * w}]
        buf.tofile(os.path.join(d, "frame-%06d.raw" % idx))
        meta["frames"].append({"idx": idx, "format": fmt, "vis": {"w": w, "h": h}, "layout": layout})
    json.dump(meta, open(os.path.join(d, "meta.json"), "w", encoding="utf-8"))
    return d


VALS = [0, 8225, (1 << 13) | 777, (2 << 13) | 8191]
for fmt in ("NV12", "RGBA"):
    for geo in ((1920, 1080), (1280, 720)):
        for label, white, refused in (("white kept", 255.0, False), ("dimmed x0.54", 255.0 * 0.54, True),
                                      ("slightly low, passes", 255.0 * 0.975, False)):
            d = fake_run([(i * 100, page(geo[0], geo[1], v, white)) for i, v in enumerate(VALS)], fmt)
            try:
                bars = driver.check_bars(d)
                why = driver.check_white(d)
            finally:
                shutil.rmtree(d)
            expect("synthetic %s %dx%d %s" % (fmt, geo[0], geo[1], label),
                   all(b is not None for _, b in bars) and (why is not None) == refused, why or "")
# the threshold itself, on a plain luma plane
for white_y, refused in ((226, False), (224, True)):
    y = np.full((1080, 1920), 126, np.uint8)
    rgb = page(1920, 1080, 8225)
    y[rgb[..., 1] == 255] = white_y
    y[rgb[..., 1] == 0] = 16
    bw = cm.bar_white(y)
    expect("threshold: white Y %d" % white_y, bw is not None and (cm.dimmed([bw]) is not None) == refused, "bar_white %.1f" % bw)
expect("unreadable barcode -> None", cm.bar_white(np.full((1080, 1920), 126, np.uint8)) is None)


# ── 2. real streams ──────────────────────────────────────────────────────────
GRAB = {0, 100, 300, 500, 700, 900, 1100, 1300}  # the decoded frames tap.js keeps


def whites_of(run):
    spec = json.load(open(os.path.join(run, "run.json"), encoding="utf-8"))
    w, h = spec["W"], spec["H"]
    stream, _ = cm.annexb(run)
    out = []
    it = cm.decode_iter(stream, w, h)
    try:
        for i, buf in it:
            if i in GRAB:
                out.append(cm.bar_white(np.asarray(buf[:w * h]).reshape(h, w)))
            if i >= max(GRAB):
                break
    finally:
        try:
            it.close()
        except BaseException:  # noqa: BLE001  ffmpeg's broken pipe when stopped early
            pass
    return out


bad = sorted(glob.glob(os.path.join(HERE, "runs", "HS-dimmed", "1920x1080p60-hevc-[0-9]*")))
good = [os.path.join(HERE, "runs", p) for p in ("B/1920x1080p60-hevc-01500", "B/1920x1080p60-hevc-30000",
                                               "M/1920x1080p60-h264-01500", "M/1280x720p30-hevc-03000",
                                               "M/1920x1080p60-hevc-06000")]
for run, refused in [(r, True) for r in bad] + [(r, False) for r in good]:
    ws = whites_of(run)
    low = cm.dimmed(ws)
    seen = [round(x) for x in ws if x is not None]
    expect("real %s" % os.path.relpath(run, os.path.join(HERE, "runs")),
           len(seen) >= 3 and (low is not None) == refused,
           "whites %s -> %s" % (seen, "refused" if low is not None else "kept"))

print("ALL OK" if not fails else "FAILED: %d" % len(fails))
sys.exit(1 if fails else 0)
