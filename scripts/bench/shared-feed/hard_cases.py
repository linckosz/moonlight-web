"""The shared feed's hard cases (plan « flux commun des invités », S9, bench
§8q.5) on the --dev instance, the owner streaming one display.

    python hard_cases.py --server-log <the --dev instance's --log file>
                         --display-gpu RTX --client-gpu AMD --guest-gpu Arc
                         --vdd DISPLAY333

A  a lone guest leaves and comes back within the feed's 10 s: no relaunch
B  the captured display changes mode under two guests (--vdd: a virtual
   display only, never a physical one): one rebuild of the feed, the guests
   follow its shape
C  the feed's worker is killed (elevated — say so first): relaunched, the
   guests come back by themselves
D  a guest whose browser decodes no HEVC joins: the whole feed goes H.264
"""
import argparse
import glob
import json
import os
import re
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

SERVER_LOG = ""  # --server-log
WORKER_LOGS = ol.WORKER_LOGS


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def server_lines(pattern, since_bytes=0):
    with open(SERVER_LOG, "rb") as f:
        f.seek(since_bytes)
        text = f.read().decode("utf-8", "replace")
    return [l for l in text.splitlines() if re.search(pattern, l)]


def log_size():
    return os.path.getsize(SERVER_LOG)


def feed_worker():
    """(pid, log path) of the newest feed worker whose process still runs."""
    best = None
    for p in glob.glob(os.path.join(WORKER_LOGS, "moonlightweb-worker-*.log")):
        if time.time() - os.path.getmtime(p) > 600:
            continue
        with open(p, encoding="utf-8", errors="replace") as f:
            text = f.read()
        if "[StreamWorker] feed: display" not in text:
            continue
        pid = int(re.search(r"worker-(\d+)", p).group(1))
        alive = subprocess.run(["powershell", "-NoProfile", "-Command",
                                "[bool](Get-Process -Id %d -ErrorAction SilentlyContinue)" % pid],
                               capture_output=True, text=True).stdout.strip() == "True"
        if alive and (best is None or os.path.getmtime(p) > os.path.getmtime(best[1])):
            best = (pid, p)
    return best


def canvas(g):
    try:
        return g.d.json_eval("(() => { const c = document.querySelector('canvas');"
                             " return JSON.stringify(c ? [c.width, c.height] : null); })()")
    except drive.PassFailed:
        return None


def frames_since(g, t_page):
    """Stats messages this guest's page got since page time t_page, with the
    frames each second carried."""
    try:
        rows = g.d.json_eval("JSON.stringify((window.__mwS0 ? window.__mwS0.stats : [])"
                             ".filter(s => s[0] > %f).map(s => [Math.round(s[0]),"
                             " (s[1].stages && s[1].stages.total && s[1].stages.total.n) || 0]))"
                             % t_page)
    except drive.PassFailed:
        return []
    return rows


def page_now(g):
    return float(g.d.eval("performance.now()"))


def set_mode(device, w, h, hz):
    """The virtual display's mode through display-mode.ps1; `device` is its bare
    name (DISPLAY333): the GDI prefix is added here, where no shell eats a
    backslash of it (Git Bash does)."""
    gdi = chr(92) * 2 + "." + chr(92) + device.lstrip(chr(92) + ".")
    out = subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(HERE, "display-mode.ps1"),
                          "-Device", gdi, "-W", str(w), "-H", str(h), "-Hz", str(hz)],
                         capture_output=True, text=True).stdout.strip()
    return out.replace(chr(10), " ")


def leave_and_rejoin(g):
    """The Leave button, then Join again: what a person does."""
    t0 = time.time()
    g.d.eval("(() => { const b = document.getElementById('btn-stream-quit');"
             " if (b) { b.click(); return 'left'; } return 'no button'; })()")
    if not g._wait("!!document.querySelector('.player-join-btn')", 30):
        raise SystemExit("guest %d: no Join button after Leave" % g.slot)
    t_left = time.time() - t0
    pos = g.d.json_eval("""(() => { const e = document.querySelector('.player-join-btn');
        const r = e.getBoundingClientRect();
        return JSON.stringify({x: r.left + r.width / 2, y: r.top + r.height / 2}); })()""")
    t1 = time.time()
    for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
        g.d.call("Input.dispatchMouseEvent", type=kind, x=pos["x"], y=pos["y"],
                 button="left", clickCount=1)
        time.sleep(0.05)
    ok = g._wait("!!document.querySelector('canvas')", 40)
    return t_left, time.time() - t1, ok


def kill_elevated(pid):
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Start-Process powershell -Verb RunAs -WindowStyle Hidden -ArgumentList "
                    "'-NoProfile','-Command','Stop-Process -Id %d -Force'" % pid],
                   capture_output=True, text=True)


def main():
    global SERVER_LOG
    ap = argparse.ArgumentParser()
    ap.add_argument("--server-log", required=True,
                    help="the --dev instance's log (--log): where SharedFeed says what it does")
    ap.add_argument("--display-gpu", default="RTX")
    ap.add_argument("--client-gpu", default="AMD")
    ap.add_argument("--guest-gpu", default="Arc")
    ap.add_argument("--vdd", default="", help="the captured display's name (DISPLAY333), when "
                                              "it is a virtual one whose mode may change (case B)")
    ap.add_argument("--vdd-mode", default="2560,1440,120")
    ap.add_argument("--alt-mode", default="2224,1440,120")
    ap.add_argument("--cases", default="A,B,C,D")
    a = ap.parse_args()
    SERVER_LOG = a.server_log
    cases = set(a.cases.split(","))

    _cn, _cl, _cr = ol.gpu(a.client_gpu)
    _gn, guest_luid, guest_rect = ol.gpu(a.guest_gpu)
    enc_name, _el, enc_rect = ol.gpu(a.display_gpu)
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

    run.kiosk_start(base, on_gpu=a.client_gpu)
    d = drive.Driver(run.DEBUG_PORT)
    guests = {}
    gx, gy, _gw, _gh = guest_rect
    spots = {2: (gx, gy), 3: (gx + ol.GUEST_W, gy), 4: (gx, gy + ol.GUEST_H)}

    def join(slot, no_hevc=False):
        g = ol.Guest(slot, (spots[slot][0], spots[slot][1], ol.GUEST_W, ol.GUEST_H), guest_luid,
                     60, no_hevc)
        t0 = time.time()
        g.join(d, base)
        guests[slot] = g
        log("guest %d in after %.1f s, canvas %s" % (slot, time.time() - t0, canvas(g)))
        return g

    try:
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
        log("owner streams", app.get("name"), "on the", enc_name)
        run.content_start("scroll.html?px=600", probe=False)
        d.launch(card, app)
        d.wait_picture(timeout=60)
        time.sleep(5)

        g2 = join(2)
        time.sleep(5)
        feed = feed_worker()
        log("feed worker", feed)

        if "A" in cases:
            mark = log_size()
            t_left, t_back, ok = leave_and_rejoin(g2)
            time.sleep(3)
            launched = server_lines(r"\[SharedFeed\] (launched|relaunched)", mark)
            stops = server_lines(r"\[SharedFeed\] no guest for", mark)
            log("A  left in %.1f s, back in %.1f s (canvas %s) — feed launches since: %d, "
                "idle stops: %d, same worker: %s" % (t_left, t_back, ok, len(launched), len(stops),
                                                    feed_worker() == feed))

        g3 = join(3)
        time.sleep(5)

        if "B" in cases and a.vdd:
            feed = feed_worker()
            w0, h0, hz0 = [int(x) for x in a.vdd_mode.split(",")]
            w1, h1, hz1 = [int(x) for x in a.alt_mode.split(",")]
            before = open(feed[1], encoding="utf-8", errors="replace").read().count(
                "the stream follows it")
            got = set_mode(a.vdd, w1, h1, hz1)
            time.sleep(8)
            mid = open(feed[1], encoding="utf-8", errors="replace").read().count(
                "the stream follows it")
            sizes = {s: canvas(g) for s, g in guests.items()}
            log("B  display -> %s: feed rebuilds %d, guests' pictures %s, same worker %s"
                % (got, mid - before, sizes, feed_worker() == feed))
            got = set_mode(a.vdd, w0, h0, hz0)
            time.sleep(8)
            after = open(feed[1], encoding="utf-8", errors="replace").read().count(
                "the stream follows it")
            sizes = {s: canvas(g) for s, g in guests.items()}
            log("B  display -> %s: feed rebuilds %d, guests' pictures %s" % (got, after - mid, sizes))

        if "C" in cases:
            feed = feed_worker()
            mark = log_size()
            t_pages = {s: page_now(g) for s, g in guests.items()}
            t_kill = time.time()
            log("C  killing the feed worker", feed[0], "(elevated)")
            kill_elevated(feed[0])
            time.sleep(12)
            died = server_lines(r"\[SharedFeed\] the feed died", mark)
            up = server_lines(r"\[SharedFeed\] (relaunched|feed up)", mark)
            back = server_lines(r"back on the feed", mark)
            log("C  server: %s" % [l[1:24] + l[l.find("[SharedFeed]"):][:90] for l in died + up])
            log("C  subscribers back: %s" % [l[1:24] for l in back])
            for s, g in guests.items():
                rows = frames_since(g, t_pages[s])
                gaps = [rows[i + 1][0] - rows[i][0] for i in range(len(rows) - 1)]
                zero = sum(1 for r in rows if r[1] == 0)
                log("C  guest %d: %d stats seconds since the kill, longest gap %d ms, empty %d, "
                    "canvas %s" % (s, len(rows), max(gaps) if gaps else -1, zero, canvas(g)))
            log("C  new feed worker", feed_worker(), "(was %d), %.1f s after the kill"
                % (feed[0], time.time() - t_kill))

        if "D" in cases:
            mark = log_size()
            g4 = join(4, no_hevc=True)
            time.sleep(12)
            retire = server_lines(r"decodes no HEVC|goes H\.264", mark)
            codecs = {s: g.codec() for s, g in guests.items()}
            log("D  server: %s" % [l[l.find("[SharedFeed]"):][:120] for l in retire])
            for s, c in codecs.items():
                log("D  guest %d joins %s, feedcodec notices %s, canvas %s"
                    % (s, [j[1] for j in (c.get("joins") or [])], c.get("notices"), canvas(guests[s])))
    finally:
        for g in list(guests.values()):
            g.leave(d)
        try:
            d.stop()
        except Exception:
            pass
        run.content_stop()
        run.kiosk_stop()


if __name__ == "__main__":
    main()
