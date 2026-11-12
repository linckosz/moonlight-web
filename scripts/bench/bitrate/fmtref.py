"""Formats past SDR 4:2:0 for the Auto-bitrate bench, and their references.

    "444": full chroma, BT.709 limited, 8-bit. Any stream can be scored this way: a 4:2:0
           stream is decoded with its chroma brought up to 4:4:4 by ffmpeg, so a 4:2:0 and a
           4:4:4 stream are measured against the same full-chroma reference.
    "hdr": BT.2020 PQ, 10-bit limited, 4:2:0 (what the native host sends with HDR on).

The references follow the native host's shaders (backend/native-host/src/convert/windows/
ConvertHlsl.h: PsPacked444, PsLumaHdr, PsChromaHdr), from the page's sRGB pixels:
    - 444: y = Kr R + Kg G + Kb B on the 0..1 values, cb = (B - y) / 1.8556, cr = (R - y) / 1.5748,
      limited range, rounded to nearest (UNORM);
    - hdr: the compositor paints SDR content at the desktop's SDR white (scRGB, 1.0 = 80 nits),
      stored as FP16; BT.709 -> BT.2020 in linear light; PQ; BT.2020 NCL matrix on the PQ signal;
      luma at each pixel, chroma from the 2 x 2 linear-light mean (the sampler at the chroma
      texel's centre), 10-bit codes rounded to nearest.
How the compositor turns sRGB into light (the piecewise sRGB curve or a 2.2 power) is a
parameter: `check_hdr.py` picks the one a high-bitrate stream agrees with.
"""
import numpy as np

KR709, KB709 = 0.2126, 0.0722
K2020 = np.array([0.2627, 0.6780, 0.0593], dtype=np.float32)
M709_2020 = np.array([[0.627403914928436, 0.329283038616180, 0.043313046455383],
                      [0.069097289442527, 0.919540429115295, 0.011362281441689],
                      [0.016391439363360, 0.088013307750225, 0.895595252513885]], dtype=np.float32)
PQ_M1, PQ_M2 = 0.1593017578125, 78.84375
PQ_C1, PQ_C2, PQ_C3 = 0.8359375, 18.8515625, 18.6875

FORMATS = {
    # pix: ffmpeg pixel format (decode and libvmaf); bps: bytes per sample; sub: chroma subsampling
    "444": {"pix": "yuv444p", "bps": 1, "sub": 1},
    "hdr": {"pix": "yuv420p10le", "bps": 2, "sub": 2},
}


def frame_bytes(fmt, w, h):
    f = FORMATS[fmt]
    return f["bps"] * (w * h + 2 * (w // f["sub"]) * (h // f["sub"]))


def planes(fmt, buf, w, h):
    """Decoded frame bytes -> (Y, Cb, Cr) arrays (uint8 or uint16)."""
    f = FORMATS[fmt]
    a = np.frombuffer(buf, dtype=np.uint8 if f["bps"] == 1 else "<u2")
    cw, ch = w // f["sub"], h // f["sub"]
    y = a[:w * h].reshape(h, w)
    cb = a[w * h:w * h + cw * ch].reshape(ch, cw)
    cr = a[w * h + cw * ch:w * h + 2 * cw * ch].reshape(ch, cw)
    return y, cb, cr


def luma8(fmt, y):
    """A luma plane on the 8-bit scale, for the barcode."""
    return y if y.dtype == np.uint8 else (y >> 2).astype(np.uint8)


def unorm(x, scale, bias):
    return np.floor(x * scale + bias + 0.5)


def pq(lin_scrgb):
    """scRGB linear light (1.0 = 80 nits) -> PQ signal 0..1, as PqFromLinear."""
    y = np.maximum(lin_scrgb, 0.0).astype(np.float32) * np.float32(80.0 / 10000.0)
    ym = np.power(y, np.float32(PQ_M1))
    return np.power((PQ_C1 + PQ_C2 * ym) / (1.0 + PQ_C3 * ym), np.float32(PQ_M2))


def pq_inverse(e):
    """PQ signal -> nits."""
    e = np.clip(e, 0.0, 1.0).astype(np.float64)
    ep = np.power(e, 1.0 / PQ_M2)
    return 10000.0 * np.power(np.maximum(ep - PQ_C1, 0.0) / (PQ_C2 - PQ_C3 * ep), 1.0 / PQ_M1)


def srgb_light(v8, curve):
    """8-bit sRGB values -> linear light, 0..1 of SDR white."""
    v = v8.astype(np.float32) / 255.0
    if curve == "g22":
        return np.power(v, np.float32(2.2))
    return np.where(v <= 0.04045, v / 12.92, np.power((v + 0.055) / 1.055, np.float32(2.4))).astype(np.float32)


class Ref444:
    """Full-chroma BT.709 limited planes of a whole source picture, cropped per frame."""

    def __init__(self, src):
        rgb = src.astype(np.float32) / 255.0
        y = rgb @ np.array([KR709, 1 - KR709 - KB709, KB709], dtype=np.float32)
        cb = (rgb[..., 2] - y) / 1.8556
        cr = (rgb[..., 0] - y) / 1.5748
        self.p = [unorm(y, 219.0, 16.0).astype(np.uint8), unorm(cb, 224.0, 128.0).astype(np.uint8),
                  unorm(cr, 224.0, 128.0).astype(np.uint8)]

    def frame(self, top, rows, w):
        return [np.ascontiguousarray(p[top:top + rows, :w]) for p in self.p]


def boost_light(lin, k, t):
    """The highlights of bench.html's "hdr" scenes (its WGSL `boost`), in light, 1.0 = SDR white:
    above the BT.709 luminance t, every channel is multiplied by 1 + (k - 1) u^2,
    u = (Y - t) / (1 - t) clamped to 0..1, so SDR white becomes k x SDR white."""
    y = lin @ np.array([KR709, 1 - KR709 - KB709, KB709], dtype=np.float32)
    u = np.clip((y - np.float32(t)) / np.float32(1.0 - t), 0.0, 1.0)
    return (lin * (1.0 + np.float32(k - 1.0) * u * u)[..., None]).astype(np.float32)


class RefHdr:
    """BT.2020 PQ 10-bit limited planes: luma precomputed for the whole source, chroma per frame
    (a crop may start on an odd row, and chroma comes from the 2 x 2 mean in light).
    `boost` {"k", "t"}: a scene drawn by bench.html's HDR canvas, with highlights over the SDR
    white. That canvas decodes the page's sRGB with the piecewise curve on the GPU and hands the
    compositor its light directly, so the curve is the piecewise one whatever `curve` says."""

    def __init__(self, src, sdr_nits, curve="srgb", boost=None):
        white = np.float32(sdr_nits / 80.0)  # SDR white in scRGB
        light = srgb_light(src, "srgb" if boost else curve)
        if boost:
            light = boost_light(light, float(boost["k"]), float(boost["t"]))
        lin = (light * white).astype(np.float16)  # the FP16 desktop
        self.lin = (lin.astype(np.float32) @ M709_2020.T).astype(np.float32)
        e = pq(self.lin)
        self.y = unorm(e @ K2020, 876.0, 64.0).astype(np.uint16)

    def frame(self, top, rows, w):
        y = np.ascontiguousarray(self.y[top:top + rows, :w])
        L = self.lin[top:top + rows, :w]
        m = 0.25 * (L[0::2, 0::2] + L[1::2, 0::2] + L[0::2, 1::2] + L[1::2, 1::2])
        e = pq(m)
        ey = e @ K2020
        cb = unorm((e[..., 2] - ey) / 1.8814, 896.0, 512.0).astype(np.uint16)
        cr = unorm((e[..., 0] - ey) / 1.4746, 896.0, 512.0).astype(np.uint16)
        return [y, cb, cr]


# ── ΔE ITP (ITU-R BT.2124) between two BT.2020 PQ 10-bit limited 4:2:0 pictures ───────────────
_LMS = np.array([[1688, 2146, 262], [683, 2951, 462], [99, 309, 3688]], dtype=np.float64) / 4096
_ICTCP = np.array([[2048, 2048, 0], [6610, -13613, 7003], [17933, -17390, -543]], dtype=np.float64) / 4096


def _ictcp(y, cb, cr):
    """Y'CbCr codes on the chroma grid (luma taken at even pixels) -> ICtCp."""
    e_y = (y.astype(np.float64) - 64.0) / 876.0
    e_cb = (cb.astype(np.float64) - 512.0) / 896.0
    e_cr = (cr.astype(np.float64) - 512.0) / 896.0
    r = e_y + 1.4746 * e_cr
    b = e_y + 1.8814 * e_cb
    g = (e_y - K2020[0] * r - K2020[2] * b) / K2020[1]
    rgb = np.stack([pq_inverse(r), pq_inverse(g), pq_inverse(b)], axis=-1) / 10000.0
    lms = rgb @ _LMS.T
    ym = np.power(np.clip(lms, 0.0, 1.0), PQ_M1)  # PQ of LMS, 0..1
    lp = np.power((PQ_C1 + PQ_C2 * ym) / (1.0 + PQ_C3 * ym), PQ_M2)
    return lp @ _ICTCP.T


def delta_itp(dist, ref):
    """Mean ΔE ITP over the picture; dist and ref are (Y, Cb, Cr) uint16 planes, 4:2:0."""
    a = _ictcp(dist[0][0::2, 0::2], dist[1], dist[2])
    b = _ictcp(ref[0][0::2, 0::2], ref[1], ref[2])
    d = a - b
    return float(np.mean(720.0 * np.sqrt(d[..., 0] ** 2 + (0.5 * d[..., 1]) ** 2 + d[..., 2] ** 2)))
