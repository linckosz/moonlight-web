"""How much light an HDR run really carried: the calibration scene of the "hdrpeak" plan
(bench.html mode 'calib', bands of known light, 1.0 = SDR white), read back from the recorded
stream. Each band's luma code -> PQ -> nits, against band x sdr_nits. A band that comes out
short is clipped (the virtual display's headroom, or Chrome's): the highlights' peak
(driver.py --hdr-peak) must stay under the last band that came out whole.

    py hdrcal.py runs/H/<run>
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import common as cm  # noqa: E402
import fmtref as fr  # noqa: E402
from score import decode_fmt  # noqa: E402


def main():
    run = os.path.abspath(sys.argv[1])
    # --cut-cols x0:x1 (as score.py): the bands are read beside a window that covered the screen
    cut = [int(v) for v in sys.argv[sys.argv.index("--cut-cols") + 1].split(":")] if "--cut-cols" in sys.argv else None
    spec = json.load(open(os.path.join(run, "run.json"), encoding="utf-8"))
    if spec.get("fmt") != "hdr" or not spec.get("sdr_nits"):
        raise SystemExit("an HDR run with sdr_nits is needed")
    cal = [s for s in spec["scenes"] if s.get("mode") == "calib"]
    if not cal:
        raise SystemExit("no calibration scene in this run")
    sid, bands = cal[0]["id"], cal[0]["bands"]
    w, h = spec["W"], spec["H"]
    rows = h - 2 * cm.cell(h)
    stream, _ = cm.annexb(run)
    per_band = [[] for _ in bands]
    frames = 0
    for _, buf in decode_fmt(stream, w, h, "hdr"):
        y, _, _ = fr.planes("hdr", buf, w, h)
        v = cm.read_bar(fr.luma8("hdr", y))
        if v is None or cm.split(v)[0] != sid:
            continue
        frames += 1
        bh = rows / len(bands)
        for j in range(len(bands)):
            a, b = int(j * bh + bh / 4), int((j + 1) * bh - bh / 4)
            if cut:
                strip = np.concatenate([y[a:b, w // 16:cut[0] - 16], y[a:b, cut[1] + 16:w - w // 16]], axis=1)
            else:
                strip = y[a:b, w // 4:3 * w // 4]
            per_band[j].append(np.median(strip))
    if not frames:
        raise SystemExit("no frame of the calibration scene in the stream")
    nits = spec["sdr_nits"]
    print("%d frames of scene %d, SDR white %.0f nits" % (frames, sid, nits))
    whole = None
    ratios = []
    for m, codes in zip(bands, per_band):
        code = float(np.median(codes))
        got = float(fr.pq_inverse(np.array([(code - 64.0) / 876.0]))[0])
        want = m * nits
        ratios.append((m, got / want))
        ok = abs(got / want - 1) <= 0.03
        if ok:
            whole = m
        print("band %4.2f x white: want %6.0f nits, got %6.0f (code %5.1f)  %s" % (
            m, want, got, code, "ok" if ok else "SHORT" if got < want else "over"))
    print("highest band carried whole: %s x SDR white" % whole)
    # What the run really carried at 1.0: the bands up to 2x, far below any clipping. score.py
    # scales its reference by it (cal.json), so a small miss of the gain does not count as loss.
    low = [r for m, r in ratios if m <= 2.0]
    ratio = float(np.median(low))
    lin = [r / ratio for m, r in ratios]
    print("white carried: x%.4f of %.0f nits; bands against it: %s" % (
        ratio, nits, " ".join("%.2f:%.3f" % (m, x) for (m, _), x in zip(ratios, lin))))
    json.dump({"white_ratio": ratio, "whole": whole, "bands": ratios},
              open(os.path.join(run, "cal.json"), "w", encoding="utf-8"))


if __name__ == "__main__":
    main()
