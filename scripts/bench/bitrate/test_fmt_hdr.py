"""Plumbing test of the HDR scoring (no host): frames of a real run's scroll scene, barcode
included, converted by fmtref.RefHdr, encoded by libx265 in 10 bits (CPU, small), then scored by
score.py as an "hdr" run. A near-lossless encode must score near the top, a coarse one lower.

    py -E -X utf8 test_fmt_hdr.py <src run dir> <out dir> <crf> [--nits 200] [--curve srgb]
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

src_run, out, crf = sys.argv[1], sys.argv[2], sys.argv[3]
nits = float(sys.argv[sys.argv.index("--nits") + 1]) if "--nits" in sys.argv else 200.0
curve = sys.argv[sys.argv.index("--curve") + 1] if "--curve" in sys.argv else "srgb"
spec = json.load(open(os.path.join(src_run, "run.json"), encoding="utf-8"))
w, h = spec["W"], spec["H"]
scene = [s for s in spec["scenes"] if s["id"] == 2][0]
src = cm.load_rgb(os.path.join(HERE, "content", scene["src"]))
os.makedirs(out, exist_ok=True)
enc = subprocess.Popen([cm.FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "yuv420p10le",
                        "-s", "%dx%d" % (w, h), "-r", "60", "-i", "pipe:0", "-c:v", "libx265", "-preset", "ultrafast",
                        "-x265-params", "crf=%s:log-level=error" % crf, "-pix_fmt", "yuv420p10le",
                        "-f", "hevc", os.path.join(out, "bitstream.bin")], stdin=subprocess.PIPE)
for n in range(90):
    rgb = cm.ref(scene, src, w, h, n)  # the page's frame, barcode in
    y, cb, cr = fr.RefHdr(rgb, nits, curve).frame(0, h, w)
    enc.stdin.write(y.tobytes() + cb.tobytes() + cr.tobytes())
enc.stdin.close()
assert enc.wait() == 0
json.dump({"configs": [], "chunks": []}, open(os.path.join(out, "meta.json"), "w"))
json.dump({"W": w, "H": h, "fps": 60, "kbps": 0, "codec": "hevc", "fmt": "hdr", "sdr_nits": nits, "sdr_curve": curve,
           "scenes": [scene]}, open(os.path.join(out, "run.json"), "w"))
r = subprocess.run([sys.executable, "-E", "-X", "utf8", os.path.join(HERE, "score.py"), out, "--threads", "4"],
                   capture_output=True, text=True)
if r.returncode:
    raise SystemExit(r.stderr[-1500:])
s = json.load(open(os.path.join(out, "summary.json"), encoding="utf-8"))
sc = s["scenes"]["2"]
print("crf %s: scored %d/%d  negnm %.2f  psnr_y %.2f  cb %.2f  cr %.2f  dE_ITP %.3f  (%d bytes)" % (
    crf, s["scored"], s["decoded"], sc["negnm_mean"], sc["psnr_y_mean"], sc["psnr_cb_mean"], sc["psnr_cr_mean"],
    sc["itp_mean"], os.path.getsize(os.path.join(out, "bitstream.bin"))))
