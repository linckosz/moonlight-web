"""Offline check of score.py: synthetic run built from common.ref, encoded by x265.

    py -E -X utf8 selftest.py <out dir> <W>x<H> <kbps>

Scenes: grey (10 frames), junkshop still (90), google scroll step 9 (180, one frame
doubled to test the duplicate path). Writes meta.json / bitstream.bin / run.json the
way tap.js + driver.py will, then the caller runs score.py on it.
"""
import json
import os
import subprocess
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as cm  # noqa: E402


def main():
    out, geo, kbps = sys.argv[1], sys.argv[2], int(sys.argv[3])
    w, h = map(int, geo.split("x"))
    os.makedirs(out, exist_ok=True)
    scenes = [{"id": 1, "mode": "still", "src": "src/%s/junkshop.png" % geo},
              {"id": 2, "mode": "scroll", "src": "src/%s/google.png" % geo, "step": 9, "pause": 0}]
    srcs = {s["id"]: cm.load_rgb(os.path.join(cm.HERE, "content", s["src"])) for s in scenes}
    grey = np.full((h, w, 3), 128, np.uint8)
    cm.draw_bar(grey, 0)
    seq = [grey] * 10
    seq += [cm.ref(scenes[0], srcs[1], w, h, i) for i in range(90)]
    sc = [cm.ref(scenes[1], srcs[2], w, h, i) for i in range(180)]
    sc.insert(50, sc[50])  # host captured the same page frame twice
    seq += sc
    stream = os.path.join(out, "bitstream.bin")
    p = subprocess.Popen([cm.FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", "rawvideo", "-pix_fmt", "rgb24",
                          "-s", "%dx%d" % (w, h), "-r", "60", "-i", "-",
                          "-vf", "scale=out_color_matrix=bt709:out_range=tv,format=yuv420p",
                          "-c:v", "libx265", "-preset", "ultrafast", "-tune", "zerolatency",
                          "-x265-params", "bitrate=%d:vbv-maxrate=%d:vbv-bufsize=%d:keyint=-1:log-level=error"
                          % (kbps, kbps, kbps // 60 * 2),
                          "-f", "hevc", stream], stdin=subprocess.PIPE)
    for f in seq:
        p.stdin.write(f.tobytes())
    p.stdin.close()
    if p.wait():
        raise SystemExit("x265 failed")
    json.dump({"configs": [{"codec": "hev1.1.6.L120.B0"}], "chunks": [], "frames": []},
              open(os.path.join(out, "meta.json"), "w"))
    json.dump({"W": w, "H": h, "fps": 60, "kbps": kbps, "codec": "hevc", "scenes": scenes},
              open(os.path.join(out, "run.json"), "w"), indent=1)
    print("frames", len(seq), "bytes", os.path.getsize(stream))


if __name__ == "__main__":
    main()
