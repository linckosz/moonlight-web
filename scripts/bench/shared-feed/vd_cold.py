"""A guest opens an invitation on "MoonlightWeb Virtual Display" with no owner
stream (f9b47174, bench §8q.5): the display must come on, the guest must get
a picture, and it must go off once the guests leave.

    python vd_cold.py [--second] [--owner-other]

The virtual display becomes the primary screen while it is on: say so to
whoever sits at the machine first, and check monitors.ps1 before and after.

--second: a second guest joins while the first watches (no new mode, no
          second operation).
--owner-other: the owner then streams Display 1 (another app of this host)
          and the guests leave: the virtual display must still go off.
"""
import argparse
import base64
import os
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
import run, drive, fleet  # noqa: E402
import guest_link as gl  # noqa: E402

SCR = os.path.join(os.path.dirname(os.path.dirname(BENCH)), "bench-out", "shared-feed")
HOOK = open(os.path.join(HERE, "hook.js"), encoding="utf-8").read()
NATIVE = "moonlightweb-native-host"
VD_APP = 1000
PORT = 18080


def log(*a):
    print(time.strftime("%H:%M:%S"), *a, flush=True)


def monitors():
    return subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(BENCH, "monitors.ps1")],
                          capture_output=True, text=True).stdout.strip().replace("\n", " | ")


def vd(d):
    return d.json_eval("(async () => JSON.stringify(await (await fetch("
                       "'/api/native/virtual-display', {credentials: 'same-origin'})).json()))()")


def open_row(d, slot):
    """The row, on the native host's virtual display. Its link and PIN stay in
    memory (guest_link); only share.info may be printed."""
    return gl.activate(d, slot, {"host_uuid": NATIVE, "app_id": VD_APP})


def close_row(d, slot):
    return gl.deactivate(d, slot)


class Guest:
    def __init__(self, slot, rect, luid):
        self.slot, self.rect, self.luid = slot, rect, luid
        self.port = 9340 + slot
        self.profile = ".chrome-guest-%d" % slot
        self.d = None

    def join(self, base, share):
        gl.open_kiosk(self.port, self.profile, self.rect, ["-AdapterLuid", self.luid, "-Windowed"])
        self.d = drive.Driver(self.port)
        gl.go(self.port, base, share)
        self.d.call("Page.addScriptToEvaluateOnNewDocument", source=HOOK)
        self.d.eval(HOOK)
        if not self.wait("!!document.querySelector('.player-pin-input')", 40):
            raise SystemExit("guest %d: no PIN field" % self.slot)
        gl.enter_pin(self.d, share)
        if not self.wait("!!document.querySelector('.player-join-btn')", 40):
            raise SystemExit("guest %d: no Join button" % self.slot)
        self.d.eval(HOOK)
        pos = self.d.json_eval("""(() => { const e = document.querySelector('.player-join-btn');
            const r = e.getBoundingClientRect();
            return JSON.stringify({x: r.left + r.width / 2, y: r.top + r.height / 2}); })()""")
        t0 = time.time()
        for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
            self.d.call("Input.dispatchMouseEvent", type=kind, x=pos["x"], y=pos["y"],
                        button="left", clickCount=1)
            time.sleep(0.05)
        got = self.wait("!!document.querySelector('canvas')", 60)
        return got, time.time() - t0

    def wait(self, js, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                if "true" in str(self.d.eval("JSON.stringify(%s)" % js)).lower():
                    return True
            except drive.PassFailed:
                pass
            time.sleep(0.5)
        return False

    def state(self):
        return self.d.json_eval("""(() => { const c = document.querySelector('canvas');
            const err = document.querySelector('.player-error, .player-hint, .toast');
            return JSON.stringify({canvas: c ? [c.width, c.height] : null,
                joins: window.__mwS0 ? window.__mwS0.joins : null,
                text: err ? err.textContent.trim().slice(0, 200) : null}); })()""")

    def shot(self, name):
        png = self.d.call("Page.captureScreenshot", format="png")
        data = png.get("data") if isinstance(png, dict) else None
        if data:
            os.makedirs(SCR, exist_ok=True)
            path = os.path.join(SCR, name)
            with open(path, "wb") as f:
                f.write(base64.b64decode(data))
            return path

    def close(self):
        """Its Chrome closes and its profile goes: '' or a warning."""
        return gl.close_kiosk(self.profile)


def wait_vd(d, active, secs):
    end = time.time() + secs
    while time.time() < end:
        st = vd(d)
        if bool(st.get("active")) == active:
            return st
        time.sleep(1)
    return vd(d)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--second", action="store_true")
    ap.add_argument("--owner-other", action="store_true")
    a = ap.parse_args()

    log("monitors before:", monitors())
    amd = next(g for g in run.list_gpus() if "AMD" in g[0])
    luid = amd[1]
    probe = fleet.probe("local")
    # In memory only (fleet.mint_pin).
    pin = fleet.mint_pin("local", probe)
    access = dict(run.access_map().get("local") or {})
    base = fleet.lan_url("local", probe) or access.get("lan")
    log("base", base, "guest decodes on", amd[0], luid)

    started = time.time()
    gl.keep_output()
    warnings = []
    run.kiosk_start(base, on_gpu="AMD")
    d = drive.Driver(run.DEBUG_PORT)
    guests = []
    # The rows this run asked to open: each is closed in the finally, whatever happened.
    rows = []
    try:
        d.navigate(base)
        d.eval("(async () => { await caches.delete('mw-shell'); for (const r of await "
               "navigator.serviceWorker.getRegistrations()) await r.unregister(); return 1; })()")
        d.navigate(base)
        if not d.wait_library(access.get("name", "bench"), pin or access.get("pin", ""), tries=25):
            raise SystemExit("owner never reached the library")
        log("virtual display before:", vd(d))

        rows.append(2)
        share = open_row(d, 2)
        log("row 2 opened cold:", share.info)
        g2 = Guest(2, (2560 + 1280, 720, 1280, 720), luid)
        guests.append(g2)
        ok, secs = g2.join(base, share)
        share = None
        log("guest 2 canvas:", ok, "after %.1f s" % secs, g2.state())
        log("virtual display now:", vd(d))
        log("monitors now:", monitors())
        time.sleep(5)
        p = g2.shot("vd-cold-guest2.png")
        log("guest 2 screenshot:", p, g2.state())

        if a.second:
            rows.append(3)
            share = open_row(d, 3)
            log("row 3 opened:", share.info.get("status"))
            g3 = Guest(3, (2560 + 1280, 0, 1280, 720), luid)
            guests.append(g3)
            ok3, secs3 = g3.join(base, share)
            share = None
            log("guest 3 canvas:", ok3, "after %.1f s" % secs3, g3.state())
            log("virtual display now:", vd(d))
            time.sleep(5)
            log("guest 3 screenshot:", g3.shot("vd-cold-guest3.png"))

        if a.owner_other:
            os.environ["MW_BENCH_DISPLAY"] = "0"
            card, app = d.pick_tile("display")
            log("owner launches", app.get("name"), "while the guests watch the virtual display")
            d.launch(card, app)
            d.wait_picture(timeout=60)
            time.sleep(8)
            log("owner streaming; virtual display:", vd(d).get("active"))

        for g in reversed(guests):
            closed = close_row(d, g.slot)
            log("row %d closed:" % g.slot, closed)
            if closed:
                rows.remove(g.slot)
            left = g.close()
            if left:
                warnings.append(left)
        guests = []
        st = wait_vd(d, False, 40)
        log("virtual display after the guests left:", st)
        if a.owner_other:
            d.stop()
            log("owner stopped")
    finally:
        for slot in rows:
            if not close_row(d, slot):
                warnings.append("slot %d may still be open: close it on the share board" % slot)
        for g in guests:
            left = g.close()
            if left:
                warnings.append(left)
        run.kiosk_stop()
        gl.proof(SCR, started)
        for w in warnings:
            log("WARNING:", w)
    time.sleep(3)
    log("monitors after:", monitors())
    for w in warnings:
        log("WARNING:", w)


if __name__ == "__main__":
    main()
