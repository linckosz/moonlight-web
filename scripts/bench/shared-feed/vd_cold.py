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
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
import run, drive, fleet  # noqa: E402

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
    return d.json_eval("""(async () => {
        const r = await fetch('/api/share/slots/%d/activate', {method: 'POST',
            credentials: 'same-origin', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({ttl_secs: 3600, host_uuid: '%s', app_id: %d})});
        return JSON.stringify(Object.assign({status: r.status}, await r.json()));
    })()""" % (slot, NATIVE, VD_APP))


def close_row(d, slot):
    return d.eval("fetch('/api/share/slots/%d/deactivate', {method: 'POST', credentials: "
                  "'same-origin', headers: {'Content-Type': 'application/json'}, body: '{}'})"
                  ".then(r => r.status)" % slot)


class Guest:
    def __init__(self, slot, rect, luid):
        self.slot, self.rect, self.luid = slot, rect, luid
        self.port = 9340 + slot
        self.profile = ".chrome-guest-%d" % slot
        self.d = None

    def join(self, base, act):
        m = re.search(r"/p/([^/?#]+)", act.get("url", "")) or re.search(r"#t=([^&]+)", act.get("url", ""))
        url = base.rstrip("/") + "/p/" + m.group(1)
        x, y, w, h = self.rect
        subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(BENCH, "kiosk.ps1"),
                        "-Url", url, "-X", str(x), "-Y", str(y), "-W", str(w), "-H", str(h),
                        "-DebugPort", str(self.port), "-ChromeProfile", self.profile,
                        "-AdapterLuid", self.luid, "-Windowed"], capture_output=True, text=True)
        self.d = drive.Driver(self.port)
        self.d.call("Page.addScriptToEvaluateOnNewDocument", source=HOOK)
        self.d.eval(HOOK)
        if not self.wait("!!document.querySelector('.player-pin-input')", 40):
            raise SystemExit("guest %d: no PIN field" % self.slot)
        self.d.eval("(() => { const i = document.querySelector('.player-pin-input'); i.value = %s; "
                    "i.form.requestSubmit(); return 1; })()" % json.dumps(act["pin"]))
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
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
                        "Where-Object { $_.CommandLine -like '*%s*' } | "
                        "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }" % self.profile],
                       capture_output=True, text=True)


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

    run.kiosk_start(base, on_gpu="AMD")
    d = drive.Driver(run.DEBUG_PORT)
    guests = []
    try:
        d.navigate(base)
        d.eval("(async () => { await caches.delete('mw-shell'); for (const r of await "
               "navigator.serviceWorker.getRegistrations()) await r.unregister(); return 1; })()")
        d.navigate(base)
        if not d.wait_library(access.get("name", "bench"), pin or access.get("pin", ""), tries=25):
            raise SystemExit("owner never reached the library")
        log("virtual display before:", vd(d))

        act = open_row(d, 2)
        log("row 2 opened cold:", {k: act.get(k) for k in ("status", "app_id", "host_uuid", "state")})
        g2 = Guest(2, (2560 + 1280, 720, 1280, 720), luid)
        guests.append(g2)
        ok, secs = g2.join(base, act)
        log("guest 2 canvas:", ok, "after %.1f s" % secs, g2.state())
        log("virtual display now:", vd(d))
        log("monitors now:", monitors())
        time.sleep(5)
        p = g2.shot("vd-cold-guest2.png")
        log("guest 2 screenshot:", p, g2.state())

        if a.second:
            act3 = open_row(d, 3)
            log("row 3 opened:", act3.get("status"))
            g3 = Guest(3, (2560 + 1280, 0, 1280, 720), luid)
            guests.append(g3)
            ok3, secs3 = g3.join(base, act3)
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
            log("row %d closed:" % g.slot, close_row(d, g.slot))
            g.close()
        st = wait_vd(d, False, 40)
        log("virtual display after the guests left:", st)
        if a.owner_other:
            d.stop()
            log("owner stopped")
    finally:
        for g in guests:
            g.close()
        run.kiosk_stop()
    time.sleep(3)
    log("monitors after:", monitors())


if __name__ == "__main__":
    main()
