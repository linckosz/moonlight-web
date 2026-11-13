"""Non-regression of the shared feed (plan « flux commun des invités », S9,
bench §8q.5): a share from a Sunshine host and from a Wolf host works as
before — the guest's own session, the three quality buttons, no shared feed.

    python share_nonreg.py --server-log <the --dev instance's --log file>
                           --host <the GameStream host's uuid> --guest-gpu Arc
"""
import argparse
import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
sys.path.insert(0, HERE)
import run, drive, fleet  # noqa: E402
import owner_load as ol  # noqa: E402
import guest_link as gl  # noqa: E402


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--server-log", required=True, help="the --dev instance's log (--log)")
    ap.add_argument("--host", required=True, help="the host's uuid")
    ap.add_argument("--app", default="", help="app name to launch (default: the first tile)")
    ap.add_argument("--client-gpu", default="AMD")
    ap.add_argument("--guest-gpu", default="Arc")
    a = ap.parse_args()

    _gn, guest_luid, guest_rect = ol.gpu(a.guest_gpu)
    probe = fleet.probe("local")
    access = dict(run.access_map().get("local") or {})
    # In memory only (fleet.mint_pin).
    pin = fleet.mint_pin("local", probe) or access.get("pin", "")
    base = fleet.lan_url("local", probe) or access.get("lan")
    mark = os.path.getsize(a.server_log)
    started = time.time()
    gl.keep_output()
    warnings = []

    run.kiosk_start(base, on_gpu=a.client_gpu)
    d = drive.Driver(run.DEBUG_PORT)
    g = None
    try:
        d.navigate(base)
        d.wait_library(access.get("name", "bench"), pin, tries=25)
        card = None
        for _ in range(10):
            inv = d.inventory()
            card = next((c for c in inv.get("cards", []) if c.get("uuid") == a.host), None)
            if card and card.get("apps"):
                break
            time.sleep(3)
        if not card or not card.get("apps"):
            raise SystemExit("no apps on host %s: %s" % (a.host, card))
        app = next((x for x in card["apps"] if a.app and a.app.lower() in x["name"].lower()),
                   card["apps"][0])
        log("owner launches", card.get("name") or a.host, "/", app["name"])
        d.launch(card, app)
        d.wait_picture(timeout=90)
        time.sleep(5)
        log("owner streaming:", {k: v for k, v in (d.stats().get("rows") or {}).items()
                                  if k in ("Codec:", "Framerate:", "Transport:")})

        gx, gy, _w, _h = guest_rect
        # Made before the row opens: the finally closes the row whatever happens.
        g = ol.Guest(2, (gx, gy, ol.GUEST_W, ol.GUEST_H), guest_luid, 60)
        # The link and the PIN stay in memory (guest_link).
        share = gl.activate(d, 2)
        gl.open_kiosk(g.port, g.profile, g.rect, ["-AdapterLuid", guest_luid, "-Windowed"])
        g.d = drive.Driver(g.port)
        gl.go(g.port, base, share)
        g.d.call("Page.addScriptToEvaluateOnNewDocument", source=ol.HOOK)
        g.d.eval(ol.HOOK)
        if not g._wait("!!document.querySelector('.player-pin-input')", 60):
            raise SystemExit("guest: no PIN field")
        gl.enter_pin(g.d, share)
        share = None
        if not g._wait("!!document.querySelector('.player-join-btn')", 60):
            raise SystemExit("guest: no Join button")
        g.d.eval(ol.HOOK)
        page = g.d.json_eval("""(() => JSON.stringify({
            buttons: document.querySelectorAll('.player-quality-btn').length,
            fixed: !!document.querySelector('.player-quality-fixed')}))()""")
        log("join page: quality buttons %d, fixed height shown %s" % (page["buttons"], page["fixed"]))
        pos = g.d.json_eval("""(() => { const e = document.querySelector('.player-join-btn');
            const r = e.getBoundingClientRect();
            return JSON.stringify({x: r.left + r.width / 2, y: r.top + r.height / 2}); })()""")
        t0 = time.time()
        for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
            g.d.call("Input.dispatchMouseEvent", type=kind, x=pos["x"], y=pos["y"],
                     button="left", clickCount=1)
            time.sleep(0.05)
        ok = g._wait("!!document.querySelector('canvas')", 90)
        time.sleep(6)
        c = g.d.json_eval("(() => { const c = document.querySelector('canvas');"
                          " return JSON.stringify(c ? [c.width, c.height] : null); })()")
        log("guest picture %s after %.1f s, canvas %s, joins %s" % (ok, time.time() - t0, c,
                                                                     g.codec().get("joins")))
        with open(a.server_log, "rb") as f:
            f.seek(mark)
            text = f.read().decode("utf-8", "replace")
        feed = [l for l in text.splitlines() if "[SharedFeed]" in l or "feedPipe" in l]
        launch = [l for l in text.splitlines() if "Player slot 2" in l][:3]
        log("shared feed lines since the start: %d" % len(feed))
        for l in launch:
            log("   ", l[l.find("[Session]"):][:160])
    finally:
        if g:
            warnings += g.leave(d)
        try:
            d.stop()
        except Exception:
            pass
        run.kiosk_stop()
        gl.proof(ol.OUT, started)
        for w in warnings:
            log("WARNING:", w)


if __name__ == "__main__":
    main()
