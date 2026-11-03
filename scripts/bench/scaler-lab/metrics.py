"""Quality of an upscaler's output against the ground truth (plan upscaler-mwsr
§7.2, criteria C3, C4, C10).

    python metrics.py --gt <gt.png> --input <in.png> <cell.png> ... [--json out.json] [--md out.md]

For each output: PSNR and SSIM (scikit-image; SSIM with the Gaussian window of
Wang et al.) against the ground truth; the mean FLIP error, in process
when the flip-evaluator package imports, else through the Python of .flipenv
next to this file (or MW_FLIP_PYTHON): a Python 3.12 venv with the package,
since it has no wheel for Python 3.14 (10/2026) — without either the column
stays empty; and the
overshoot rate, a no-reference measure of ringing: the share of output pixels
that leave the min/max of the four input texels around them (the bilinear
footprint) by more than 2/255 on any channel. A sharpener always overshoots a
little; a kernel that rings on text overshoots a lot.

Also: the same figures on the central third of the picture (where the bench
pictures put their text), and the SSIM of the luma alone.

P3 adds chroma() — the PSNR of the Cb and Cr planes (BT.709) and the CIEDE2000
difference, for the 4:4:4 mire (C9) — and zone_metrics() for the zone plate
reduced (C10): the energy left where the plate's frequency exceeds the output's
Nyquist (aliasing), and the PSNR of the thin lines.
"""
import argparse
import json
import os

import numpy as np
from PIL import Image
from skimage.metrics import peak_signal_noise_ratio, structural_similarity

try:
    import flip_evaluator as flip  # noqa: F401
except Exception:  # noqa: BLE001
    flip = None

HERE = os.path.dirname(os.path.abspath(__file__))
FLIP_PYTHON = os.environ.get("MW_FLIP_PYTHON") or next(
    (p for p in (os.path.join(HERE, ".flipenv", "Scripts", "python.exe"),
                 os.path.join(HERE, ".flipenv", "bin", "python")) if os.path.exists(p)), None)

_FLIP_SCRIPT = '''
import json, sys
import numpy as np
import flip_evaluator as flip
gt = np.load(sys.argv[1])
out = np.load(sys.argv[2])
m, mean, _ = flip.evaluate(gt, out, "LDR")
print(json.dumps({"mean": float(mean), "p99": float(np.percentile(m, 99))}))
'''


def flip_ldr(gt, out):
    """(mean, p99) of the LDR FLIP error, or (None, None) when nothing can
    compute it: in process when the package imports, else through FLIP_PYTHON
    (the arrays go through two .npy files)."""
    if flip is not None:
        err_map, mean, _ = flip.evaluate(gt, out, "LDR")
        return float(mean), float(np.percentile(err_map, 99))
    if not FLIP_PYTHON:
        return None, None
    import subprocess
    import tempfile
    with tempfile.TemporaryDirectory() as d:
        a = os.path.join(d, "gt.npy")
        b = os.path.join(d, "out.npy")
        np.save(a, np.ascontiguousarray(gt, dtype=np.float32))
        np.save(b, np.ascontiguousarray(out, dtype=np.float32))
        r = subprocess.run([FLIP_PYTHON, "-c", _FLIP_SCRIPT, a, b], capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        raise RuntimeError((r.stderr or r.stdout)[-300:])
    j = json.loads(r.stdout.strip().splitlines()[-1])
    return j["mean"], j["p99"]


def load(path):
    return np.asarray(Image.open(path).convert("RGB"), dtype=np.float32) / 255.0


def luma(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def psnr(a, b):
    return float(peak_signal_noise_ratio(a, b, data_range=1.0))


MS_SSIM_WEIGHTS = (0.0448, 0.2856, 0.3001, 0.2363, 0.1333)


def ms_ssim(a, b):
    """MS-SSIM of two luma planes in [0, 1] (Wang, Simoncelli, Bovik 2003), 11-tap Gaussian, 5 scales."""
    from scipy.ndimage import gaussian_filter
    c1, c2 = 0.01 ** 2, 0.03 ** 2
    vals = []
    for s in range(5):
        g = lambda x: gaussian_filter(x, 1.5, truncate=3.5)  # noqa: E731
        mu_a, mu_b = g(a), g(b)
        va = g(a * a) - mu_a ** 2
        vb = g(b * b) - mu_b ** 2
        cov = g(a * b) - mu_a * mu_b
        cs = (2 * cov + c2) / (va + vb + c2)
        if s == 4:
            lum = (2 * mu_a * mu_b + c1) / (mu_a ** 2 + mu_b ** 2 + c1)
            vals.append(max(float((lum * cs).mean()), 0.0))
        else:
            vals.append(max(float(cs.mean()), 0.0))
            h, w = a.shape[0] // 2 * 2, a.shape[1] // 2 * 2
            a = a[:h, :w].reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
            b = b[:h, :w].reshape(h // 2, 2, w // 2, 2).mean(axis=(1, 3))
    return float(np.prod([v ** w for v, w in zip(vals, MS_SSIM_WEIGHTS)]))


def chroma(gt, out):
    """PSNR of the Cb and Cr planes (BT.709, full range, on the encoded
    values) and the CIEDE2000 colour difference, mean and p99."""
    from skimage.color import deltaE_ciede2000, rgb2lab

    def cbcr(a):
        y = luma(a)
        return (a[..., 2] - y) / 1.8556, (a[..., 0] - y) / 1.5748

    cb1, cr1 = cbcr(gt)
    cb2, cr2 = cbcr(out)
    de = deltaE_ciede2000(rgb2lab(gt), rgb2lab(out))
    return {
        "psnrCb": float(peak_signal_noise_ratio(cb1, cb2, data_range=1.0)),
        "psnrCr": float(peak_signal_noise_ratio(cr1, cr2, data_range=1.0)),
        "dE00": float(de.mean()),
        "dE00p99": float(np.percentile(de, 99)),
    }


def zone_metrics(gt, out, in_w, in_h):
    """The zone plate of the lab's `zone` picture (left half; Nyquist at the
    edge of the plate) reduced from in_w×in_h: `alias` is the RMS error to the
    reference in the ring whose input frequency exceeds the output's Nyquist
    (a clean reduction leaves it flat), `flat` the RMS around the mean there
    (the moiré itself), `linesPsnr` the PSNR of the right half (thin lines)."""
    oh, ow = out.shape[:2]
    s = ow / in_w
    half_in = in_w // 2
    cx, cy = (half_in / 2) * s, (in_h / 2) * s
    R = max(half_in / 2, in_h / 2) * s
    yy, xx = np.mgrid[0:oh, 0:ow]
    r = np.hypot(xx + 0.5 - cx, yy + 0.5 - cy)
    ring = (xx < half_in * s) & (r > 1.1 * s * R) & (r < R)
    lo, lg = luma(out)[ring], luma(gt)[ring]
    xr = int(half_in * s)
    return {
        "alias": float(np.sqrt(np.mean((lo - lg) ** 2))) if ring.any() else None,
        "flat": float(np.sqrt(np.mean((lo - lo.mean()) ** 2))) if ring.any() else None,
        "linesPsnr": psnr(gt[:, xr:], out[:, xr:]),
    }


def overshoot(out, inp, tol=2.0 / 255.0):
    """Share of output pixels beyond the local 2×2 min/max of the input."""
    oh, ow = out.shape[:2]
    ih, iw = inp.shape[:2]
    sx = iw / ow
    sy = ih / oh
    px = (np.arange(ow) + 0.5) * sx - 0.5
    py = (np.arange(oh) + 0.5) * sy - 0.5
    x0 = np.clip(np.floor(px).astype(int), 0, iw - 1)
    x1 = np.clip(x0 + 1, 0, iw - 1)
    y0 = np.clip(np.floor(py).astype(int), 0, ih - 1)
    y1 = np.clip(y0 + 1, 0, ih - 1)
    a = inp[y0][:, x0]
    b = inp[y0][:, x1]
    c = inp[y1][:, x0]
    d = inp[y1][:, x1]
    lo = np.minimum(np.minimum(a, b), np.minimum(c, d))
    hi = np.maximum(np.maximum(a, b), np.maximum(c, d))
    over = (out > hi + tol) | (out < lo - tol)
    return float(over.any(axis=2).mean())


def measure(gt, inp, out):
    if out.shape != gt.shape:
        raise SystemExit("size %s != ground truth %s" % (out.shape, gt.shape))
    h, w = gt.shape[:2]
    cy0, cy1 = h // 3, 2 * h // 3
    cx0, cx1 = w // 3, 2 * w // 3
    r = {
        "psnr": float(peak_signal_noise_ratio(gt, out, data_range=1.0)),
        "ssim": float(structural_similarity(gt, out, data_range=1.0, channel_axis=2,
                                            gaussian_weights=True, sigma=1.5, use_sample_covariance=False)),
        "ssimY": float(structural_similarity(luma(gt), luma(out), data_range=1.0,
                                             gaussian_weights=True, sigma=1.5, use_sample_covariance=False)),
        "overshoot": overshoot(out, inp),
        "centre": {
            "psnr": float(peak_signal_noise_ratio(gt[cy0:cy1, cx0:cx1], out[cy0:cy1, cx0:cx1], data_range=1.0)),
            "ssim": float(structural_similarity(gt[cy0:cy1, cx0:cx1], out[cy0:cy1, cx0:cx1], data_range=1.0,
                                                channel_axis=2, gaussian_weights=True, sigma=1.5,
                                                use_sample_covariance=False)),
        },
        "flip": None,
    }
    try:
        mean, p99 = flip_ldr(gt, out)
        if mean is not None:
            r["flip"] = mean
            r["flipP99"] = p99
    except Exception as e:  # noqa: BLE001
        r["flipError"] = str(e)
    return r


def fmt(x, d=3):
    return ("%.*f" % (d, x)) if isinstance(x, (int, float)) else "-"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gt", required=True)
    ap.add_argument("--input", required=True, help="the reduced input the cells read")
    ap.add_argument("outputs", nargs="+")
    ap.add_argument("--json", default="")
    ap.add_argument("--md", default="")
    a = ap.parse_args()
    gt = load(a.gt)
    inp = load(a.input)
    rows = []
    print("%-28s %7s %7s %7s %9s | centre %7s %7s | %6s %6s" % (
        "output", "PSNR", "SSIM", "SSIM Y", "overshoot", "PSNR", "SSIM", "FLIP", "p99"))
    for path in a.outputs:
        r = measure(gt, inp, load(path))
        r["file"] = os.path.basename(path)
        rows.append(r)
        print("%-28s %7.2f %7.4f %7.4f %8.2f%% | centre %7.2f %7.4f | %6s %6s" % (
            r["file"][:28], r["psnr"], r["ssim"], r["ssimY"], 100 * r["overshoot"],
            r["centre"]["psnr"], r["centre"]["ssim"], fmt(r["flip"]), fmt(r.get("flipP99"))))
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"gt": a.gt, "input": a.input, "rows": rows}, f, indent=1)
    if a.md:
        lines = ["| output | PSNR dB | SSIM | SSIM Y | overshoot % | centre PSNR | centre SSIM | FLIP | FLIP p99 |",
                 "|---|---|---|---|---|---|---|---|---|"]
        for r in rows:
            lines.append("| %s | %.2f | %.4f | %.4f | %.2f | %.2f | %.4f | %s | %s |" % (
                r["file"], r["psnr"], r["ssim"], r["ssimY"], 100 * r["overshoot"],
                r["centre"]["psnr"], r["centre"]["ssim"], fmt(r["flip"]), fmt(r.get("flipP99"))))
        with open(a.md, "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
    if flip is None and not FLIP_PYTHON:
        print("(flip-evaluator not importable and no .flipenv: FLIP left empty)")


if __name__ == "__main__":
    main()
