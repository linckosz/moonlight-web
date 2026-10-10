"""The page's GPU wait away from any stream (POC Ultra U3.7, the "attente"
plan's B1): gpuwait-lab.html in a Chrome of its own, case after case.

    python gpuwait_lab.py --tag rtx [--case NAME:QUERY ...] [--rounds 2]
                          [--chrome-arg=--use-adapter-luid=0,<low>] [--headed X,Y,W,H]

Each case is a query of gpuwait-lab.js (mode, pace, wait, n, period, draw,
thread...); without --case, the default set below. Cases run in turn, and
with --rounds 2 a second time in reverse order (ABBA). Each run writes
<dir>/<tag>-<case>[-rN].ultratrace.json, which scripts/bench/clickpath/
gpuwait.py reads (`--dir`), and a line here: submit → work done, and, from
the GPU's timestamps, how much of it is before the GPU starts, the GPU at
work, and after it ends (the two clocks put together as gpuwait.py does, at
the middle of their bounds).

The page is served cross-origin isolated, so that performance.now() counts
in µs, not in the 100 µs steps a pass's trace has. A remote Chrome (the
UM790Pro's): --remote-cdp and --http-port, through SSH tunnels, as
decoder_lab.py. Needs `pip install websocket-client`.

--page present-lab.html runs UltraPlayer itself instead, presented as a
stream is (B2.1, present-lab.js: present=vf|dom|offscreen), and --shot
keeps a screenshot of each case (<run>.png, Pillow): its mean level and its
mean difference to the first case's, the picture each way leaves on the
screen. With --chrome-arg=--window-size=1920,1080 a 1080p frame is drawn 1:1.
"""
import argparse
import base64
import functools
import http.server
import json
import os
import statistics
import subprocess
import sys
import tempfile
import threading
import time
import urllib.request

import websocket

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "clickpath"))
from decoder_lab import CHROME, REPO, Handler, free_port  # noqa: E402
import gpuwait  # noqa: E402

DEFAULT_CASES = [
    "empty-back:mode=empty&pace=back&n=1500",
    "stamp-back:mode=stamp&pace=back&n=1500",
    "stamp-tick:mode=stamp&pace=tick&n=1500",
    "map-back:mode=stamp&pace=back&wait=map&n=1500",
    "decode-back:mode=decode&pace=back&n=1500",
    "decode-tick:mode=decode&pace=tick&n=1500",
    "decode-tick-worker:mode=decode&pace=tick&n=1500&thread=worker",
]


class IsolatedHandler(Handler):
    def end_headers(self):
        self.send_header("Cross-Origin-Opener-Policy", "same-origin")
        self.send_header("Cross-Origin-Embedder-Policy", "require-corp")
        super().end_headers()


def q(xs, p):
    return gpuwait.q(xs, p)


def brief(trace):
    """submit → done, and its split by the GPU's timestamps, ms (p50)."""
    frames = [r for r in trace if not r.get("ref") and r.get("t2") is not None]
    refs = [r for r in trace if r.get("ref") and r.get("t2") is not None]
    main = frames or refs
    out = {"n": len(main), "done": q([r["t2"] - r["t1"] for r in main], .5),
           "done90": q([r["t2"] - r["t1"] for r in main], .9)}
    timed = [r for r in frames if r.get("gpu") and len(r["gpu"]) == 4]
    timed_refs = [r for r in refs if r.get("gpu") and len(r["gpu"]) == 2]
    both = timed + timed_refs
    if both:
        slope = gpuwait.drift(both)
        off = gpuwait.offsets(both, 2000, slope)
        sel = timed or timed_refs
        mid = lambda r: (off[id(r)][0] + off[id(r)][1]) / 2 + slope * r["t1"]  # noqa: E731
        out["start"] = q([r["gpu"][0] / 1e6 + mid(r) - r["t1"] for r in sel], .5)
        out["gpu"] = q([(r["gpu"][-1] - r["gpu"][0]) / 1e6 for r in sel], .5)
        out["tail"] = q([r["t2"] - (r["gpu"][-1] / 1e6 + mid(r)) for r in sel], .5)
        out["bounds"] = q([off[id(r)][1] - off[id(r)][0] for r in sel], .5)
    if frames and refs:
        out["ref_done"] = q([r["t2"] - r["t1"] for r in refs], .5)
    spins = [r["spin"] for r in main if "spin" in r]
    if spins:
        out["spin"] = q(spins, .5)
    return out


def shot_stats(path, ref):
    """A screenshot's mean level (0-255) and its mean difference to @p ref's, or None."""
    from PIL import Image, ImageChops, ImageStat
    img = Image.open(path).convert("RGB")
    level = sum(ImageStat.Stat(img).mean) / 3
    if not ref:
        return level, None
    other = Image.open(ref).convert("RGB")
    if other.size != img.size:
        return level, None
    return level, sum(ImageStat.Stat(ImageChops.difference(img, other)).mean) / 3


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", required=True)
    ap.add_argument("--page", default="gpuwait-lab.html", help="the lab page (present-lab.html: B2.1)")
    ap.add_argument("--shot", action="store_true", help="a screenshot after each case, compared to the first")
    ap.add_argument("--case", action="append", default=[], help="NAME:QUERY (gpuwait-lab.js parameters)")
    ap.add_argument("--rounds", type=int, default=1, help="2: the cases again in reverse order (ABBA)")
    ap.add_argument("--dir", default=os.path.join(REPO, "bench-out", "ultra-lab"))
    ap.add_argument("--chrome-arg", action="append", default=[])
    ap.add_argument("--headed", default="", help="X,Y,W,H: a shown window there instead of headless")
    ap.add_argument("--remote-cdp", type=int, default=0, help="DevTools port of an already running Chrome")
    ap.add_argument("--http-port", type=int, default=0, help="fixed port for the server (remote runs)")
    a = ap.parse_args()
    os.makedirs(a.dir, exist_ok=True)
    cases = [c.split(":", 1) for c in (a.case or DEFAULT_CASES)]
    order = []
    for r in range(a.rounds):
        seq = cases if r % 2 == 0 else list(reversed(cases))
        order += [(name, query, r) for name, query in seq]

    http_port = a.http_port or free_port()
    server = http.server.ThreadingHTTPServer(
        ("127.0.0.1", http_port), functools.partial(IsolatedHandler, directory=REPO))
    threading.Thread(target=server.serve_forever, daemon=True).start()

    cdp_port = a.remote_cdp or free_port()
    profile = tempfile.mkdtemp(prefix="mw-gpuwait-lab-")
    window = []
    if a.headed:
        x, y, w, h = a.headed.split(",")
        window = ["--window-position=%s,%s" % (x, y), "--window-size=%s,%s" % (w, h)]
    chrome = None if a.remote_cdp else subprocess.Popen([
        CHROME, *([] if a.headed else ["--headless=new"]), "--remote-debugging-port=%d" % cdp_port,
        "--user-data-dir=" + profile, "--no-first-run", "--no-default-browser-check",
        "--enable-unsafe-webgpu", "--enable-webgpu-developer-features", *window, "about:blank",
    ] + a.chrome_arg, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                targets = json.load(urllib.request.urlopen("http://127.0.0.1:%d/json" % cdp_port))
                page = next(t for t in targets if t["type"] == "page")
                break
            except Exception:
                time.sleep(0.2)
        ws = websocket.create_connection(page["webSocketDebuggerUrl"], timeout=900, suppress_origin=True)
        ids = iter(range(1, 1 << 30))

        def call(method, **params):
            mid = next(ids)
            ws.send(json.dumps({"id": mid, "method": method, "params": params}))
            while True:
                msg = json.loads(ws.recv())
                if msg.get("id") == mid:
                    return msg.get("result", msg)

        call("Performance.enable")

        def task_ms():
            m = call("Performance.getMetrics").get("metrics", [])
            return 1000 * next((x["value"] for x in m if x["name"] == "TaskDuration"), 0.0)

        print("%-22s %-28s %6s %7s %7s | %6s %6s %6s (bounds) | %-9s | %s" % (
            "case", "adapter", "i/s", "done50", "done90", "start", "gpu", "tail", "empty ref",
            "main thread ms/frame, nudges/frame, spin p50"))
        first_shot = None
        for name, query, rnd in order:
            url = "http://127.0.0.1:%d/scripts/bench/ultra/%s?%s" % (http_port, a.page, query)
            call("Page.navigate", url=url)
            busy0 = task_ms()
            for _ in range(240):
                time.sleep(0.5)
                ready = call("Runtime.evaluate", returnByValue=True,
                             expression="location.href === %s && window.__labResult !== undefined"
                             % json.dumps(url))
                if ready.get("result", {}).get("value"):
                    break
            r = call("Runtime.evaluate", expression="window.__labResult", awaitPromise=True,
                     returnByValue=True)
            res = r.get("result", {}).get("value") or {"error": json.dumps(r)[:2000]}
            busy = task_ms() - busy0
            if "error" in res:
                print("%-22s ERROR %s" % (name, res["error"][:600]))
                continue
            run = "%s-%s%s" % (a.tag, name, "-r%d" % rnd if a.rounds > 1 else "")
            extra = ""
            if a.shot:
                png = os.path.join(a.dir, run + ".png")
                with open(png, "wb") as f:
                    f.write(base64.b64decode(call("Page.captureScreenshot", format="png")["data"]))
                level, diff = shot_stats(png, first_shot)
                first_shot = first_shot or png
                res["shot"] = {"level": level, "diffToFirst": diff}
                extra += "  shot level %.1f%s" % (level, "" if diff is None else ", diff %.2f" % diff)
            if res.get("submitToHanded"):
                extra += "  handed p50 %s, shown %d, replaced %d" % (
                    res["submitToHanded"]["p50"], res["shown"], res["replaced"])
            trace = res.pop("trace")
            with open(os.path.join(a.dir, run + ".ultratrace.json"), "w") as f:
                json.dump(trace, f)
            b = brief(trace)
            b["mainMsPerFrame"] = busy / max(1, b["n"])
            nd = res["params"].get("nudge")
            b["nudgesPerFrame"] = nd["count"] / max(1, b["n"]) if nd else 0
            res["brief"] = b
            with open(os.path.join(a.dir, run + ".json"), "w") as f:
                json.dump(res, f, indent=1)
            fmt = lambda k: ("%6.2f" % b[k]) if b.get(k) is not None else "     -"  # noqa: E731
            print("%-22s %-28s %6.1f %7.3f %7.3f | %s %s %s (%s) | %-9s | %5.2f %5.1f %s%s%s%s" % (
                name + ("/%d" % rnd if a.rounds > 1 else ""), res["adapter"][:28], res["perSecond"],
                b["done"], b["done90"], fmt("start"), fmt("gpu"), fmt("tail"), fmt("bounds").strip(),
                fmt("ref_done").strip(), b["mainMsPerFrame"], b["nudgesPerFrame"], fmt("spin").strip(),
                "" if res.get("isolated") else "  (not isolated: 0.1 ms steps)", extra,
                ("  errors: %s" % res["errors"][:2]) if res.get("errors") else ""))
            sys.stdout.flush()
    finally:
        if chrome:
            chrome.kill()
        server.shutdown()


if __name__ == "__main__":
    main()
