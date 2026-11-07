"""Bit-exact check: the client's hardware decoder against ffmpeg, on the frames tap.js copied.

    py -E -X utf8 verify.py <run dir>

tap.js copied the decoder's own planes (VideoFrame.copyTo) at a few output indices.
ffmpeg decodes the recorded bitstream again; output index i of both is the same picture
(no reordering in a low-latency stream: checked on the barcode too). Prints, per frame,
the number of differing bytes in Y and in UV, and the barcode both carry.
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common as cm  # noqa: E402


def planes(buf, meta, w, h):
    fmt = meta["format"]
    lay = meta["layout"]
    if fmt != "NV12":
        raise SystemExit("frame %d is %s, only NV12 handled" % (meta["idx"], fmt))
    y0, ys = lay[0]["offset"], lay[0]["stride"]
    u0, us = lay[1]["offset"], lay[1]["stride"]
    # copyTo() without a rect copies the visible rect only: planes of h and h/2 rows
    yp = np.frombuffer(buf, np.uint8, ys * h, y0).reshape(h, ys)[:, :w]
    uv = np.frombuffer(buf, np.uint8, us * (h // 2), u0).reshape(h // 2, us)[:, :w]
    return yp, uv


def main():
    run = os.path.abspath(sys.argv[1])
    spec = json.load(open(os.path.join(run, "run.json"), encoding="utf-8"))
    meta = json.load(open(os.path.join(run, "meta.json"), encoding="utf-8"))
    w, h = spec["W"], spec["H"]
    stream, _ = cm.annexb(run)
    want = {fm["idx"] for fm in meta["frames"]}
    kept = {}
    n = 0
    for i, buf in cm.decode_iter(stream, w, h):
        n = i + 1
        if i in want:
            kept[i] = buf
    out = []
    for fm in meta["frames"]:
        i = fm["idx"]
        hw = open(os.path.join(run, "frame-%06d.raw" % i), "rb").read()
        hy, huv = planes(hw, fm, w, h)
        if i >= n:
            out.append({"idx": i, "error": "ffmpeg has only %d frames" % n})
            continue
        sw = kept[i]
        sy = sw[:w * h].reshape(h, w)
        suv = sw[w * h:].reshape(h // 2, w)
        out.append({"idx": i, "hw_bar": cm.read_bar(np.ascontiguousarray(hy)), "sw_bar": cm.read_bar(sy),
                    "y_diff_bytes": int((hy != sy).sum()), "uv_diff_bytes": int((huv != suv).sum()),
                    "y_max_abs": int(np.abs(hy.astype(int) - sy).max()),
                    "format": fm["format"], "colorSpace": fm.get("colorSpace")})
    res = {"frames": out, "bit_exact": all(o.get("y_diff_bytes") == 0 and o.get("uv_diff_bytes") == 0 for o in out)}
    json.dump(res, open(os.path.join(run, "verify.json"), "w", encoding="utf-8"), indent=1)
    print(json.dumps(res, indent=1))


if __name__ == "__main__":
    main()
