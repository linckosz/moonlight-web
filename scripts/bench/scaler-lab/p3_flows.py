"""The P3 flows of the laboratory driver (plan upscaler-mwsr.md §4, §8.2;
criteria C8, C9, C10): the reduction against a Lanczos-2 reference, the 4:4:4
mire against its 4:2:0 self, the HDR mires on the half-float feed, the P3 timing
set, and the golden fingerprints. Called from scaler_lab.py with its `run_page`.
"""
import hashlib
import json
import os
import sys

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import metrics  # noqa: E402

try:  # the logs go to files on Windows: never let a cp1252 console choke on a sign
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except Exception:  # noqa: BLE001
    pass

QUICK = "&rounds=1&batches=1&batch=4&warm=1"

# The reduction (C10): the bilinear the client does today (`tex-s1`), Canvas2D's
# `high` smoothing (the Smooth2D reference of the criterion), the shipped
# kernels run out of their range, and the mwsr surface filter in its grids,
# spans and lights.
REDUCE_CELLS = ("tex-s1,tex-s1-srgb,smooth2d,fsr1,sgsr-src,mwsr,mwsr:srgb=1,mwsr:ll=0,mwsr:ll=2,"
                "mwsr:taps=9,mwsr:taps=16,mwsr:rdown=0.75,mwsr:rdown=1.25")
# 4:4:4 (C9): the kernels on the raw input and on its 4:2:0 self.
C444_CELLS = "tex-s1,sgsr-src,nis,fsr1,mwsr,mwsr:c444=1,mwsr:show=1,mwsr:c444=1;show=1"
# HDR (C8): FSR1 is the product's HDR enhancer; SGSR1 clamps to 1.0 (a witness).
HDR_CELLS = "tex-s1,fsr1,sgsr-src,mwsr,mwsr:half=1,mwsr:show=1"


def sizes(a):
    iw, ih = (int(x) for x in a.size_in.split("x"))
    ow, oh = (int(x) for x in a.size_out.split("x"))
    return iw, ih, ow, oh


# ── resampling references ───────────────────────────────────────────────────

def _lanczos_matrix(n_in, n_out, a):
    """Weights (n_out × n_in) of a Lanczos-a resampling, normalised per row."""
    scale = n_in / n_out
    support = a * max(scale, 1.0)
    W = np.zeros((n_out, n_in), dtype=np.float64)
    for i in range(n_out):
        centre = (i + 0.5) * scale - 0.5
        j0 = max(0, int(np.ceil(centre - support)))
        j1 = min(n_in - 1, int(np.floor(centre + support)))
        j = np.arange(j0, j1 + 1)
        x = (j - centre) / max(scale, 1.0)
        w = np.sinc(x) * np.sinc(x / a)
        w[np.abs(x) >= a] = 0.0
        s = w.sum()
        if s != 0:
            W[i, j0:j1 + 1] = w / s
    return W


def lanczos_linear(img_srgb, size, a=2):
    """img (h, w, 3) sRGB floats → (size[1], size[0], 3), Lanczos-a in linear light."""
    lin = np.where(img_srgb <= 0.04045, img_srgb / 12.92, ((img_srgb + 0.055) / 1.055) ** 2.4)
    Wy = _lanczos_matrix(lin.shape[0], size[1], a)
    Wx = _lanczos_matrix(lin.shape[1], size[0], a)
    out = np.einsum("oi,ijc->ojc", Wy, lin)
    out = np.einsum("pj,ojc->opc", Wx, out)
    out = np.clip(out, 0.0, 1.0)
    return np.where(out <= 0.0031308, out * 12.92, 1.055 * np.power(out, 1 / 2.4) - 0.055).astype(np.float32)


def save_png(img, path):
    Image.fromarray((np.clip(img, 0, 1) * 255.0 + 0.5).astype(np.uint8), mode="RGB").save(path)


def chroma_420(img):
    """The picture through a 4:2:0 pipeline: Y'CbCr (BT.709, full range) on the
    encoded values, the chroma planes box-averaged 2×2 and brought back by a
    bilinear upsampling (what the browser's import does), then RGB again."""
    r, g, b = img[..., 0], img[..., 1], img[..., 2]
    y = 0.2126 * r + 0.7152 * g + 0.0722 * b
    cb = (b - y) / 1.8556
    cr = (r - y) / 1.5748
    h, w = y.shape
    h2, w2 = h - h % 2, w - w % 2

    def down_up(p):
        q = p[:h2, :w2].reshape(h2 // 2, 2, w2 // 2, 2).mean(axis=(1, 3))
        up = Image.fromarray(np.ascontiguousarray(q.astype(np.float32)), mode="F").resize((w, h), Image.BILINEAR)
        return np.asarray(up, dtype=np.float32)

    cb2, cr2 = down_up(cb), down_up(cr)
    r2 = y + 1.5748 * cr2
    b2 = y + 1.8556 * cb2
    g2 = (y - 0.2126 * r2 - 0.0722 * b2) / 0.7152
    return np.clip(np.stack([r2, g2, b2], axis=2), 0.0, 1.0).astype(np.float32)


# ── the flows ───────────────────────────────────────────────────────────────

def _print_rows(rows, extra=()):
    for r in rows:
        line = "  %-18s PSNR %6.2f  SSIM %.4f  over %5.2f%%  centre PSNR %6.2f  FLIP %s" % (
            r["cell"], r["psnr"], r["ssim"], 100 * r["overshoot"], r["centre"]["psnr"], metrics.fmt(r["flip"]))
        c = r.get("chroma")
        if c:
            line += "  | Cb %5.2f Cr %5.2f dE00 %5.3f p99 %5.2f" % (c["psnrCb"], c["psnrCr"], c["dE00"], c["dE00p99"])
        z = r.get("zone")
        if z:
            line += "  | alias %6.4f flat %6.4f lines PSNR %6.2f" % (z["alias"], z["flat"], z["linesPsnr"])
        print(line)


def _run_cells(a, run_page, query, base, gt, inp, zone_in=None, rows_extra=None):
    res = run_page(query)
    if "error" in res:
        print(base, "cells ERROR", res["error"][:600])
        return None, res
    print("== %s (%s -> %s), adapter %s" % (base, a.size_in, a.size_out, res["adapter"]["name"]))
    rows = []
    for name in res.get("dumped", []):
        cell = name[len(base) + 1:]
        out = metrics.load(os.path.join(a.dir, name + ".png"))
        if "show" in cell:
            print("  %-18s edge path %5.1f%% of the pixels" % (cell, 100 * float(out[..., 1].mean())))
            continue
        r = metrics.measure(gt, inp, out)
        r["chroma"] = metrics.chroma(gt, out)
        if zone_in:
            r["zone"] = metrics.zone_metrics(gt, out, *zone_in)
        r["cell"] = cell
        rows.append(r)
    _print_rows(rows)
    if res.get("errors"):
        print("  errors:", res["errors"][:5])
    with open(os.path.join(a.dir, base + "-quality.json"), "w") as f:
        json.dump({"adapter": res["adapter"], "rows": rows}, f, indent=1)
    sys.stdout.flush()
    return rows, res


def reduce_quality(a, run_page, contents):
    """C10: the page's picture at the input size, its Lanczos-2 reduction in
    linear light as the reference, the cells' outputs against it."""
    iw, ih, ow, oh = sizes(a)
    cells = a.quality_cells or REDUCE_CELLS
    for content in contents:
        base = "%s-down-%s" % (a.tag, content.replace(":", "_").replace("/", "_"))
        res = run_page("gt=1&content=%s&out=%s&tag=%s" % (content, a.size_in, base))
        if "error" in res:
            print(base, "source ERROR", res["error"][:600])
            continue
        src_path = os.path.join(a.dir, base + "-src.png")
        os.replace(os.path.join(a.dir, base + "-gt.png"), src_path)
        src = metrics.load(src_path)
        gt = lanczos_linear(src, (ow, oh), a=2)
        gt_path = os.path.join(a.dir, base + "-gt.png")
        save_png(gt, gt_path)
        gt = metrics.load(gt_path)
        _run_cells(a, run_page,
                   "in=%s&out=%s&content=img:/__dir/%s-src.png&codec=none&cells=%s&dump=all%s&tag=%s" % (
                       a.size_in, a.size_out, base, cells, QUICK, base),
                   base, gt, src, zone_in=(iw, ih) if content == "zone" else None)


def c444_quality(a, run_page, contents):
    """C9: the ground truth at the output size, the Lanczos-3 input (4:4:4) and
    its 4:2:0 self, the cells on both."""
    iw, ih, ow, oh = sizes(a)
    cells = a.quality_cells or C444_CELLS
    from scaler_lab import make_input
    for content in contents:
        base = "%s-c444-%s" % (a.tag, content.replace(":", "_").replace("/", "_"))
        res = run_page("gt=1&content=%s&out=%s&tag=%s" % (content, a.size_out, base))
        if "error" in res:
            print(base, "ground truth ERROR", res["error"][:600])
            continue
        gt_path = os.path.join(a.dir, base + "-gt.png")
        in_path = os.path.join(a.dir, base + "-in.png")
        make_input(gt_path, in_path, (iw, ih))
        in420_path = os.path.join(a.dir, base + "-in420.png")
        save_png(chroma_420(metrics.load(in_path)), in420_path)
        gt = metrics.load(gt_path)
        for variant, suffix in (("444", "-in"), ("420", "-in420")):
            inp = metrics.load(os.path.join(a.dir, base + suffix + ".png"))
            tag = base + "-" + variant
            _run_cells(a, run_page,
                       "in=%s&out=%s&content=img:/__dir/%s%s.png&codec=none&cells=%s&dump=all%s&tag=%s" % (
                           a.size_in, a.size_out, base, suffix, cells, QUICK, tag),
                       tag, gt, inp)


def hdr_quality(a, run_page, mires):
    """C8: the HDR mires (hdr_mires.py) on the rgba16float feed, judged in
    linear light (metrics_hdr.py)."""
    import hdr_mires
    import metrics_hdr
    iw, ih, ow, oh = sizes(a)
    cells = a.quality_cells or HDR_CELLS
    for mire in mires:
        base = "%s-hdr-%s" % (a.tag, mire)
        if mire == "game":
            res = run_page("gt=1&content=game&out=%s&tag=%s-sdr" % (a.size_out, base))
            if "error" in res:
                print(base, "sdr picture ERROR", res["error"][:600])
                continue
            nits = hdr_mires.game(os.path.join(a.dir, base + "-sdr-gt.png"))
        elif mire in ("ramp", "text"):
            nits = getattr(hdr_mires, mire)(ow, oh)
        else:
            print(base, "no such HDR mire (ramp, text, game)")
            continue
        gt_path = os.path.join(a.dir, base + "-gt.npy")
        in_path = os.path.join(a.dir, base + "-in.f16")
        in_enc = hdr_mires.write(nits, (iw, ih), gt_path, in_path)
        gt_enc = np.load(gt_path)
        hdr_mires.preview_png(gt_enc, os.path.join(a.dir, base + "-gt-pq.png"))
        res = run_page("hdr=1&f16=1&in=%s&out=%s&content=f16:/__dir/%s-in.f16&codec=none&cells=%s&dump=all%s&tag=%s" % (
            a.size_in, a.size_out, base, cells, QUICK, base))
        if "error" in res:
            print(base, "cells ERROR", res["error"][:600])
            continue
        print("== %s (%s -> %s, %s), adapter %s" % (base, a.size_in, a.size_out, res.get("format"), res["adapter"]["name"]))
        rows = []
        for name in res.get("dumped", []):
            cell = name[len(base) + 1:]
            out = metrics_hdr.load_f16(os.path.join(a.dir, name + ".f16"), ow, oh)
            if "show" in cell:
                print("  %-14s edge path %5.1f%% of the pixels" % (cell, 100 * float(out[..., 1].mean())))
                continue
            r = metrics_hdr.measure(gt_enc, in_enc, out)
            r["cell"] = cell
            rows.append(r)
            print("  " + metrics_hdr.fmt_row(cell, r))
            hdr_mires.preview_png(out, os.path.join(a.dir, name + "-pq.png"))
        if res.get("errors"):
            print("  errors:", res["errors"][:5])
        with open(os.path.join(a.dir, base + "-quality.json"), "w") as f:
            json.dump({"adapter": res["adapter"], "format": res.get("format"), "rows": rows}, f, indent=1)
        sys.stdout.flush()


def p3_cases(a):
    """The P3 timing set: the HDR feed at ×2 and ×1.33 (rgba16float targets, the
    text mire written first), the reduction at ×0.75, ×0.5 and ×0.33, and the
    4:4:4 options at ×2 on the code mire."""
    import hdr_mires
    hdr = []
    for iw, ih, name in ((1280, 720, "720"), (1920, 1080, "1080")):
        nits = hdr_mires.text(2560, 1440)
        base = os.path.join(a.dir, "%s-p3hdr-%s" % (a.tag, name))
        hdr_mires.write(nits, (iw, ih), base + "-gt.npy", base + "-in.f16")
        hdr.append("f16:/__dir/%s-p3hdr-%s-in.f16" % (a.tag, name))
    hdr_cells = "tex-s1,fsr1,sgsr-src,mwsr,mwsr:half=1"
    return [
        ("hdr-x2-text", "hdr=1&f16=1&in=1280x720&out=2560x1440&codec=none&content=%s&cells=%s&rounds=3" % (hdr[0], hdr_cells)),
        ("hdr-x133-text", "hdr=1&f16=1&in=1920x1080&out=2560x1440&codec=none&content=%s&cells=%s&rounds=3" % (hdr[1], hdr_cells)),
        ("down-x075-text", "in=2560x1440&out=1920x1080&content=text&codec=none&cells="
                           "tex-s1,tex-s1-srgb,fsr1,sgsr-src,mwsr,mwsr:srgb=1,mwsr:taps=4;srgb=1,mwsr:ll=0,"
                           "mwsr:ll=2,mwsr:taps=9;ll=1&rounds=3"),
        ("down-x05-text", "in=2560x1440&out=1280x720&content=text&codec=none&cells="
                          "tex-s1,tex-s1-srgb,fsr1,mwsr,mwsr:srgb=1,mwsr:taps=4;srgb=1,mwsr:ll=0,mwsr:ll=2,"
                          "mwsr:taps=9;ll=1,mwsr:taps=16;ll=1&rounds=3"),
        ("down-x033-zone", "in=3840x2160&out=1280x720&content=zone&codec=none&cells="
                           "tex-s1,tex-s1-srgb,mwsr,mwsr:srgb=1,mwsr:ll=0,mwsr:ll=2,mwsr:taps=16;ll=1,"
                           "mwsr:taps=16;srgb=1&rounds=3"),
        ("x2-code-c444", "in=1280x720&out=2560x1440&content=code&codec=none&cells="
                         "tex-s1,mwsr,mwsr:c444=1&rounds=3"),
    ]


# ── golden fingerprints (plan §8.2) ─────────────────────────────────────────

GOLDEN_FILE = os.path.join(HERE, "golden.json")
# chain-mwsr is the product's WebGPU renderer with the key (P4): its picture
# must be the kernel's at scale and the sRGB-view reduction's below 1:1. (The
# WebGL2 twin runs in another Chrome and is held by the conformity flow,
# p4_flows.py, to a tolerance rather than a fingerprint.)
GOLDEN_UP = "tex-s1,mwsr,mwsr:c444=1,chain-mwsr"
GOLDEN_DOWN = "tex-s1,mwsr,mwsr:srgb=1,mwsr:taps=9,mwsr:ll=2,chain-mwsr"
GOLDEN_CASES = [
    ("text", "1280x720", "2560x1440", GOLDEN_UP),
    ("game", "1280x720", "2560x1440", GOLDEN_UP),
    ("code", "1280x720", "2560x1440", GOLDEN_UP),
    ("text", "1920x1080", "2560x1440", GOLDEN_UP),
    ("game", "1920x1080", "2560x1440", GOLDEN_UP),
    ("code", "1920x1080", "2560x1440", GOLDEN_UP),
    ("text", "2560x1440", "1280x720", GOLDEN_DOWN),
    ("zone", "2560x1440", "1280x720", GOLDEN_DOWN),
    ("code", "2560x1440", "1280x720", GOLDEN_DOWN),
]


def golden(a, run_page, check):
    """Each variant on three pictures at three scales, on the software adapter:
    a fingerprint of the pixels (sha256), the mean and the PSNR against the
    bilinear of the same run. `check` compares with golden.json, else writes it."""
    ref = json.load(open(GOLDEN_FILE)) if check and os.path.exists(GOLDEN_FILE) else {}
    if check and not ref:
        print("no", GOLDEN_FILE, "to check against")
        return 2
    new = {"adapter": None, "cases": {}}
    bad = 0
    for content, size_in, size_out, cells in GOLDEN_CASES:
        key = "%s %s->%s" % (content, size_in, size_out)
        base = "golden-%s-%s-%s" % (content, size_in, size_out)
        res = run_page("in=%s&out=%s&content=%s&codec=none&cells=%s&dump=all%s&tag=%s" % (
            size_in, size_out, content, cells, QUICK, base))
        if "error" in res:
            print(key, "ERROR", res["error"][:600])
            bad += 1
            continue
        new["adapter"] = res["adapter"]["name"]
        if "swiftshader" not in new["adapter"].lower():
            print("warning: the adapter is %s, not SwiftShader: the fingerprints are not portable" % new["adapter"])
        outs = {}
        for name in res.get("dumped", []):
            cell = name[len(base) + 1:]
            outs[cell] = metrics.load(os.path.join(a.dir, name + ".png"))
        bil = outs.get("tex-s1")
        entry = {}
        for cell, img in outs.items():
            e = {"sha256": hashlib.sha256((img * 255.0 + 0.5).astype(np.uint8).tobytes()).hexdigest(),
                 "mean": round(float(img.mean()), 6)}
            if bil is not None and cell != "tex-s1":
                e["psnrVsBilinear"] = round(metrics.psnr(bil, img), 3)
            entry[cell] = e
        new["cases"][key] = entry
        if check:
            old = ref.get("cases", {}).get(key, {})
            for cell, e in entry.items():
                o = old.get(cell)
                if not o:
                    print("  %-28s %-16s new (no reference)" % (key, cell))
                    continue
                if o["sha256"] == e["sha256"]:
                    continue
                bad += 1
                print("  %-28s %-16s CHANGED  mean %.6f -> %.6f  psnr vs bilinear %s -> %s" % (
                    key, cell, o["mean"], e["mean"], o.get("psnrVsBilinear"), e.get("psnrVsBilinear")))
        else:
            print("  %-28s %s" % (key, " ".join("%s=%s" % (c, e["sha256"][:8]) for c, e in entry.items())))
        if res.get("errors"):
            print("  errors:", res["errors"][:5])
            bad += 1
        sys.stdout.flush()
    if check:
        print("golden check:", "OK" if not bad else "%d difference(s)" % bad)
        return 1 if bad else 0
    with open(GOLDEN_FILE, "w") as f:
        json.dump(new, f, indent=1, sort_keys=True)
        f.write("\n")
    print("wrote", GOLDEN_FILE, "(adapter %s)" % new["adapter"])
    return 0
