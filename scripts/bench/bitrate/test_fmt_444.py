"""Plumbing test of the 4:4:4 scoring on the color444 plan's coloured code (no host): the page's
frames of scene 5 (barcode in), converted by fmtref.Ref444, encoded by libx265 in 4:4:4 and in
4:2:0 (chroma halved by ffmpeg), both scored by score.py against the full-chroma reference (the
4:2:0 one with --as 444, as a 4:2:0 run of the bench is). At one crf, 4:4:4 must keep more chroma
(PSNR-Cb/Cr, CIEDE2000) than 4:2:0; a near-lossless encode must score near the top.

    py -E -X utf8 test_fmt_444.py <out dir> <crf> [<crf> ...] [--geo 1920x1080]
"""
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as cm  # noqa: E402
import driver  # noqa: E402
import fmtref as fr  # noqa: E402

args = [a for a in sys.argv[1:]]
geo = args[args.index("--geo") + 1] if "--geo" in args else "1920x1080"
if "--geo" in args:
    i = args.index("--geo")
    del args[i:i + 2]
out_root, crfs = args[0], args[1:]
w, h = (int(v) for v in geo.split("x"))
scenes, _ = driver.plan("color444", geo, 60, h)
scene = [s for s in scenes if s["id"] == 5][0]
src = cm.load_rgb(os.path.join(HERE, "content", scene["src"]))
N = 90
print("code.png", src.shape, "scene", scene)

for crf in crfs:
    for fmt in ("444", "420"):
        out = os.path.join(out_root, "%s-crf%s" % (fmt, crf))
        os.makedirs(out, exist_ok=True)
        enc = subprocess.Popen([cm.FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo",
                                "-pix_fmt", "yuv444p", "-s", "%dx%d" % (w, h), "-r", "60", "-i", "pipe:0",
                                "-c:v", "libx265", "-preset", "ultrafast",
                                "-x265-params", "crf=%s:log-level=error" % crf,
                                "-pix_fmt", "yuv444p" if fmt == "444" else "yuv420p",
                                "-f", "hevc", os.path.join(out, "bitstream.bin")], stdin=subprocess.PIPE)
        for n in range(N):
            rgb = cm.ref(scene, src, w, h, n)  # the page's frame, barcode in
            y, cb, cr = fr.Ref444(rgb).frame(0, h, w)
            enc.stdin.write(y.tobytes() + cb.tobytes() + cr.tobytes())
        enc.stdin.close()
        assert enc.wait() == 0
        json.dump({"configs": [], "chunks": []}, open(os.path.join(out, "meta.json"), "w"))
        json.dump({"W": w, "H": h, "fps": 60, "kbps": 0, "codec": "hevc", "fmt": fmt, "scenes": [scene]},
                  open(os.path.join(out, "run.json"), "w"))
        cmd = [sys.executable, "-E", "-X", "utf8", os.path.join(HERE, "score.py"), out, "--threads", "4"]
        if fmt == "420":
            cmd += ["--as", "444"]
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode:
            raise SystemExit(r.stderr[-1500:])
        summ = "summary-444.json" if fmt == "420" else "summary.json"
        s = json.load(open(os.path.join(out, summ), encoding="utf-8"))
        sc = s["scenes"]["5"]
        print("%s crf %s: scored %d/%d  negnm %.2f  psnr_y %.2f  cb %.2f  cr %.2f  ciede %.2f  (%d bytes)" % (
            fmt, crf, s["scored"], s["decoded"], sc["negnm_mean"], sc["psnr_y_mean"], sc["psnr_cb_mean"],
            sc["psnr_cr_mean"], sc["ciede_mean"], os.path.getsize(os.path.join(out, "bitstream.bin"))), flush=True)
