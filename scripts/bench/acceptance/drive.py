"""Drive one acceptance pass in the bench client Chrome, over DevTools.

One pass is: write the settings, reload, unlock if the host asks for a PIN,
click the right tile, wait, screenshot, read the overlay, stop. All of it on a
single CDP connection — six `cdp.py` invocations per pass would mean six
WebSocket handshakes, and a campaign is sixty passes.

`cdp.py` next door stays the tool for driving a pass BY HAND. This module is
what the unattended run uses, and it deliberately imports cdp.py rather than
copying it: the click-by-coordinates rule, the bounded timeouts and the
settings patch are one implementation, in one place.
"""
import base64
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
import cdp  # noqa: E402  — scripts/bench/cdp.py


class PassFailed(Exception):
    """A pass that cannot produce a number. Carries the reason, for the report."""


class NotApplicable(PassFailed):
    """This machine cannot be asked this, and that is not a failure.

    A missing Virtual Display on Linux, a GameStream host that was never
    paired: the cell is grey WITH ITS REASON, never red. Guessing which is
    which from the wording of a message is how a real failure gets excused, so
    the two are different types.
    """


class Driver:
    """One CDP connection, re-established whenever Chrome drops it.

    cdp.py opens a socket per invocation, so it never meets this. Here one
    connection is reused across a whole pass, and a navigation to a different
    origin gives the page a new target: the old socket is closed under us and
    every later call fails with ECONNRESET. Reconnecting on demand is what
    keeps sixty passes on one browser.
    """

    def __init__(self, port=9333):
        self.port = port
        self.c = cdp.Cdp(port)

    def reconnect(self):
        try:
            self.c.ws.close()
        except Exception:
            pass
        last = None
        for attempt in range(5):
            try:
                self.c = cdp.Cdp(self.port)
                return True
            except (Exception, SystemExit) as e:
                last = e
                time.sleep(2 + attempt)
        raise PassFailed("lost the browser and could not reconnect: %s" % last)

    def _retry(self, fn, *args, **kwargs):
        # SystemExit, not just Exception: cdp.py reports a stuck DevTools
        # endpoint with `raise SystemExit(...)`, which sails straight through
        # `except Exception` and takes the whole campaign down with it — a run
        # that had just done twenty good passes died on one slow navigate, with
        # no traceback and no result written.
        try:
            return fn(*args, **kwargs)
        except PassFailed:
            raise
        except (Exception, SystemExit):
            self.reconnect()
            try:
                return fn(*args, **kwargs)
            except SystemExit as e:
                raise PassFailed("the browser stopped answering: %s" % e)

    # ── page plumbing ───────────────────────────────────────────────────────
    def eval(self, js):
        return self._retry(lambda: self.c.eval(js))

    def call(self, method, **params):
        return self._retry(lambda: self.c.call(method, **params))

    def json_eval(self, js):
        """Evaluate JS that returns a JSON string, and parse it.

        Everything this module reads out of the page goes through here: a page
        that answers `undefined` because a view is not mounted yet must fail
        with the JS in the message, not with a KeyError three frames up.
        """
        out = self.c.eval(js)
        try:
            return json.loads(out)
        except (ValueError, TypeError):
            raise PassFailed("page returned %r for %s" % (out, js.strip()[:80]))

    def navigate(self, url):
        self.call("Page.navigate", url=url)
        time.sleep(3)

    def apply_settings(self, settings):
        """Patch localStorage, then reload — the client reads it at launch only.

        mw-lang is pinned to English so the overlay labels this module scrapes
        read the same on every machine of the fleet.
        """
        payload = json.dumps(settings)
        self.eval("""(() => {
            const s = JSON.parse(localStorage.getItem('mw-streaming-settings') || '{}');
            Object.assign(s, %s);
            localStorage.setItem('mw-streaming-settings', JSON.stringify(s));
            localStorage.setItem('mw-lang', 'en');
            localStorage.setItem('mw_perf_diag', '1');
            return JSON.stringify(s);
        })()""" % payload)
        self.call("Page.reload")
        time.sleep(4)

    # ── getting in ──────────────────────────────────────────────────────────
    def unlock(self, machine_name, pin):
        """Fill the PIN page if it is up. Returns True when it submitted one.

        The value is set through the native setter so the app's own listeners
        see it; assigning `.value` updates the DOM and tells nobody.
        """
        if not pin:
            return False
        present = self.eval("JSON.stringify(!!document.getElementById('login-pin-input'))")
        if "true" not in present:
            return False
        self.eval("""(() => {
            const set = (el, v) => {
                const p = Object.getOwnPropertyDescriptor(window.HTMLInputElement.prototype, 'value').set;
                p.call(el, v);
                el.dispatchEvent(new Event('input', { bubbles: true }));
                el.dispatchEvent(new Event('change', { bubbles: true }));
            };
            set(document.getElementById('login-machine-input'), %s);
            set(document.getElementById('login-pin-input'), %s);
            const k = document.getElementById('login-remember');
            if (k && !k.checked) k.click();
            const b = [...document.querySelectorAll('button')].find(e => e.textContent.trim() === 'Unlock');
            if (b) b.click();
            return 'submitted';
        })()""" % (json.dumps(machine_name), json.dumps(pin)))
        time.sleep(8)
        return True

    def wait_library(self, machine_name="bench", pin="", tries=30):
        """Wait until host cards are on screen, unlocking as many times as asked."""
        for _ in range(tries):
            if self.unlock(machine_name, pin):
                continue
            cards = self.eval("JSON.stringify(document.querySelectorAll('.app-card').length)")
            try:
                if int(cards) > 0:
                    return True
            except (ValueError, TypeError):
                pass
            time.sleep(2)
        return False

    # ── what there is to click ──────────────────────────────────────────────
    def inventory(self):
        """Every host card and every tile on it, with the ids the app uses.

        `backendType` is what separates the native host from a Sunshine or a
        Wolf sitting on the same machine; app id 1000 is the Virtual Display
        (NativeHostBackend::kVirtualDisplayAppId) and 1 is the first physical
        display, because a native display's app id is its display id plus one.
        """
        return self.json_eval("""(() => {
            const cards = [...document.querySelectorAll('.host-card')].map(card => ({
                uuid: card.dataset.uuid || '',
                name: card.querySelector('.host-card-name')?.textContent.trim() || '',
                apps: [...card.querySelectorAll('.app-card')].map(a => ({
                    appId: a.dataset.appId || '',
                    name: a.querySelector('.app-card-name')?.textContent.trim() || ''
                }))
            }));
            return JSON.stringify({ cards: cards });
        })()""")

    def host_meta(self):
        """uuid -> what the API knows about each host card.

        `isLocalHost` is the field that matters most here. A bench client that
        has been paired with the whole fleet sees seventeen cards, and picking
        "the Sunshine" by position would just as happily pick a Wolf on another
        machine — the pass would run, the numbers would be real, and they would
        be about the wrong computer.
        """
        try:
            got = self.json_eval("""(async () => {
                const r = await fetch('/api/hosts', { credentials: 'same-origin' });
                const j = await r.json();
                const hosts = Array.isArray(j) ? j : (j.hosts || []);
                const out = {};
                for (const h of hosts) out[h.uuid || h.id] = {
                    name: h.name || '',
                    backendType: h.backendType || '',
                    isLocalHost: !!h.isLocalHost,
                    state: h.state || '',
                    pairState: h.pairState || '',
                    reachable: h.reachable !== false
                };
                return JSON.stringify(out);
            })()""")
            return got if isinstance(got, dict) else {}
        except PassFailed:
            return {}

    def pick_tile(self, target, tries=5, index=None):
        """Turn a matrix `target` into the tile to click, or say why there is none.

        Patient on purpose. A host card appears before its app list does — the
        library is fetched per host, asynchronously — so a card that will offer
        Desktop in two seconds looks like a machine with no Sunshine right now.
        Concluding on the first look marked a working Sunshine as absent.
        """
        last = None
        for attempt in range(tries):
            try:
                return self._pick_tile_once(target, index)
            except PassFailed as e:
                last = e
                time.sleep(3)
        raise last

    def _pick_tile_once(self, target, index=None):
        inv = self.inventory()
        cards = inv.get("cards", [])
        if not cards:
            raise PassFailed("no host card on the page")
        meta = self.host_meta()
        for c in cards:
            info = meta.get(c["uuid"], {})
            c["name"] = c["name"] or info.get("name", "")
            c["backendType"] = info.get("backendType", "")
            c["isLocalHost"] = info.get("isLocalHost", False)
            c["state"] = info.get("state", "")
            c["pairState"] = info.get("pairState", "")

        native = [c for c in cards if c.get("backendType") == "native"]
        # A list whose backendType never came back is still readable: the card
        # that offers app id 1000 is the native one, because only the native
        # host mints a Virtual Display.
        if not native:
            native = [c for c in cards if any(a["appId"] == "1000" for a in c["apps"])]
        # Only this machine's own GameStream host is a candidate for `sunshine`.
        # Every other card is a neighbour this client happens to be paired with.
        others = [c for c in cards
                  if c not in native and c.get("isLocalHost") and c["apps"]
                  and c.get("state") != "offline"]

        if target == "vdisplay":
            for c in native:
                for a in c["apps"]:
                    if a["appId"] == "1000":
                        return c, a
            raise NotApplicable("no MoonlightWeb Virtual Display on this host")
        if target == "display":
            # MW_BENCH_DISPLAY picks another physical display than the first,
            # counted in app-id order: the way to stream a screen driven by a
            # different GPU (DualRTX: 1 is the AMD iGPU's). A pass that names
            # its encoder GPU (`displayGpu`) hands the index in instead.
            if index is None:
                index = int(os.environ.get("MW_BENCH_DISPLAY", "0") or 0)
            for c in native:
                phys = [a for a in c["apps"] if a["appId"].isdigit() and a["appId"] != "1000"]
                if len(phys) > index:
                    phys.sort(key=lambda a: int(a["appId"]))
                    return c, phys[index]
            raise NotApplicable("the native host offers no physical display")
        if target == "sunshine":
            for c in others:
                return c, c["apps"][0]
            # An empty app list on a host that IS there is almost always a
            # pairing that was never done for this edition, and saying "no
            # Sunshine here" would be false. GameStream pairing needs a PIN
            # typed into Sunshine's own web interface, so an unattended bench
            # cannot do it: the cell is grey with this reason, never red.
            unpaired = [c for c in cards
                        if c.get("isLocalHost") and c.get("backendType") != "native"
                        and c.get("pairState") and c["pairState"] != "paired"]
            if unpaired:
                raise NotApplicable(
                    "the local GameStream host %r is %s for this edition, so it "
                    "offers no apps — pairing needs a PIN typed into Sunshine's own "
                    "web interface, which an unattended bench cannot do"
                    % (unpaired[0].get("name") or "?", unpaired[0]["pairState"]))
            raise NotApplicable("no local Sunshine/Wolf host on this machine")
        raise PassFailed("unknown target %r" % (target,))

    # ── running one ─────────────────────────────────────────────────────────
    def launch(self, card, app):
        """Click a tile the way a finger does: a real event, at coordinates.

        element.click() is not enough. The app reads a scripted click on a tile
        as a hover, and the Fullscreen button that follows needs the user
        activation only a real event carries.
        """
        pos = self.json_eval("""(() => {
            const sel = '.host-card[data-uuid=%s] .app-card[data-app-id=%s]';
            const el = document.querySelector(sel);
            if (!el) return JSON.stringify(null);
            el.scrollIntoView({ block: 'center' });
            const r = el.getBoundingClientRect();
            return JSON.stringify({ x: Math.round(r.left + r.width / 2),
                                    y: Math.round(r.top + r.height / 2) });
        })()""" % (json.dumps(card["uuid"]), json.dumps(app["appId"])))
        if not pos:
            raise PassFailed("tile %r vanished before the click" % (app["name"],))
        for kind in ("mousePressed", "mouseReleased"):
            self.call("Input.dispatchMouseEvent", type=kind, x=pos["x"], y=pos["y"],
                        button="left", clickCount=1)
            time.sleep(0.05)
        # Streaming your own PC puts a confirmation in the way and leaves the
        # tile stuck on "Launching…" until it is answered. The dialog only
        # appears once the app has read the internet status, and right after
        # the instance starts that takes several seconds: one look at 1.5 s
        # missed it, and the first pass after every restart sat on
        # "Launching…" until "no picture after 60s" (23/09/2026).
        end = time.time() + 20
        while time.time() < end:
            time.sleep(0.5)
            got = self.eval("(() => { const g = document.querySelector('.self-stream-go');"
                            " if (g) { g.click(); return 'confirmed'; }"
                            " return document.querySelector('canvas') ? 'streaming' : 'waiting'; })()")
            if "waiting" not in got:
                return
            if "true" in self.eval(
                    "JSON.stringify(!!document.querySelector('.app-card--launch-failed'))"):
                return

    def wait_picture(self, timeout=45):
        """True once a canvas is on screen — the first frame has been drawn."""
        end = time.time() + timeout
        while time.time() < end:
            if "true" in self.eval("JSON.stringify(!!document.querySelector('canvas'))"):
                return True
            fail = self.eval(
                "JSON.stringify(!!document.querySelector('.app-card--launch-failed'))")
            if "true" in fail:
                raise PassFailed("the host refused the launch (tile went to launch-failed)")
            time.sleep(1)
        raise PassFailed("no picture after %ds" % timeout)

    def expand_latency_detail(self):
        """Unfold the per-leg breakdown, which is not rendered while collapsed."""
        try:
            self.eval("""(() => {
                const row = document.querySelector('.stats-latency-row');
                const t = (row && row.closest('.stats-content')) || row;
                if (!t) return 'no card';
                const o = { bubbles: true, cancelable: true, button: 0, pointerId: 1 };
                t.dispatchEvent(new PointerEvent('pointerdown', o));
                t.dispatchEvent(new PointerEvent('pointerup', o));
                return 'toggled';
            })()""")
            time.sleep(0.6)
        except Exception:
            pass

    def stats(self):
        """The overlay, as numbers. Total latency is the row labelled `Latency:`.

        The overlay is the only place that says what was NEGOTIATED — a HEVC
        that came back H.264, a 4:4:4 that came back 4:2:0 and an HDR that came
        back SDR are all silent everywhere else.
        """
        return self.json_eval("""(() => {
            const grab = sel => {
                const out = {};
                for (const r of document.querySelectorAll(sel)) {
                    const k = r.querySelector('.stats-label')?.textContent.trim();
                    const v = r.querySelector('.stats-value')?.textContent.trim();
                    if (k) out[k] = v;
                }
                return out;
            };
            const c = document.querySelector('canvas');
            return JSON.stringify({
                rows: grab('.stats-row'),
                legs: grab('.stats-leg-row'),
                latencyText: document.querySelector('.stats-value.stats-latency')?.textContent.trim() || null,
                canvas: c ? { w: c.width, h: c.height,
                              renderer: c.getContext('webgl2') ? 'webgl2' : 'canvas2d' } : null,
                fullscreen: !!document.fullscreenElement,
                visibility: document.visibilityState
            });
        })()""")

    def screenshot(self, path):
        directory = os.path.dirname(path)
        if directory:
            os.makedirs(directory, exist_ok=True)
        out = self.call("Page.captureScreenshot", format="png")
        data = None
        if isinstance(out, dict):
            data = out.get("result", {}).get("data") or out.get("data")
        if not data:
            return None
        with open(path, "wb") as f:
            f.write(base64.b64decode(data))
        return path

    def poke_input(self):
        """One key and one mouse move, so the host log can prove input arrives.

        A stream that shows a picture and takes no input still reads green
        everywhere else, which is exactly the failure worth catching.
        """
        self.call("Input.dispatchMouseEvent", type="mouseMoved", x=640, y=400)
        time.sleep(0.2)
        self.call("Input.dispatchMouseEvent", type="mouseMoved", x=900, y=520)
        for kind in ("keyDown", "keyUp"):
            self.call("Input.dispatchKeyEvent", type=kind, key="a", code="KeyA",
                        windowsVirtualKeyCode=65,
                        text="a" if kind == "keyDown" else "")
            time.sleep(0.05)

    def audio_state(self):
        """Whether an audio track is actually attached and running."""
        try:
            return self.json_eval("""(() => {
                const a = document.getElementById('stream-audio') || document.querySelector('audio');
                if (!a) return JSON.stringify({ element: false });
                const s = a.srcObject;
                const tracks = s && s.getAudioTracks ? s.getAudioTracks() : [];
                return JSON.stringify({
                    element: true, paused: a.paused, muted: a.muted,
                    tracks: tracks.length,
                    live: tracks.some(t => t.readyState === 'live' && t.enabled)
                });
            })()""")
        except PassFailed:
            return {"element": False}

    def stop(self):
        try:
            self.eval("(() => { const b = document.getElementById('btn-stream-quit');"
                      " if (b) { b.click(); return 'stopped'; } return 'no button'; })()")
        except Exception:
            pass
        time.sleep(3)
