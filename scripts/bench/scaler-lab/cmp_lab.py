"""The lab's scores of the MWSR comparison (website/mwsr/): LPIPS, VMAF NEG, PSNR, SSIM, MS-SSIM and FLIP
of each upscaler, on each of the page's SDR pictures, at each scale and chroma, written to lab-scores.json
next to this script: the figures of the page's and the README's tables (the page computes its own live).

The page itself draws them: this script serves website/ (the templates' version stamps cut, the
page-view counter left out), opens /mwsr/?soft=1&lab=1 in a headless Chrome on SwiftShader, and
for each case asks the page's lab hook (app.js, labHook) to draw it, then reads back the native
frame (below x1, the Lanczos-3 reference) and every upscaler's 32-bit picture, RGBA8, exactly as
the page shows them. Then, against that frame:

  LPIPS     AlexNet, v0.1 (the lpips package, 8 torch threads: --threads), the RGB in [-1, 1]
  VMAF NEG  libvmaf's vmaf_v0.6.1neg through ffmpeg, both pictures 4:2:0 BT.709 limited range
  PSNR, SSIM (on RGB), MS-SSIM (on luma), FLIP (LDR)   metrics.py

and the overshoot the page measured on SwiftShader (the three-GPU tables' column), as a third value.

    py -E -P cmp_lab.py [--keep DIR] [--only PIC]
    env MW_LPIPS_PY  a Python with torch and lpips (default: this one)
        TORCH_HOME   where AlexNet's weights already are (lpips fetches them otherwise)
        MW_FFMPEG    an ffmpeg with libvmaf (default: ffmpeg on the PATH)
        MW_CHROME    as scaler_lab.py

The HDR pictures and a visitor's own get no score (the page says why): LPIPS and VMAF NEG are SDR
measures, and the lab has the page's pictures only.
"""
import argparse
import functools
import http.server
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from scaler_lab import CHROME, REPO, free_port  # noqa: E402

import websocket  # noqa: E402

SITE = os.path.join(REPO, "website")
OUT_JSON = os.path.join(HERE, "lab-scores.json")
PICS = ["junkshop", "0ad", "gears", "text", "iso"]
SCALES = ["0.5", "0.75", "1", "1.33", "2"]
CHROMAS = [("444", False), ("444", True), ("420", False)]
STAMP = re.compile(r"\?v=\{\{\(fileStat `[^`]*`\)\.ModTime\.Unix\}\}")
COUNTER = re.compile(r'<script defer src="https://stats\.moonlightweb\.top/script\.js"[^>]*></script>', re.S)
LPIPS_SCRIPT = r'''
import json, sys, warnings
warnings.filterwarnings("ignore")
import numpy as np, torch, lpips
from PIL import Image
torch.set_num_threads(int(sys.argv[1]))
m = lpips.LPIPS(net="alex", verbose=False)
def t(p):
    a = np.asarray(Image.open(p).convert("RGB"), dtype=np.float32) / 127.5 - 1.0
    return torch.from_numpy(a).permute(2, 0, 1)[None]
gt = t(sys.argv[2])
res = {}
with torch.no_grad():
    for p in sys.argv[3:]:
        res[p] = float(m(gt, t(p)))
print(json.dumps(res))
'''


class Site(http.server.SimpleHTTPRequestHandler):
    """website/ as the site serves it, minus its templates and its counter; POST /__lab/<file> keeps
    a picture the page sends."""

    def log_message(self, *a):
        pass

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def do_GET(self):
        path = self.path.split("?", 1)[0].split("#", 1)[0]
        if path.endswith("/"):
            path += "index.html"
        if path.endswith(".html"):
            f = os.path.join(SITE, *path.lstrip("/").split("/"))
            if not os.path.isfile(f):
                self.send_error(404)
                return
            body = COUNTER.sub("", STAMP.sub("", open(f, encoding="utf-8").read())).encode("utf-8")
            self.send_response(200)
            self.send_header("Content-Type", "text/html; charset=utf-8")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            return
        super().do_GET()

    def do_POST(self):
        if not self.path.startswith("/__lab/"):
            self.send_error(404)
            return
        name = os.path.basename(self.path[len("/__lab/"):])
        n = int(self.headers["Content-Length"])
        data = self.rfile.read(n)
        with open(os.path.join(self.server.lab_dir, name), "wb") as f:
            f.write(data)
        self.send_response(204)
        self.end_headers()


def classics(ref, out):
    """PSNR, SSIM on RGB, MS-SSIM on luma, FLIP."""
    from skimage.metrics import structural_similarity
    import metrics
    a, b = metrics.load(ref), metrics.load(out)
    flip_mean, _ = metrics.flip_ldr(a, b)
    return [
        round(metrics.psnr(a, b), 4),
        round(float(structural_similarity(a, b, data_range=1.0, channel_axis=2, gaussian_weights=True, sigma=1.5, use_sample_covariance=False)), 6),
        round(metrics.ms_ssim(metrics.luma(a).astype(np.float64), metrics.luma(b).astype(np.float64)), 6),
        None if flip_mean is None else round(flip_mean, 6),
    ]


def vmaf_neg(ffmpeg, dist, ref):
    lav = ("[0:v]scale=out_color_matrix=bt709:out_range=tv,format=yuv420p[d];"
           "[1:v]scale=out_color_matrix=bt709:out_range=tv,format=yuv420p[r];"
           "[d][r]libvmaf=n_threads=4:model=version=vmaf_v0.6.1neg")
    r = subprocess.run([ffmpeg, "-hide_banner", "-nostats", "-i", dist, "-i", ref, "-lavfi", lav, "-f", "null", "-"],
                       capture_output=True, text=True)
    m = re.search(r"VMAF score: ([\d.]+)", r.stderr)
    if not m:
        raise SystemExit("VMAF NEG: " + r.stderr[-400:])
    return float(m.group(1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--keep", help="keep the pictures in this directory (default: a temporary one, removed)")
    ap.add_argument("--only", help="one picture (a trial): its scores are printed, lab-scores.json is not written")
    ap.add_argument("--scales", default=",".join(SCALES), help="a trial's scales (keys as the page's: 1.33, 2...)")
    ap.add_argument("--threads", type=int, default=8, help="LPIPS's torch threads (8: the second sweep's)")
    a = ap.parse_args()
    lpips_py = os.environ.get("MW_LPIPS_PY", sys.executable)
    ffmpeg = os.environ.get("MW_FFMPEG", "ffmpeg")
    work = a.keep or tempfile.mkdtemp(prefix="mw-cmp-lab-")
    os.makedirs(work, exist_ok=True)

    port = free_port()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", port), functools.partial(Site, directory=SITE))
    server.lab_dir = work
    threading.Thread(target=server.serve_forever, daemon=True).start()
    cdp = free_port()
    profile = tempfile.mkdtemp(prefix="mw-cmp-")
    chrome = subprocess.Popen([CHROME, "--headless=new", "--remote-debugging-port=%d" % cdp,
                               "--user-data-dir=" + profile, "--enable-unsafe-webgpu", "--enable-unsafe-swiftshader",
                               "--mute-audio", "--no-first-run", "--no-default-browser-check",
                               "--disable-background-timer-throttling", "--disable-renderer-backgrounding",
                               "about:blank"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    scores = {}
    try:
        page = None
        for _ in range(50):
            try:
                targets = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % cdp))
                page = next(t for t in targets if t["type"] == "page")
                break
            except Exception:  # noqa: BLE001
                time.sleep(0.2)
        if not page:
            raise SystemExit("no page on the DevTools port %d" % cdp)
        ws = websocket.create_connection(page["webSocketDebuggerUrl"], timeout=1800, suppress_origin=True)
        ids = iter(range(1, 1 << 30))

        def call(method, **params):
            mid = next(ids)
            ws.send(json.dumps({"id": mid, "method": method, "params": params}))
            while True:
                msg = json.loads(ws.recv())
                if msg.get("id") == mid:
                    if "error" in msg:
                        raise SystemExit("%s: %s" % (method, msg["error"]))
                    return msg["result"]

        def js(expr):
            r = call("Runtime.evaluate", expression=expr, awaitPromise=True, returnByValue=True)
            if "exceptionDetails" in r:
                raise SystemExit("page: " + json.dumps(r["exceptionDetails"])[:600])
            return r["result"].get("value")

        call("Page.navigate", url="http://127.0.0.1:%d/mwsr/?soft=1&lab=1" % port)
        for _ in range(600):
            if js("!!window.mwsrLab"):
                break
            time.sleep(0.5)
        else:
            raise SystemExit("the page's lab hook never came (status: %r)" % js("document.getElementById('status').textContent"))
        kernels = js("mwsrLab.kernels()")
        for pic in ([a.only] if a.only else PICS):
            for sc in a.scales.split(","):
                for chroma, c444 in CHROMAS:
                    key = "%s|%s|%s|%d" % (pic, sc, chroma, int(c444))
                    t0 = time.time()
                    info = js("mwsrLab.run(%s, %s, %s, %s)" % (json.dumps(pic), json.dumps(sc), json.dumps(chroma),
                                                              "true" if c444 else "false"))
                    if not info["native"] or info["hdr"]:
                        raise SystemExit("%s: no native frame, or HDR" % key)
                    w, h = info["outW"], info["outH"]
                    stem = key.replace("|", "_")
                    for who in ["ref"] + kernels:
                        get = "mwsrLab.ref()" if who == "ref" else "await mwsrLab.read(%s)" % json.dumps(who)
                        js("(async () => { const px = %s; await fetch('/__lab/%s_%s.rgba', {method: 'POST', body: px}); })()"
                           % (get, stem, who))
                        raw = os.path.join(work, "%s_%s.rgba" % (stem, who))
                        px = np.fromfile(raw, dtype=np.uint8)
                        if px.size != w * h * 4:
                            raise SystemExit("%s %s: %d bytes for %dx%d" % (key, who, px.size, w, h))
                        Image.fromarray(px.reshape(h, w, 4)[..., :3]).save(raw[:-5] + ".png")
                        os.remove(raw)
                    ref = os.path.join(work, stem + "_ref.png")
                    outs = [os.path.join(work, "%s_%s.png" % (stem, k)) for k in kernels]
                    r = subprocess.run([lpips_py, "-I", "-c", LPIPS_SCRIPT, str(a.threads), ref] + outs,
                                       capture_output=True,
                                       text=True, check=True)
                    lp = json.loads(r.stdout.strip().splitlines()[-1])
                    over = js("Object.fromEntries(mwsrLab.kernels().map((k) => [k, mwsrLab.overshoot(k)]))")
                    scores[key] = {k: [round(lp[o], 6), round(vmaf_neg(ffmpeg, o, ref), 4), over[k], *classics(ref, o)]
                                   for k, o in zip(kernels, outs)}
                    print("%-22s %4.0f s  %s" % (key, time.time() - t0, "  ".join(
                        "%s %.4f/%.2f" % (k, *scores[key][k][:2]) for k in kernels)), flush=True)
        ws.close()
    finally:
        chrome.terminate()
        try:
            chrome.wait(timeout=10)
        except subprocess.TimeoutExpired:
            chrome.kill()
        server.shutdown()
        shutil.rmtree(profile, ignore_errors=True)
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)
    if a.only:
        return
    doc = {
        "measured": time.strftime("%d/%m/%Y"),
        "how": "scripts/bench/scaler-lab/cmp_lab.py: this page on SwiftShader, LPIPS (alex, v0.1), VMAF NEG "
               "(vmaf_v0.6.1neg, 4:2:0 BT.709), PSNR, SSIM, MS-SSIM and FLIP against the native frame, or the "
               "Lanczos-3 reference below x1",
        "keys": "picture|scale|chroma|chroma444 -> upscaler: [LPIPS, VMAF NEG, the page's overshoot on SwiftShader, "
                "PSNR, SSIM, MS-SSIM, FLIP]",
        "scores": scores,
    }
    with open(OUT_JSON, "w", encoding="utf-8", newline="\n") as f:
        json.dump(doc, f, indent=1, sort_keys=True)
        f.write("\n")
    print("wrote", OUT_JSON, len(scores), "cases", flush=True)


if __name__ == "__main__":
    main()
