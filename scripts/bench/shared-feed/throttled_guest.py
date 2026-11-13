"""The shared feed's hard case « one guest's link under the feed's floor »
(plan « flux commun des invités », S9, bench §8q.5).

    python throttled_guest.py --shaper-dir <folder of mwshaper.py + WinDivert>
                              --display-gpu Arc --client-gpu AMD --guest-gpu RTX --kbit 3000

Two guests on DualRTX (local Chromes) and one on the N95, in Wi-Fi, all on
the guests' shared feed. The N95's downlink goes through the host-side shaper
(mwshaper.py, WinDivert, elevated — say so first; memory
host-link-shaper-windivert) at --kbit for --secs: the feed must not follow it
below 60 % of its rate, the N95 alone must drop pictures, and the local
guests keep showing 60 per second. Afterwards: `sc stop WinDivert`, elevated.

Each page's shown pictures are counted where the plain Canvas2D renderer
draws them (drawImage on the page's thread); the feed's own rate is read in
its worker's log ("[native] link: ... encoding at").
"""
import argparse
import json
import os
import re
import socket
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
sys.path.insert(0, HERE)
import run, drive, fleet  # noqa: E402
import owner_load as ol  # noqa: E402

SHAPER_LOG = os.path.join(ol.OUT, "shaper.log")
N95_CDP = 9423

DRAW_HOOK = r"""(() => {
    if (window.__mwDraw) return 'already';
    const W = (window.__mwDraw = { n: 0 });
    const orig = CanvasRenderingContext2D.prototype.drawImage;
    CanvasRenderingContext2D.prototype.drawImage = function (...a) {
        W.n++;
        return orig.apply(this, a);
    };
    return 'hooked';
})()"""


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def ctl(line):
    with socket.create_connection(("127.0.0.1", 47250), timeout=5) as s:
        f = s.makefile("rw", encoding="utf-8", newline="\n")
        f.write(line + "\n")
        f.flush()
        return json.loads(f.readline() or "{}")


def shaper_up():
    try:
        return bool(ctl("stats"))
    except OSError:
        return False


def drawn(d):
    try:
        return int(d.eval("window.__mwDraw ? window.__mwDraw.n : -1"))
    except (drive.PassFailed, ValueError, TypeError):
        return -1


def recovery_logs(d):
    try:
        return d.json_eval("JSON.stringify(window.__mwS0 ? window.__mwS0.logs.splice(0) : [])")
    except drive.PassFailed:
        return []


def feed_log():
    import glob
    best = None
    for p in glob.glob(os.path.join(ol.WORKER_LOGS, "moonlightweb-worker-*.log")):
        if time.time() - os.path.getmtime(p) > 300:
            continue
        with open(p, encoding="utf-8", errors="replace") as f:
            if "[StreamWorker] feed: display" in f.read():
                if best is None or os.path.getmtime(p) > os.path.getmtime(best):
                    best = p
    return best


def feed_rates(path, since):
    """(time, kbps) of the feed's governor moves since `since` (epoch)."""
    out = []
    stamp = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(since))
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            if line[1:20] < stamp:
                continue
            m = re.search(r"\[native\] link: (.*?) — encoding at (\d+) kbps", line)
            if m:
                out.append((line[12:24], int(m.group(2)), m.group(1)[:40]))
    return out


def phase(name, pages, secs):
    """Shown pictures per second on every page over `secs`."""
    start = {k: drawn(d) for k, d in pages.items()}
    t0 = time.time()
    time.sleep(secs)
    dt = time.time() - t0
    fps = {k: round((drawn(d) - start[k]) / dt, 1) for k, d in pages.items()}
    logs = {k: len(recovery_logs(d)) for k, d in pages.items()}
    log("%-9s shown fps %s · recovery lines %s" % (name, fps, logs))
    return fps, logs


def n95(cmd, timeout=60):
    return subprocess.run(["ssh", "mw-intel", cmd], stdin=subprocess.DEVNULL,
                          capture_output=True, text=True, timeout=timeout).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--display-gpu", default="Arc")
    ap.add_argument("--client-gpu", default="AMD")
    ap.add_argument("--guest-gpu", default="RTX")
    ap.add_argument("--kbit", type=int, default=3000)
    ap.add_argument("--secs", type=int, default=25)
    ap.add_argument("--shaper-dir", required=True,
                    help="the folder of mwshaper.py, its WinDivert-2.2.2-A beside it")
    ap.add_argument("--lan-url", default="https://192.168.1.66:18443",
                    help="this --dev instance as the N95 reaches it")
    a = ap.parse_args()
    os.makedirs(ol.OUT, exist_ok=True)

    _gn, guest_luid, guest_rect = ol.gpu(a.guest_gpu)
    _en, _el, enc_rect = ol.gpu(a.display_gpu)
    probe = fleet.probe("local")
    access = dict(run.access_map().get("local") or {})
    # In memory only (fleet.mint_pin); the status through the local channel.
    pin = fleet.mint_pin("local", probe) or access.get("pin", "")
    base = fleet.lan_url("local", probe) or access.get("lan")
    import gpu_load
    status = gpu_load.native_status(18080)
    displays = sorted(status.get("displays") or [], key=lambda x: x["id"])
    index = next(i for i, x in enumerate(displays) if a.display_gpu.lower() in x.get("gpu", "").lower())
    os.environ["MW_BENCH_CONTENT_RECT"] = "%d,%d,%d,%d" % enc_rect

    shaper = None
    tunnel = None
    run.kiosk_start(base, on_gpu=a.client_gpu)
    d = drive.Driver(run.DEBUG_PORT)
    guests = {}
    try:
        # The shaper first (elevated, announced): only the N95 and the router
        # it may come back through are shaped; anything else passes and is
        # counted apart.
        # An elevated Start-Process ignores its working directory: the
        # script by its full path. One already up (started by hand) is kept.
        if not shaper_up():
            subprocess.run(["powershell", "-NoProfile", "-Command",
                            "Start-Process py -Verb RunAs -WindowStyle Hidden -ArgumentList "
                            "'\"%s\"','--remote','192.168.1.168,192.168.1.254','--log','\"%s\"'"
                            % (os.path.join(a.shaper_dir, "mwshaper.py"), SHAPER_LOG)],
                           capture_output=True, text=True)
        for _ in range(30):
            if shaper_up():
                shaper = True
                break
            time.sleep(1)
        log("shaper", "up" if shaper else "NOT UP")
        if not shaper:
            raise SystemExit("the shaper did not start")

        d.navigate(base)
        d.eval("(async () => { await caches.delete('mw-shell'); for (const r of await "
               "navigator.serviceWorker.getRegistrations()) await r.unregister(); return 1; })()")
        d.navigate(base)
        d.wait_library(access.get("name", "bench"), pin, tries=25)
        settings = dict(run.load_matrix()["base"])
        settings.update({"stream_fps": 60})
        d.apply_settings(settings)
        d.wait_library(access.get("name", "bench"), pin, tries=25)
        os.environ["MW_BENCH_DISPLAY"] = str(index)
        card, app = d.pick_tile("display")
        log("owner streams", app.get("name"))
        run.content_start("scroll.html?px=600", probe=False)
        d.launch(card, app)
        d.wait_picture(timeout=60)
        time.sleep(5)

        gx, gy, _w, _h = guest_rect
        for i, slot in enumerate((2, 3)):
            g = ol.Guest(slot, (gx + i * ol.GUEST_W, gy, ol.GUEST_W, ol.GUEST_H), guest_luid, 60)
            g.join(d, base)
            g.d.eval(DRAW_HOOK)
            guests[slot] = g
            log("guest %d in (local, %s)" % (slot, a.guest_gpu))

        # The N95's guest: its own Chrome in max's console session, reached
        # through an SSH tunnel to its DevTools port.
        act = d.json_eval("""(async () => {
            const r = await fetch('/api/share/slots/4/activate', {method: 'POST',
                credentials: 'same-origin', headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ttl_secs: 3600})});
            return JSON.stringify(Object.assign({status: r.status}, await r.json()));
        })()""")
        link = act.get("url", "")
        token = (re.search(r"/p/([^/?#]+)", link) or re.search(r"[#&]t=([^&]+)", link)).group(1)
        log("N95 Chrome:", n95("powershell -NoProfile -ExecutionPolicy Bypass -File "
                               r"C:\Users\Public\mw-run\n95-chrome.ps1").strip()[-60:])
        tunnel = subprocess.Popen(["ssh", "-N", "-L", "%d:127.0.0.1:9222" % N95_CDP, "mw-intel"],
                                  stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                                  stderr=subprocess.DEVNULL)
        time.sleep(4)
        nd = drive.Driver(N95_CDP)
        nd.call("Page.addScriptToEvaluateOnNewDocument", source=ol.HOOK)
        nd.navigate(a.lan_url.rstrip("/") + "/p/" + token)
        nd.eval(ol.HOOK)
        g4 = ol.Guest(4, (0, 0, 0, 0), "", 90)
        g4.d = nd
        if not g4._wait("!!document.querySelector('.player-pin-input')", 90):
            raise SystemExit("N95: no PIN field")
        nd.eval("(() => { const i = document.querySelector('.player-pin-input'); i.value = %s; "
                "i.form.requestSubmit(); return 1; })()" % json.dumps(act["pin"]))
        if not g4._wait("!!document.querySelector('.player-join-btn')", 90):
            raise SystemExit("N95: no Join button")
        nd.eval(ol.HOOK)
        pos = nd.json_eval("""(() => { const e = document.querySelector('.player-join-btn');
            const r = e.getBoundingClientRect();
            return JSON.stringify({x: r.left + r.width / 2, y: r.top + r.height / 2}); })()""")
        for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
            nd.call("Input.dispatchMouseEvent", type=kind, x=pos["x"], y=pos["y"],
                    button="left", clickCount=1)
            time.sleep(0.05)
        if not g4._wait("!!document.querySelector('canvas')", 90):
            raise SystemExit("N95: no picture")
        nd.eval(DRAW_HOOK)
        log("guest 4 in (N95, Wi-Fi):", g4.codec().get("joins"))
        time.sleep(10)

        pages = {2: guests[2].d, 3: guests[3].d, 4: nd}
        for p in pages.values():
            recovery_logs(p)
        flog = feed_log()
        log("feed worker log", flog)
        t_base = time.time()
        phase("baseline", pages, 15)
        s0 = ctl("stats")
        t_shape = time.time()
        log("shaper:", ctl("rate %d %d 200" % (a.kbit, a.secs * 1000)))
        phase("throttled", pages, a.secs - 2)
        time.sleep(3)
        s1 = ctl("stats")
        log("shaper counters:", {k: s1.get(k, 0) - s0.get(k, 0) for k in
                                 ("down", "tailDrop", "queued", "queueMax", "other")})
        phase("after", pages, 20)
        rates = feed_rates(flog, t_base) if flog else []
        log("feed rate moves since the baseline:", rates[:20])
        log("feed's lowest rate while shaped:",
            min([r[1] for r in rates], default=None), "kbps")
    finally:
        try:
            ctl("clear")
            ctl("quit")
        except OSError:
            pass
        if tunnel:
            try:
                n95("powershell -NoProfile -ExecutionPolicy Bypass -File "
                    r"C:\Users\Public\mw-run\n95-chrome.ps1 -Stop")
            except Exception:
                pass
            tunnel.terminate()
        for g in list(guests.values()):
            g.leave(d)
        try:
            d.eval("fetch('/api/share/slots/4/deactivate', {method: 'POST', credentials: "
                   "'same-origin', headers: {'Content-Type': 'application/json'}, body: '{}'})")
        except Exception:
            pass
        try:
            d.stop()
        except Exception:
            pass
        run.content_stop()
        run.kiosk_stop()


if __name__ == "__main__":
    main()
