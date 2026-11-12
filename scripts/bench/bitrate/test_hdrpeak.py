"""Plumbing test of the "hdrpeak" plan (no host, no GPU): what the HDR canvas of bench.html
would hand the compositor — the calibration bands (scene 7), then the two renders scrolling with
their highlights at k x SDR white (scene 2), barcode at 1.0 — converted like fmtref.RefHdr,
optionally clipped at `--clip` x SDR white (a display's headroom), encoded by libx265 in 10
bits on the CPU, then scored by score.py and read by hdrcal.py.

    py test_hdrpeak.py <out dir> <crf> [--k 3] [--nits 240] [--clip 0]
"""
import json
import os
import subprocess
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as cm  # noqa: E402
import fmtref as fr  # noqa: E402


def opt(name, default):
    return float(sys.argv[sys.argv.index(name) + 1]) if name in sys.argv else default


out, crf = sys.argv[1], sys.argv[2]
k, nits, clip = opt("--k", 3.0), opt("--nits", 240.0), opt("--clip", 0.0)
w, h, step = 1920, 1080, 9
C = cm.cell(h)
rows = h - 2 * C
BANDS = [0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0]
hp = {"k": k, "t": 0.6}
calib = {"id": 7, "mode": "calib", "src": "src/1920x1080/junkshop.png", "bands": BANDS}
scroll = {"id": 2, "mode": "scroll", "src": "src/1920x1080/jsgears.png", "step": step, "pause": 0, "hdr": hp}
src = cm.load_rgb(os.path.join(HERE, "content", scroll["src"]))


def planes_from_light(light):
    """fmtref.RefHdr's planes from light (1.0 = SDR white) instead of sRGB bytes."""
    if clip > 0:
        light = np.minimum(light, np.float32(clip))
    r = object.__new__(fr.RefHdr)
    r.lin = ((light * np.float32(nits / 80.0)).astype(np.float16).astype(np.float32) @ fr.M709_2020.T).astype(np.float32)
    r.y = fr.unorm(fr.pq(r.lin) @ fr.K2020, 876.0, 64.0).astype(np.uint16)
    return r.frame(0, h, w)


def frame(scene, n):
    """The canvas's light for one frame: picture (boosted, or bands), then the barcode at 1.0 / 0."""
    rgb = cm.ref(scene if scene["mode"] != "calib" else {**scene, "mode": "still"}, src, w, h, n)
    light = fr.srgb_light(rgb, "srgb")  # barcode rows: 1.0 and 0.0
    if scene["mode"] == "calib":
        bh = rows / len(BANDS)
        for j, m in enumerate(BANDS):
            light[int(round(j * bh)):int(round((j + 1) * bh))] = m
    else:
        light[:rows] = fr.boost_light(light[:rows], k, 0.6)
    return planes_from_light(light)


os.makedirs(out, exist_ok=True)
enc = subprocess.Popen([cm.FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p10le",
                        "-s", "%dx%d" % (w, h), "-r", "60", "-i", "pipe:0", "-c:v", "libx265", "-preset", "ultrafast",
                        "-x265-params", "crf=%s:log-level=error" % crf, "-pix_fmt", "yuv420p10le",
                        "-f", "hevc", os.path.join(out, "bitstream.bin")], stdin=subprocess.PIPE)
for scene, count in ((calib, 30), (scroll, 120)):
    for n in range(count):
        y, cb, cr = frame(scene, n)
        enc.stdin.write(y.tobytes() + cb.tobytes() + cr.tobytes())
enc.stdin.close()
assert enc.wait() == 0
json.dump({"configs": [], "chunks": []}, open(os.path.join(out, "meta.json"), "w"))
json.dump({"W": w, "H": h, "fps": 60, "kbps": 0, "codec": "hevc", "fmt": "hdr", "sdr_nits": nits,
           "scenes": [calib, scroll]}, open(os.path.join(out, "run.json"), "w"))
for script in ("score.py", "hdrcal.py"):
    args = [sys.executable, "-E", "-X", "utf8", os.path.join(HERE, script), out]
    r = subprocess.run(args + (["--threads", "4"] if script == "score.py" else []), capture_output=True, text=True)
    if r.returncode:
        raise SystemExit(script + ": " + r.stderr[-1500:])
    if script == "hdrcal.py":
        print(r.stdout.strip())
s = json.load(open(os.path.join(out, "summary.json"), encoding="utf-8"))
sc = s["scenes"]["2"]
print("crf %s k %g clip %g: scored %d/%d (grey %d)  negnm %.2f  psnr_y %.2f  dE_ITP %.3f  (%d bytes)" % (
    crf, k, clip, s["scored"], s["decoded"], s["grey"], sc["negnm_mean"], sc["psnr_y_mean"], sc["itp_mean"],
    os.path.getsize(os.path.join(out, "bitstream.bin"))))
