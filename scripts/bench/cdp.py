"""Drive the dedicated bench client Chrome over the DevTools protocol.

    python cdp.py [--port 9333] <command> [args]

    settings '{"video_codec":"hevc","stream_height":1080,...}'
                          patch localStorage['mw-streaming-settings'] and reload
    launch "Display 1"    click the tile whose text is that, by real coordinates
    fullscreen            click the app's Fullscreen button
    exitfs                Ctrl+Alt+Shift+X
    stop                  click "Stop streaming"
    stats                 one snapshot of the overlay's numbers, as JSON
    perf <seconds>        collect the once-a-second "[perf] ..." console lines
    eval "<js>"           evaluate and print
    evalfile <path>       evaluate JS read from a file
    call <Method> [json]  raw CDP call
    tiles                 list what looks clickable, to find a tile's exact text
    console <sec> [needle]  tail the page console

Why a dedicated Chrome and not the Chrome extension: the extension emulates a
fixed viewport whatever the screen, its synthetic clicks carry no user
activation so requestFullscreen is refused, and its tab stays
visibilityState=hidden — which freezes rAF and hangs the latency probe. Every
presentation measurement has to come through here.

Clicks are real Input.dispatchMouseEvent events at coordinates, never
element.click(): the app treats a scripted click on a host tile as a hover, and
a launch needs the activation a real event carries.

Needs `pip install websocket-client`.
"""
import argparse
import json
import sys
import time
import urllib.request

import websocket  # websocket-client


def page_ws(port):
    tabs = json.load(urllib.request.urlopen(f"http://localhost:{port}/json"))
    pages = [t for t in tabs if t["type"] == "page" and "moonlightweb" in t["url"]]
    if not pages:
        pages = [t for t in tabs if t["type"] == "page"]
    if not pages:
        raise SystemExit(f"no page on the debugging port {port} — is the kiosk Chrome running?")
    return websocket.create_connection(pages[0]["webSocketDebuggerUrl"], suppress_origin=True)


class Cdp:
    def __init__(self, port):
        self.port = port
        self.ws = page_ws(port)
        self.n = 0

    def call(self, method, **params):
        self.n += 1
        self.ws.send(json.dumps({"id": self.n, "method": method, "params": params}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg.get("id") == self.n:
                if "error" in msg:
                    raise RuntimeError(msg["error"])
                return msg.get("result", {})

    def eval(self, js):
        r = self.call("Runtime.evaluate", expression=js, awaitPromise=True, returnByValue=True)
        return r.get("result", {}).get("value")

    def click(self, x, y):
        for t in ("mouseMoved", "mousePressed", "mouseReleased"):
            self.call("Input.dispatchMouseEvent", type=t, x=x, y=y, button="left", clickCount=1)
            time.sleep(0.05)

    def click_text(self, text, tag_hint=""):
        js = f"""(() => {{
            const els = [...document.querySelectorAll('{tag_hint or "*"}')].filter(e =>
                e.children.length === 0 && e.textContent.trim() === {json.dumps(text)});
            if (!els.length) return null;
            els[0].scrollIntoView({{block: 'center'}});
            const r = els[0].getBoundingClientRect();
            return [r.left + r.width / 2, r.top + r.height / 2];
        }})()"""
        pos = self.eval(js)
        if not pos:
            raise SystemExit(f"no element with text {text!r} — try `tiles` to see what is there")
        self.click(*pos)
        return pos

    def key_combo(self, key, code, mods):
        # mods bitmask: Alt=1, Ctrl=2, Meta=4, Shift=8
        for phase in ("keyDown", "keyUp"):
            self.call("Input.dispatchKeyEvent", type=phase, key=key, code=code, modifiers=mods,
                      windowsVirtualKeyCode=ord(key.upper()))


# The overlay is the only place the NEGOTIATED truth is visible: which codec was
# actually agreed, which enhancer is actually running, what the stream really
# decodes and presents. The requested settings say what we asked for; these say
# what we got, and a campaign that reports only the first is reporting fiction.
#
# The rows carry no ids — they are label/value pairs whose labels come from the
# translations — so they are scraped as a dictionary rather than by id, and
# `settings` pins the bench profile to English (mw-lang) so those keys are
# stable from one machine to the next. The locale-independent facts (canvas
# size, renderer, fullscreen, visibility) are read directly and are what the
# report keys off when a label is missing.
STATS_JS = """(() => {
  const rows = {};
  for (const r of document.querySelectorAll('.stats-row')) {
    const k = r.querySelector('.stats-label')?.textContent.trim();
    const v = r.querySelector('.stats-value')?.textContent.trim();
    if (k) rows[k] = v;
  }
  const c = document.querySelector('canvas');
  return JSON.stringify({
    rows,
    canvas: c ? { w: c.width, h: c.height,
                  renderer: c.getContext('webgl2') ? 'webgl2' : 'canvas2d',
                  cssW: Math.round(c.getBoundingClientRect().width),
                  cssH: Math.round(c.getBoundingClientRect().height) } : null,
    fullscreen: !!document.fullscreenElement,
    visibility: document.visibilityState,
    streaming: !!document.querySelector('canvas')
  });
})()"""


def main():
    parser = argparse.ArgumentParser(add_help=False)
    parser.add_argument("--port", type=int, default=9333)
    parser.add_argument("command")
    parser.add_argument("args", nargs="*")
    ns = parser.parse_args()

    c = Cdp(ns.port)
    cmd, args = ns.command, ns.args

    if cmd == "settings":
        # The client reads its settings from localStorage at launch, so the
        # patch has to land before the reload, not after. mw-lang is pinned so
        # the overlay's labels — which `stats` scrapes — read the same on every
        # machine of the fleet, and mw_perf_diag so the once-a-second [perf]
        # line exists to be collected.
        print(c.eval(f"""(() => {{
            const s = JSON.parse(localStorage.getItem('mw-streaming-settings') || '{{}}');
            Object.assign(s, {args[0]});
            localStorage.setItem('mw-streaming-settings', JSON.stringify(s));
            localStorage.setItem('mw-lang', 'en');
            localStorage.setItem('mw_perf_diag', '1');
            return JSON.stringify(s);
        }})()"""))
        c.call("Page.reload")
    elif cmd == "launch":
        print("clicked tile at", c.click_text(args[0]))
    elif cmd == "fullscreen":
        pos = c.click_text("Fullscreen")
        time.sleep(1.5)
        print("fullscreen at", pos, "->", c.eval("!!document.fullscreenElement"))
    elif cmd == "exitfs":
        c.key_combo("x", "KeyX", 1 | 2 | 8)
        time.sleep(1)
        print("fullscreen:", c.eval("!!document.fullscreenElement"))
    elif cmd == "stop":
        print("stop at", c.click_text("Stop streaming"))
    elif cmd == "stats":
        print(c.eval(STATS_JS))
    elif cmd == "perf":
        # mw_perf_diag=1 makes StreamView log one "[perf] ..." line a second
        # with the queue depths, the drop causes and the p99 of every leg —
        # host and client. Reading them here means the numbers are collected
        # without anyone looking at the screen.
        seconds = float(args[0]) if args else 10.0
        c.eval("localStorage.setItem('mw_perf_diag','1')")
        c.call("Runtime.enable")
        c.ws.settimeout(0.5)
        end = time.time() + seconds
        while time.time() < end:
            try:
                msg = json.loads(c.ws.recv())
            except Exception:
                continue
            if msg.get("method") != "Runtime.consoleAPICalled":
                continue
            text = " ".join(str(a.get("value", a.get("description", "")))
                            for a in msg["params"].get("args", []))
            if "[perf]" in text:
                print(text)
    elif cmd == "eval":
        print(c.eval(args[0]))
    elif cmd == "evalfile":
        # JS read from a file: PowerShell 5.1 eats the double quotes of an
        # inline argument before python ever sees them.
        with open(args[0], encoding="utf-8") as f:
            print(c.eval(f.read()))
    elif cmd == "nav":
        print(c.call("Page.navigate", url=args[0]))
    elif cmd == "call":
        params = json.loads(args[1]) if len(args) > 1 else {}
        print(c.call(args[0], **params))
    elif cmd == "tiles":
        print(c.eval("""(() => [...document.querySelectorAll('h1,h2,h3,h4,button,[role=button],.card,[class*=tile],[class*=card]')]
            .filter(e => e.children.length <= 3)
            .map(e => e.tagName + '.' + e.className + ' | ' + e.textContent.trim().slice(0, 50))
            .slice(0, 60).join('\\n'))()"""))
    elif cmd == "console":
        seconds = float(args[0]) if args else 8.0
        needle = args[1] if len(args) > 1 else ""
        c.call("Runtime.enable")
        c.ws.settimeout(0.5)
        end = time.time() + seconds
        while time.time() < end:
            try:
                msg = json.loads(c.ws.recv())
            except Exception:
                continue
            if msg.get("method") != "Runtime.consoleAPICalled":
                continue
            text = " ".join(str(a.get("value", a.get("description", "")))
                            for a in msg["params"].get("args", []))
            if needle in text:
                print(f"[{msg['params'].get('type')}] {text}")
    else:
        raise SystemExit(f"unknown command {cmd!r} — see the docstring at the top of this file")


if __name__ == "__main__":
    sys.exit(main())
