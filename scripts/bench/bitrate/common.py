"""Shared pieces of the Auto-bitrate bench: geometry of content/bench.html, barcode,
references, bitstream to Annex B, ffmpeg decode."""
import base64
import json
import os
import subprocess
import sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nowin  # noqa: E402,F401  no child console window, ever

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
FFMPEG = os.environ.get("MW_FFMPEG", "ffmpeg")


def cell(h):
    return 2 * round(h / 120)


# ── bench.html geometry (must match the page exactly) ────────────────────────
def moved(n, pause):
    if not pause:
        return n
    cyc = 2 * pause
    return (n // cyc) * pause + min(n % cyc, pause)


def pos(n, step, pause, travel):
    if travel <= 0:
        return 0
    u = (moved(n, pause) * step) % (2 * travel)
    return u if u <= travel else 2 * travel - u


def check(v):
    return (((v & 255) + ((v >> 8) & 255)) ^ 0xA5) & 255


def draw_bar(rgb, v):
    """The page's barcode, drawn into an HxWx3 uint8 array in place."""
    h = rgb.shape[0]
    c = cell(h)
    v &= 0xFFFF
    ch = check(v)
    for b in range(24):
        one = (v >> b) & 1 if b < 16 else (ch >> (b - 16)) & 1
        rgb[h - 2 * c:h - c, b * c:(b + 1) * c] = 255 if one else 0
        rgb[h - c:h, b * c:(b + 1) * c] = 0 if one else 255
    return rgb


def read_bar(y):
    """Frame number from a luma plane (HxW uint8, limited or full range). None if unreadable."""
    h = y.shape[0]
    c = cell(h)
    q = max(1, c // 4)
    bits = []
    for b in range(24):
        cx = b * c + c // 2
        top = float(y[h - 2 * c + c // 2 - q:h - 2 * c + c // 2 + q, cx - q:cx + q].mean())
        bot = float(y[h - c + c // 2 - q:h - c + c // 2 + q, cx - q:cx + q].mean())
        if abs(top - bot) < 40:
            return None
        bits.append(1 if top > bot else 0)
    v = sum(bits[i] << i for i in range(16))
    ch = sum(bits[16 + i] << i for i in range(8))
    return v if ch == check(v) else None


DIM_WHITE = 225  # SDR barcode white below this: the screen was dimmed (driver.py refuses the point)


def bar_white(y):
    """Mean luma of the barcode's white cells (their centres, as read_bar samples them), or None
    if the barcode is unreadable. The cells are drawn #fff outside the page's picture: an SDR
    frame carries them at Y 235 (limited) or 255 (the G of a full-range RGB frame). 12/11: a
    Windows Security prompt on the primary virtual screen dimmed the whole screen behind it, x0.54
    on the encoded values (white at 137), and every SDR point of that run was lost."""
    v = read_bar(y)
    if v is None:
        return None
    h = y.shape[0]
    c = cell(h)
    q = max(1, c // 4)
    ch = check(v & 0xFFFF)
    vals = []
    for b in range(24):
        one = (v >> b) & 1 if b < 16 else (ch >> (b - 16)) & 1
        cy = h - 2 * c + c // 2 if one else h - c + c // 2  # white on top for a 1, below for a 0
        cx = b * c + c // 2
        vals.append(float(y[cy - q:cy + q, cx - q:cx + q].mean()))
    return float(np.mean(vals))


def dimmed(whites):
    """The lowest barcode white of a run if it is below DIM_WHITE (the run must be refused), else
    None. `whites`: bar_white() of each checked frame (None ones skipped)."""
    ws = [w for w in whites if w is not None]
    if not ws:
        return None
    low = min(ws)
    return low if low < DIM_WHITE else None


def crop_rows(h):
    """Rows scored: everything above the barcode band."""
    return h - 2 * cell(h)


def split(v):
    """Barcode value -> (scene id, frame n within the scene)."""
    return (v >> 13) & 7, v & 0x1FFF


# ── sources and references ───────────────────────────────────────────────────
def load_rgb(path):
    return np.asarray(Image.open(path).convert("RGB"))


def scene_pos(scene, src_h, h, i):
    if scene.get("mode") != "scroll":
        return 0
    return pos(i, scene["step"], scene.get("pause", 0), max(0, src_h - h))


def ref(scene, src, w, h, i):
    """The frame bench.html draws for scene `scene` at frame i (barcode included)."""
    p = scene_pos(scene, src.shape[0], h, i)
    rgb = np.array(src[p:p + h, :w], dtype=np.uint8)
    return draw_bar(rgb, ((scene["id"] & 7) << 13) | (i & 0x1FFF))


# ── bitstream ────────────────────────────────────────────────────────────────
def _avcc_ps(desc, hevc):
    """Parameter sets (Annex B) and NAL length size from an avcC/hvcC description."""
    d = desc
    out = b""
    if not hevc:
        nls = (d[4] & 3) + 1
        i = 5
        nsps = d[i] & 31
        i += 1
        for _ in range(nsps):
            n = int.from_bytes(d[i:i + 2], "big")
            out += b"\0\0\0\1" + d[i + 2:i + 2 + n]
            i += 2 + n
        npps = d[i]
        i += 1
        for _ in range(npps):
            n = int.from_bytes(d[i:i + 2], "big")
            out += b"\0\0\0\1" + d[i + 2:i + 2 + n]
            i += 2 + n
        return out, nls
    nls = (d[21] & 3) + 1
    narr = d[22]
    i = 23
    for _ in range(narr):
        i += 1
        cnt = int.from_bytes(d[i:i + 2], "big")
        i += 2
        for _ in range(cnt):
            n = int.from_bytes(d[i:i + 2], "big")
            out += b"\0\0\0\1" + d[i + 2:i + 2 + n]
            i += 2 + n
    return out, nls


def annexb(run_dir):
    """meta.json + bitstream.bin -> stream.<ext> in Annex B; returns (path, codec)."""
    meta = json.load(open(os.path.join(run_dir, "meta.json"), encoding="utf-8"))
    raw = open(os.path.join(run_dir, "bitstream.bin"), "rb").read()
    cfg = meta["configs"][-1] if meta["configs"] else {"codec": "hev1"}
    codec = cfg["codec"].split(".")[0]
    hevc = codec in ("hev1", "hvc1")
    av1 = codec == "av01"
    ext = "hevc" if hevc else "obu" if av1 else "h264"
    out = os.path.join(run_dir, "stream." + ext)
    desc = base64.b64decode(cfg["description"]) if cfg.get("description") else None
    with open(out, "wb") as f:
        if av1 or desc is None:
            f.write(raw)  # Annex B (H.264/HEVC without description) or AV1 low-overhead OBUs
        else:
            ps, nls = _avcc_ps(desc, hevc)
            o = 0
            for c in meta["chunks"]:
                b = raw[o:o + c["n"]]
                o += c["n"]
                if c["type"] == "key":
                    f.write(ps)
                i = 0
                while i + nls <= len(b):
                    n = int.from_bytes(b[i:i + nls], "big")
                    f.write(b"\0\0\0\1" + b[i + nls:i + nls + n])
                    i += nls + n
    return out, codec


def decode_nv12(stream, w, h, out_raw):
    """Software decode (bit-exact) of the whole stream to raw NV12 frames, one file."""
    fmt = {"hevc": "hevc", "h264": "h264", "obu": "obu"}[stream.rsplit(".", 1)[1]]
    subprocess.run([FFMPEG, "-hide_banner", "-loglevel", "error", "-y", "-f", fmt, "-i", stream,
                    "-vf", "crop=%d:%d:0:0" % (w, h), "-pix_fmt", "nv12", "-f", "rawvideo", out_raw], check=True, stdin=subprocess.DEVNULL)
    n = os.path.getsize(out_raw) // (w * h * 3 // 2)
    return n


def decode_iter(stream, w, h):
    """Same decode as decode_nv12, read through a pipe: yields (index, NV12 bytes as uint8)."""
    fmt = {"hevc": "hevc", "h264": "h264", "obu": "obu"}[stream.rsplit(".", 1)[1]]
    p = subprocess.Popen([FFMPEG, "-hide_banner", "-loglevel", "error", "-f", fmt, "-i", stream,
                          "-vf", "crop=%d:%d:0:0" % (w, h), "-pix_fmt", "nv12", "-f", "rawvideo", "pipe:1"],
                         stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, bufsize=0)
    size = w * h * 3 // 2
    i = 0
    try:
        while True:
            b = read_exact(p.stdout, size)
            if b is None:
                break
            yield i, np.frombuffer(b, dtype=np.uint8)
            i += 1
    finally:
        p.stdout.close()
        if p.wait() != 0:
            raise SystemExit("ffmpeg decode failed")


def read_exact(f, size):
    """size bytes from a pipe, or None at the end (a partial last frame is dropped, as before)."""
    buf = bytearray(size)
    view = memoryview(buf)
    got = 0
    while got < size:
        k = f.readinto(view[got:])
        if not k:
            return None
        got += k
    return bytes(buf)


def nv12_frames(raw_path, w, h):
    """Plain reads, no memmap: Windows will not delete a file that is still mapped."""
    size = w * h * 3 // 2
    with open(raw_path, "rb") as f:
        i = 0
        while True:
            b = f.read(size)
            if len(b) < size:
                return
            yield i, np.frombuffer(b, dtype=np.uint8)
            i += 1


def nv12_frame(raw_path, w, h, i):
    size = w * h * 3 // 2
    with open(raw_path, "rb") as f:
        f.seek(i * size)
        return np.frombuffer(f.read(size), dtype=np.uint8)


def nv12_to_rgb(buf, w, h):
    """BT.709 limited range NV12 -> RGB uint8 (nearest chroma, fixed math: decoder-independent)."""
    y = buf[:w * h].reshape(h, w).astype(np.float32)
    uv = buf[w * h:].reshape(h // 2, w // 2, 2).astype(np.float32)
    u = np.repeat(np.repeat(uv[:, :, 0], 2, 0), 2, 1) - 128.0
    v = np.repeat(np.repeat(uv[:, :, 1], 2, 0), 2, 1) - 128.0
    yy = (y - 16.0) * (255.0 / 219.0)
    s = 255.0 / 224.0
    r = yy + s * 1.5748 * v
    g = yy - s * 0.1873 * u - s * 0.4681 * v
    b = yy + s * 1.8556 * u
    return np.clip(np.stack([r, g, b], -1) + 0.5, 0, 255).astype(np.uint8)
