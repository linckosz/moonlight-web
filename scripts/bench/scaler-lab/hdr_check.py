"""mwsr on an HDR stream, through the product's WebGpuRenderer, on the software
adapter: upscaled and downscaled, on the two HDR paths that run it.

    python -X utf8 hdr_check.py [--dir DIR] [--keep] [--only NAME,...]
    python -X utf8 hdr_check.py --time N --cdp PORT [--vendor WORD] [--only NAME,...] [--gap MS]

The lab's HDR mires (hdr_mires.py: a PQ ramp up to 10 000 nits with fine
1000 / 0.5-nit stripes, text on 1000 nits, the 0 A.D. capture of
C:\\Test\\01_4K.png with its highlights pushed to 1000 nits, and stripes of 1000
and 0.5 nits one and three pixels wide) are made into
what a decoder hands the renderer: PQ, BT.2020, limited-range Y'CbCr in
10 bits, I420P10 (one case in I444P10). hdr-check.html draws each one with
the real WebGpuRenderer, on its two HDR readback paths:
  - 'linear' (an HDR display): the rgba16float canvas in extended tone
    mapping, mwsr on float intermediates (1.0 = 203 nits, sRGB transfer
    continued past 1.0, Display-P3);
  - 'tonemap' (an SDR display): ACES in pass 0, then mwsr on 8-bit.
Each with 'off' (the product's plain copy) and 'mwsr', at 720p and 1080p to
1440p, 2160p to 1440p and 1440p to 720p, plus one frame at 1:1. Beside them,
reported only: the reduction of before 26/10/2026 ('mwsr-enc': pass 0
encoded, the 2x2 grid in gamma-2 light, where the product's pass 0 now
writes linear light for a reduction, journal J47), and the f32 kernel where
SwiftShader grants shader-f16.

The checks, judged here in linear light (metrics_hdr.py):
  - setup: no WebGPU error, the readback path taken (+yuv), the float canvas
    accepted on 'linear';
  - feed: at 1:1 the picture equals numpy's pass 0 (YUV -> PQ -> P3);
  - finite: every value finite, none negative;
  - peak: the highlights kept (no clip at SDR white): the share of pixels
    above 600 nits at least 0.9 of the truth's (not the peaks themselves:
    the truth's Lanczos rings past the input's peak, and past 1100 nits on
    the edges of 1000-nit text, a 4:2:0 input has its fringes); and none
    made, the output's peak at most 1.05 of the input's;
  - bounds: on 'linear', no channel beyond the 4x4 input texels around the
    pixel, in the kernel's own space (the encoded values), on an upscale and
    at x0.5; the J11 measure (BT.2020 linear, the local 2x2) is reported beside
    it. On 'tonemap' the kernel is the SDR one, whose margin is on purpose: the
    share is reported only;
  - quality: mwsr's colour error to the truth (dE ITP) no worse than off's
    (+2 %) on 'linear', as on 21/10 (J11); on 'tonemap' the kernel is the SDR
    one on an SDR picture, judged by the SDR lab, its PSNR reported only;
  - light: on a reduction of 1- and 3-pixel stripes of 1000 and 0.5 nits,
    their mean light within 5 % of the truth's (a filter averaging encoded
    values, or the square of them, mixes them too dark);
  - f16: the f16 kernel within 0.4 % of the f32 one, where both ran;
  - off: a scaled frame with no upscaler is the bilinear copy of its input
    (the YUV pass 0 drawn at the canvas' size took the nearest texel).
Chrome runs headless on SwiftShader only (the page refuses any other adapter
before asking for a device), at BelowNormal on CPUs 16-23, with a profile of
its own and --mute-audio; its tree is killed at the end ("chrome left: 0").
The dumps (RGBA half floats) are deleted once measured, unless --keep. Exit 1
on a failed check.

--time N measures the speed instead, on a GPU, never on the software adapter:
'mwsr' and 'mwsr-enc' on 'linear', N draws each in ABBA order after a
warm-up, every render pass of the renderer stamped (timestamp-query), so that
pass 0 and the kernel are timed apart (p50, p99, ms; hdr-time.json). It
launches no Chrome: --cdp names one the caller launched pinned on that GPU
(--use-adapter-luid, its own profile, --mute-audio) and checked, before
anything is measured, by the growth of the WDDM counters on that adapter
alone; the page refuses SwiftShader and an adapter whose name lacks
--vendor. Each draw's start and passes go into hdr-time.json too (`seq`), to
set a tail against the GPU's clocks; --gap MS idles before each block, so
that the GPU drops its P-state in front of both variants (V10).
"""
import argparse
import functools
import http.server
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

import numpy as np
import psutil
import websocket
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import hdr_mires  # noqa: E402
import metrics_hdr  # noqa: E402
from hdr_mires import REF_WHITE, encode, lin_to_srgb, pq_eotf, pq_oetf, reduce_linear, srgb_to_lin  # noqa: E402
from metrics_hdr import M_2020_TO_P3, M_P3_TO_2020  # noqa: E402
from scaler_lab import CHROME, REPO, LabHandler, free_port  # noqa: E402

BELOW_NORMAL = 0x00004000
CREATE_SUSPENDED = 0x00000004
CPUS = list(range(16, 24))
GAME_PNG = r"C:\Test\01_4K.png"
# Bruno's SDR yardstick (2560x1440): in HDR, its highlights pushed to 1000
# nits as the game's are; in SDR, itself.
ETALON_PNG = r"C:\Test\00_Background.png"
# BT.2020 non-constant luminance
KR, KG, KB = 0.2627, 0.6780, 0.0593
# The renderer's BT.2020 -> Rec.709 matrix (WGSL, column-major), as rows here.
M_2020_TO_709 = np.array([[1.6605, -0.5876, -0.0728],
                          [-0.1246, 1.1329, -0.0083],
                          [-0.0182, -0.1006, 1.1187]])
TONEMAP_WHITE = 433.0  # the 'tonemap' path's reference white (nits)
ACES_WHITE = 0.80379

# name, mire, input size, output size, chroma
CASES = [
    ("id720-ramp", "ramp", (1280, 720), (1280, 720), "420"),
    ("up720-ramp", "ramp", (1280, 720), (2560, 1440), "420"),
    ("up720-text", "text", (1280, 720), (2560, 1440), "420"),
    ("up720-game", "game", (1280, 720), (2560, 1440), "420"),
    ("up720-text444", "text", (1280, 720), (2560, 1440), "444"),
    ("up1080-ramp", "ramp", (1920, 1080), (2560, 1440), "420"),
    ("up1080-game", "game", (1920, 1080), (2560, 1440), "420"),
    ("down2160-ramp", "ramp", (3840, 2160), (2560, 1440), "420"),
    ("down2160-text", "text", (3840, 2160), (2560, 1440), "420"),
    ("down2160-game", "game", (3840, 2160), (2560, 1440), "420"),
    ("down2160-stripes", "stripes", (3840, 2160), (2560, 1440), "420"),
    ("down2160to720-stripes", "stripes", (3840, 2160), (1280, 720), "420"),
    ("down1440-ramp", "ramp", (2560, 1440), (1280, 720), "420"),
    ("down1440-text", "text", (2560, 1440), (1280, 720), "420"),
    ("down1440-stripes", "stripes", (2560, 1440), (1280, 720), "420"),
    ("up720-etalon", "etalon", (1280, 720), (2560, 1440), "420"),
    ("up1080-etalon", "etalon", (1920, 1080), (2560, 1440), "420"),
    ("up720-etalon444", "etalon", (1280, 720), (2560, 1440), "444"),
    ("up1080-etalon444", "etalon", (1920, 1080), (2560, 1440), "444"),
]
PNGS = {"game": GAME_PNG, "etalon": ETALON_PNG}


def stripes(w, h):
    """Vertical stripes of 1000 and 0.5 nits, one pixel wide in the top half,
    three in the bottom one: every reduction mixes them, and the light it
    mixes them in shows in their mean (500 nits)."""
    xs = np.arange(w)
    nits = np.empty((h, w, 3), dtype=np.float32)
    nits[:h // 2] = np.where((xs % 2 == 0)[None, :, None], 1000.0, 0.5)
    nits[h // 2:] = np.where(((xs // 3) % 2 == 0)[None, :, None], 1000.0, 0.5)
    return nits


def mire(name, size, cache):
    """A mire's linear nits (Display-P3) at size, made at that size or reduced
    from the 4K capture."""
    w, h = size
    if name in PNGS:
        if name not in cache:
            cache[name] = hdr_mires.game(PNGS[name])
        g = cache[name]
        return g if g.shape[:2] == (h, w) else reduce_linear(g, size)
    if name == "stripes":
        return stripes(w, h)
    return getattr(hdr_mires, name)(w, h)


def box2(c):
    return 0.25 * (c[0::2, 0::2] + c[1::2, 0::2] + c[0::2, 1::2] + c[1::2, 1::2])


def yuv_planes(nits, chroma):
    """Linear nits (Display-P3) -> the decoder's planes: PQ, BT.2020, limited
    range, 10 bits (uint16), the chroma averaged 2x2 in PQ for 4:2:0."""
    rgb = pq_oetf(np.maximum(nits.astype(np.float64) @ M_P3_TO_2020.T, 0.0))
    y = rgb @ np.array([KR, KG, KB])
    cb = (rgb[..., 2] - y) / 1.8814
    cr = (rgb[..., 0] - y) / 1.4746
    if chroma == "420":
        cb, cr = box2(cb), box2(cr)

    def q(v, off, k):
        return np.clip(np.round(off + k * v), 0, 1023).astype("<u2")
    return q(y, 64, 876), q(cb, 512, 896), q(cr, 512, 896)


def rgb_from_planes(y, u, v, chroma):
    """The renderer's pass 0 up to R'G'B' (PQ, BT.2020, clamped to [0, 1]),
    the chroma taken by its nearest texel as the shader's textureLoad does."""
    h, w = y.shape
    if chroma == "420":
        u = u.repeat(2, 0).repeat(2, 1)[:h, :w]
        v = v.repeat(2, 0).repeat(2, 1)[:h, :w]
    yl = (y.astype(np.float64) - 64.0) / 876.0
    cb = (u.astype(np.float64) - 512.0) / 896.0
    cr = (v.astype(np.float64) - 512.0) / 896.0
    rgb = np.stack([yl + 1.4746 * cr, yl - 0.16455 * cb - 0.57135 * cr, yl + 1.8814 * cb], axis=2)
    return np.clip(rgb, 0.0, 1.0)


def feed_linear(y, u, v, chroma):
    """numpy's pass 0 of the 'linear' path (tonemapPq, curve 2): the feed's
    encoded values, Display-P3, 1.0 = 203 nits."""
    p3 = pq_eotf(rgb_from_planes(y, u, v, chroma)) @ M_2020_TO_P3.T
    return lin_to_srgb(np.maximum(p3, 0.0) / REF_WHITE).astype(np.float32)


def tonemap_sdr(nits_2020):
    """The 'tonemap' path's curve (ACES, white-normalised) on BT.2020 nits:
    linear Rec.709 in [0, 1]."""
    rgb709 = np.maximum((nits_2020 / TONEMAP_WHITE) @ M_2020_TO_709.T, 0.0)
    a = (rgb709 * (2.51 * rgb709 + 0.03)) / (rgb709 * (2.43 * rgb709 + 0.59) + 0.14)
    return np.clip(np.clip(a, 0.0, 1.0) / ACES_WHITE, 0.0, 1.0)


def bilinear(img, size):
    """A bilinear sample at each output pixel's centre, clamped to the edge,
    as the GPU's sampler takes it."""
    ow, oh = size
    ih, iw = img.shape[:2]

    def axis(n_out, n_in):
        u = (np.arange(n_out) + 0.5) * (n_in / n_out) - 0.5
        i0 = np.floor(u).astype(int)
        f = (u - i0).astype(np.float32)
        return np.clip(i0, 0, n_in - 1), np.clip(i0 + 1, 0, n_in - 1), f
    y0, y1, fy = axis(oh, ih)
    x0, x1, fx = axis(ow, iw)
    rows = img[y0] * (1 - fy)[:, None, None] + img[y1] * fy[:, None, None]
    return rows[:, x0] * (1 - fx)[None, :, None] + rows[:, x1] * fx[None, :, None]


def beyond_texels(out, feed, tol_abs, tol_rel):
    """The share of pixels with a channel beyond the 4x4 input texels around
    them (the bilinear sample's quad and one more on each side), past a
    rounding tolerance."""
    oh, ow = out.shape[:2]
    ih, iw = feed.shape[:2]
    x0 = np.floor((np.arange(ow) + 0.5) * (iw / ow) - 0.5).astype(int)
    y0 = np.floor((np.arange(oh) + 0.5) * (ih / oh) - 0.5).astype(int)
    lo = np.full(out.shape, np.inf, dtype=np.float32)
    hi = np.full(out.shape, -np.inf, dtype=np.float32)
    for dy in (-1, 0, 1, 2):
        rows = feed[np.clip(y0 + dy, 0, ih - 1)]
        for dx in (-1, 0, 1, 2):
            v = rows[:, np.clip(x0 + dx, 0, iw - 1)]
            np.minimum(lo, v, out=lo)
            np.maximum(hi, v, out=hi)
    tol = tol_abs + tol_rel * np.abs(hi)
    return float(((out > hi + tol) | (out < lo - tol)).any(axis=2).mean())


def load_dump(path, w, h):
    a = np.frombuffer(open(path, "rb").read(), dtype=np.float16)
    if a.size != w * h * 4:
        raise SystemExit("%s: %d halves, not %dx%dx4" % (path, a.size, w, h))
    return a.reshape(h, w, 4)[..., :3].astype(np.float32)


def prepare(a):
    """The planes of each case on disk, its truth and feed kept here."""
    cache, cases, data = {}, [], {}
    for name, mname, size_in, size_out, chroma in CASES:
        if a.only and name not in a.only:
            continue
        if mname in PNGS and not os.path.exists(PNGS[mname]):
            print("%s: skipped, no %s" % (name, PNGS[mname]))
            continue
        if size_out[0] >= size_in[0]:
            truth = mire(mname, size_out, cache)
            inp = truth if size_out == size_in else reduce_linear(truth, size_in)
        else:
            inp = mire(mname, size_in, cache)
            truth = reduce_linear(inp, size_out)
        y, u, v = yuv_planes(inp, chroma)
        yuv = "hdr-%s.yuv" % name
        with open(os.path.join(a.dir, yuv), "wb") as f:
            for p in (y, u, v):
                f.write(np.ascontiguousarray(p).tobytes())
        feed = feed_linear(y, u, v, chroma)
        # the 'tonemap' path's input to the scaler, and the truths of both paths
        sdr_in = tonemap_sdr(pq_eotf(rgb_from_planes(y, u, v, chroma)))
        truth_2020 = np.maximum(truth.astype(np.float64) @ M_P3_TO_2020.T, 0.0)
        sdr_truth = (tonemap_sdr(truth_2020) if size_out[0] >= size_in[0]
                     else reduce_linear(sdr_in.astype(np.float32), size_out))
        data[name] = {"feed": feed, "gt": encode(truth), "sdr_gt": lin_to_srgb(sdr_truth).astype(np.float32),
                      # the 'tonemap' path's inputTex: 8-bit
                      "sdr_feed": np.round(lin_to_srgb(sdr_in) * 255.0).astype(np.uint8),
                      "mire": mname, "in": size_in, "out": size_out}
        case = {"name": name, "mire": mname, "in": size_in, "out": size_out, "chroma": chroma, "yuv": yuv}
        cases.append(case)
        print("prepared %-14s %s %dx%d -> %dx%d, input peak %.0f nits" % (
            name, chroma, size_in[0], size_in[1], size_out[0], size_out[1],
            float(metrics_hdr.luminance(metrics_hdr.nits_2020(feed)).max())))
    with open(os.path.join(a.dir, "hdr-cases.json"), "w") as f:
        json.dump(cases, f)
    return data


def drive(a, cdp, port):
    """hdr-check.html (with ?time= under --time) in the Chrome on DevTools port
    cdp, followed to its answer."""
    page = None
    for _ in range(100):
        try:
            targets = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % cdp))
            page = next(t for t in targets if t["type"] == "page")
            break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    if not page:
        raise SystemExit("no page on the DevTools port %d" % cdp)
    ws = websocket.create_connection(page["webSocketDebuggerUrl"], timeout=600, suppress_origin=True)
    ids = iter(range(1, 1 << 30))

    def call(method, **params):
        mid = next(ids)
        ws.send(json.dumps({"id": mid, "method": method, "params": params}))
        while True:
            msg = json.loads(ws.recv())
            if msg.get("id") == mid:
                return msg.get("result", msg)

    query = "?time=%d&vendor=%s" % (a.time, a.vendor) if a.time else "?"
    if a.dry:
        query += "&dry=1"
    if a.gap:
        query += "&gap=%d" % a.gap
    call("Page.navigate", url="http://127.0.0.1:%d/scripts/bench/scaler-lab/hdr-check.html%s" % (port, query))
    deadline = time.time() + a.timeout
    shown = None
    res = None
    while time.time() < deadline:
        time.sleep(2)
        r = call("Runtime.evaluate", returnByValue=True,
                 expression="JSON.stringify([window.__progress || '', window.__done || null])")
        progress, res = json.loads(r.get("result", {}).get("value") or '["", null]')
        if progress != shown:
            shown = progress
            print("  %s page: %s" % (time.strftime("%H:%M:%S"), progress), flush=True)
        if res:
            break
    ws.close()
    return res


def run_page(a, port):
    """hdr-check.html in a headless Chrome on SwiftShader, or (--cdp) in the
    Chrome the caller launched, pinned on a GPU and checked; its answer."""
    if a.cdp:
        return drive(a, a.cdp, port)
    cdp = free_port()
    profile = tempfile.mkdtemp(prefix="mw-scaler-lab-")
    flags = ["--headless=new", "--remote-debugging-port=%d" % cdp, "--user-data-dir=" + profile,
             "--enable-unsafe-webgpu", "--enable-unsafe-swiftshader", "--use-webgpu-adapter=swiftshader",
             "--mute-audio", "--no-first-run", "--no-default-browser-check",
             "--disable-background-timer-throttling", "--disable-renderer-backgrounding"]
    # Suspended until its priority and CPUs are set: the processes it starts take both.
    chrome = subprocess.Popen([CHROME, *flags, "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                              creationflags=BELOW_NORMAL | CREATE_SUSPENDED)
    proc = psutil.Process(chrome.pid)
    proc.cpu_affinity(CPUS)
    proc.resume()
    res = None
    try:
        res = drive(a, cdp, port)
    finally:
        tree = [chrome.pid]
        try:
            tree += [c.pid for c in proc.children(recursive=True)]
        except psutil.Error:
            pass
        for pid in tree:
            try:
                psutil.Process(pid).kill()
            except psutil.Error:
                pass
        time.sleep(1)
        left = [p.pid for p in psutil.process_iter(["cmdline"])
                if p.info["cmdline"] and any(profile in c for c in p.info["cmdline"])]
        print("chrome left: %d" % len(left))
        shutil.rmtree(profile, ignore_errors=True)
    return res


def judge(a, data, res):
    """Each draw measured against its truth; the failed checks."""
    fails = []
    rows = {}

    def check(ok, what):
        if not ok:
            fails.append(what)
        return ok

    for r in res["results"]:
        d = data[r["case"]]
        tag = "%s %s %s" % (r["case"], r["mode"], r["variant"])
        ow, oh = d["out"]
        if r.get("error"):
            check(False, "%s: %s" % (tag, r["error"][:300]))
            continue
        check(not r["errors"] and not r["failed"], "%s: WebGPU errors %s, failed %s" % (tag, r["errors"][:3], r["failed"]))
        check("+yuv" in (r["path"] or ""), "%s: not the readback path (%s)" % (tag, r["path"]))
        if r["mode"] == "linear":
            check(r["cfg"]["hdr"] and r["cfg"]["format"] == "rgba16float",
                  "%s: the float canvas refused (%s)" % (tag, r["cfg"]))
        check(r["size"] == [ow, oh], "%s: canvas %s, not %dx%d" % (tag, r["size"], ow, oh))
        path = os.path.join(a.dir, r["dump"] + ".f16")
        out = load_dump(path, ow, oh)
        row = {"path": r["path"], "regime": r.get("regime"), "half": r["cfg"].get("half")}
        row["finite"] = bool(np.isfinite(out).all())
        row["min"] = float(np.nanmin(out))
        check(row["finite"] and row["min"] >= -1e-4, "%s: values not finite or negative (min %.4g)" % (tag, row["min"]))
        down = ow < d["in"][0]
        x05 = down and d["in"][0] == 2 * ow
        # the kernel's checks; the reduction of before is reported beside it only
        judged = r["variant"] not in ("off", "mwsr-enc")
        if r["variant"] == "off" and d["out"] != d["in"]:
            if r["mode"] == "linear":
                ref = bilinear(d["feed"], d["out"])
                err = np.abs(out - ref) - 0.004 * np.abs(ref)
                lim = 0.02
            else:
                ref = np.round(bilinear(d["sdr_feed"].astype(np.float32) / 255.0, d["out"]) * 255.0) / 255.0
                err = np.abs(out - ref)
                lim = 2.5 / 255.0
            row["bilinear"] = float(err.max())
            check(row["bilinear"] <= lim, "%s: not the bilinear copy of its input (%.4f off)" % (tag, row["bilinear"]))
        if r["mode"] == "linear":
            m = metrics_hdr.measure(d["gt"], d["feed"], out)
            row.update({k: m[k] for k in ("dEitp", "dEitpP99", "psnrPQ", "maxNits", "maxRatio", "overshoot")})
            if d["out"] == d["in"]:
                diff = np.abs(out - d["feed"])
                row["feedMax"] = float(diff.max())
                check(bool((diff <= 0.002 + 0.003 * np.abs(d["feed"])).all()),
                      "%s: the 1:1 picture is not numpy's pass 0 (max %.4f)" % (tag, row["feedMax"]))
            if judged and d["mire"] != "stripes":
                b_out = float((metrics_hdr.luminance(metrics_hdr.nits_2020(out)) > 600.0).mean())
                b_gt = float((metrics_hdr.luminance(metrics_hdr.nits_2020(d["gt"])) > 600.0).mean())
                row["peakKept"] = b_out / b_gt
                check(row["peakKept"] >= 0.9, "%s: %.2f %% of the pixels above 600 nits, %.3f of the truth's share" % (
                    tag, 100 * b_out, row["peakKept"]))
                check(m["maxRatio"] <= 1.05, "%s: peak %.0f nits, %.3f of the input's: a highlight made" % (
                    tag, m["maxNits"], m["maxRatio"]))
            if judged and (not down or x05):
                row["bounds"] = beyond_texels(out, d["feed"], 0.002, 0.004)
                check(row["bounds"] <= 0.0001, "%s: %.3f %% of the pixels beyond the 4x4 texels around" % (
                    tag, 100 * row["bounds"]))
            lum_out = metrics_hdr.luminance(metrics_hdr.nits_2020(out))
            lum_gt = metrics_hdr.luminance(metrics_hdr.nits_2020(d["gt"]))
        else:
            gt = d["sdr_gt"]
            mse = float(np.mean((out - gt) ** 2))
            row["psnr"] = 10 * np.log10(1.0 / mse) if mse > 0 else 99.0
            if judged and (not down or x05):
                row["bounds"] = beyond_texels(out, d["sdr_feed"].astype(np.float32) / 255.0, 1.5 / 255.0, 0.0)
            lum_out = srgb_to_lin(out) @ np.array([0.2126, 0.7152, 0.0722])
            lum_gt = srgb_to_lin(gt) @ np.array([0.2126, 0.7152, 0.0722])
        if d["mire"] == "stripes":
            # their mean light, 4 pixels off the edges (any normalised filter keeps it)
            row["stripes"] = float(lum_out[4:-4, 4:-4].mean() / lum_gt[4:-4, 4:-4].mean())
            if r["variant"] == "mwsr":
                check(abs(row["stripes"] - 1) <= 0.05,
                      "%s: the stripes' light %.3f of the truth's" % (tag, row["stripes"]))
        # the picture itself only where the f16 kernel will be held against the f32 one
        rows[(r["case"], r["mode"], r["variant"])] = (row, out if res.get("f16") else None)
        if not a.keep:
            os.remove(path)

    # mwsr against off, and the f16 kernel against the f32 one
    for (case, mode, variant), (row, out) in rows.items():
        off = rows.get((case, mode, "off"))
        tag = "%s %s %s" % (case, mode, variant)
        if variant == "mwsr" and off and mode == "linear" and data[case]["out"] != data[case]["in"]:
            check(row["dEitp"] <= 1.02 * off[0]["dEitp"],
                  "%s: dE ITP %.3f, off %.3f" % (tag, row["dEitp"], off[0]["dEitp"]))
        f32 = rows.get((case, mode, "mwsr-f32"))
        if variant == "mwsr" and f32:
            rel = float((np.abs(out - f32[1]) / np.maximum(np.abs(f32[1]), 1.0)).max())
            row["f16VsF32"] = rel
            check(rel <= 0.004, "%s: the f16 kernel %.4f off the f32 one" % (tag, rel))
    return rows, fails


def fmt(x, spec):
    return spec % x if isinstance(x, (int, float)) else "-"


def time_report(a, res):
    """--time: the table of pass 0, the kernel and both, p50 and p99 (ms, the
    GPU's own stamps); written to hdr-time.json. Exit 1 on an error."""
    fails = []
    print("%-22s %-6s %-9s %-12s %-12s %6s %17s %17s %17s" % (
        "case", "mode", "variant", "path", "regime", "passes", "pass 0 p50/p99", "kernel p50/p99", "total p50/p99"))
    for r in res["results"]:
        if r.get("error"):
            fails.append("%s: %s" % (r["case"], r["error"][:300]))
            continue
        yuv = r.get("mode", "linear") == "linear"
        if r["errors"] or r["failed"] or (yuv and "+yuv" not in (r["path"] or "")) or not (r["path"] or "").startswith("mwsr"):
            fails.append("%s %s: errors %s, failed %s, path %s" % (
                r["case"], r["variant"], r["errors"][:3], r["failed"], r["path"]))
        reg = r["regime"] or {}
        regime = reg.get("mode", "") + (" t%d ll%d" % (reg["taps"], reg["linearLight"]) if reg.get("mode") == "down" else "")
        print("%-22s %-6s %-9s %-12s %-12s %6d %8.4f/%8.4f %8.4f/%8.4f %8.4f/%8.4f" % (
            r["case"], r.get("mode", "linear"), r["variant"], r["path"], regime, r["passes"],
            r["pass0"]["p50"], r["pass0"]["p99"], r["kernel"]["p50"], r["kernel"]["p99"],
            r["total"]["p50"], r["total"]["p99"]))
    with open(os.path.join(a.dir, "hdr-time.json"), "w") as f:
        json.dump({"adapter": res["adapter"], "draws": a.time, "results": res["results"], "fails": fails}, f, indent=1)
    print("FAIL:\n  " + "\n  ".join(fails) if fails else "timed, no error")
    sys.exit(1 if fails else 0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default=os.path.join(REPO, "bench-out", "scaler-lab", "hdr"))
    ap.add_argument("--keep", action="store_true", help="keep the dumps (RGBA half floats)")
    ap.add_argument("--only", default="", help="the cases to run, by name (comma-separated)")
    ap.add_argument("--timeout", type=float, default=3600)
    ap.add_argument("--time", type=int, default=0,
                    help="N: the speed of 'mwsr' and 'mwsr-enc' on 'linear' instead, N draws each (needs --cdp)")
    ap.add_argument("--cdp", type=int, default=0,
                    help="the DevTools port of a Chrome the caller launched, pinned on a GPU and checked")
    ap.add_argument("--vendor", default="nvidia", help="--time: a word the adapter's name must hold")
    ap.add_argument("--dry", action="store_true", help="--time on SwiftShader: the code alone, figures worth nothing")
    ap.add_argument("--gap", type=int, default=0,
                    help="--time: MS of idle before each block, for the GPU to drop its P-state (V10, 28/10/2026)")
    a = ap.parse_args()
    a.only = [s for s in a.only.split(",") if s]
    if not a.only:
        # the yardstick's cases only when named by --only
        a.only = [name for name, mname, size_in, size_out, _ in CASES if mname != "etalon"]
    if a.time and not a.cdp and not a.dry:
        raise SystemExit("--time measures a GPU: give it the Chrome pinned and checked on one (--cdp)")
    os.makedirs(a.dir, exist_ok=True)
    data = prepare(a)

    port = free_port()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(LabHandler, directory=REPO))
    server.lab_dir = a.dir
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        res = run_page(a, port)
    finally:
        server.shutdown()
    if not res:
        raise SystemExit("the page gave no answer in %d s" % a.timeout)
    if not res.get("ok"):
        raise SystemExit("the page failed: %s" % res.get("error"))

    print("adapter: %s, shader-f16 %s" % (res["adapter"], res["f16"]))
    if a.time:
        time_report(a, res)
    rows, fails = judge(a, data, res)
    print("%-16s %-7s %-9s %-9s %-14s %7s %7s %8s %6s %6s %7s %8s %7s %7s %8s %8s" % (
        "case", "mode", "variant", "path", "regime", "dE ITP", "PSNR", "peak", "x in", "kept", "over %", "bounds %",
        "stripes", "f16", "1:1 max", "bilin"))
    for (case, mode, variant), (row, _) in rows.items():
        reg = row.get("regime") or {}
        regime = reg.get("mode", "") + (" t%d ll%d" % (reg["taps"], reg["linearLight"]) if reg.get("mode") == "down" else "")
        print("%-16s %-7s %-9s %-9s %-14s %7s %7s %8s %6s %6s %7s %8s %7s %7s %8s %8s" % (
            case, mode, variant, row["path"], regime,
            fmt(row.get("dEitp"), "%.3f"), fmt(row.get("psnrPQ", row.get("psnr")), "%.2f"),
            fmt(row.get("maxNits"), "%.0f"), fmt(row.get("maxRatio"), "%.3f"), fmt(row.get("peakKept"), "%.3f"),
            fmt(None if row.get("overshoot") is None else 100 * row["overshoot"], "%.3f"),
            fmt(None if row.get("bounds") is None else 100 * row["bounds"], "%.3f"),
            fmt(row.get("stripes"), "%.3f"), fmt(row.get("f16VsF32"), "%.4f"), fmt(row.get("feedMax"), "%.4f"),
            fmt(row.get("bilinear"), "%.4f")))
    summary = {"adapter": res["adapter"], "f16": res["f16"],
               "rows": [{"case": c, "mode": m, "variant": v, **row} for (c, m, v), (row, _) in rows.items()],
               "fails": fails}
    with open(os.path.join(a.dir, "hdr-check.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print("FAIL:\n  " + "\n  ".join(fails) if fails else "all checks passed")
    sys.exit(1 if fails else 0)


if __name__ == "__main__":
    main()
