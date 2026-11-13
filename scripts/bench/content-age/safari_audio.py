#!/usr/bin/env python3
"""A2 on Safari: NetEq's buffer with and without the page's audio floor, on
an iPhone (USB to the Mac) or the Mac's own Safari, driven by safaridriver.

    python safari_audio.py --platform iOS --runs 60,off,60,off --secs 75 --prefix a2-iphone
    python safari_audio.py --platform mac ...

On the Mac, once (by its owner): `safaridriver --enable`; for an iPhone, Web
Inspector and Remote Automation on in its Safari settings, the phone trusted
and plugged in. This script starts `safaridriver -p 4444` on the Mac over ssh,
tunnels it to this machine, and drives Safari through W3C WebDriver.

Each run: a --dev of this machine (its own rendezvous link: an iPhone refuses
the LAN address's certificate on the signalling socket), the PIN page filled
(a WebDriver session starts with no site data: a PIN every time),
`mw_audio_target` set (a number, or `off`; `60` = today's default, the key
removed), the Arc's screen streamed with the bench page on it, `--secs` of
`mwAudio.csv()` kept as `<prefix>-<target>-r<k>.audio.csv` next to the other
passes (audio_summary.py reads them). Safari has no CDP, so pass.py's probes
(clicks, frame ages) do not run here: this is the sound's buffer only.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time
import urllib.request

os.environ.setdefault("MW_BENCH_LOCAL_PORTS", "8080,8443")
HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
for p in (HERE, BENCH, os.path.join(BENCH, "acceptance")):
    sys.path.insert(0, p)
import local_matrix as LM  # noqa: E402
import run  # noqa: E402
import fleet  # noqa: E402
import gpu_load  # noqa: E402

OUT = LM.OUT
WD_PORT = 4444
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)


class WebDriver:
    def __init__(self, port, caps):
        self.base = "http://127.0.0.1:%d" % port
        got = self._req("POST", "/session", {"capabilities": {"alwaysMatch": caps}}, timeout=120)
        self.sid = got["sessionId"]
        print("WebDriver session", self.sid, got.get("capabilities", {}).get("browserVersion"),
              flush=True)

    def _req(self, method, path, body=None, timeout=60):
        data = json.dumps(body).encode() if body is not None else None
        req = urllib.request.Request(self.base + path, data=data, method=method,
                                     headers={"Content-Type": "application/json"})
        try:
            with urllib.request.urlopen(req, timeout=timeout) as r:
                return json.load(r).get("value")
        except urllib.error.HTTPError as e:
            raise RuntimeError("%s %s: %s" % (method, path, e.read()[:300]))

    def s(self, method, path, body=None, timeout=60):
        return self._req(method, "/session/%s%s" % (self.sid, path), body, timeout)

    def js(self, script, *args):
        return self.s("POST", "/execute/sync", {"script": script, "args": list(args)})

    def go(self, url):
        self.s("POST", "/url", {"url": url}, timeout=120)

    def quit(self):
        try:
            self._req("DELETE", "/session/" + self.sid)
        except Exception:
            pass


def ssh(cmd, timeout=60):
    return subprocess.run(["ssh", "mw-mac", cmd], capture_output=True, text=True,
                          timeout=timeout, creationflags=NOWIN).stdout


def rendezvous_link(log, timeout=60):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with open(log, encoding="utf-8", errors="replace") as f:
                m = re.findall(r"rendezvous line up\W+(https://\S+?)\"?\s*$", f.read(), re.M)
            if m:
                return m[-1].strip('"')
        except OSError:
            pass
        time.sleep(2)
    raise SystemExit("no rendezvous line in %s" % log)


def unlock_and_wait(wd, pin, tries=40):
    for _ in range(tries):
        state = wd.js("""
            if (document.getElementById('login-pin-input')) return 'pin';
            return document.querySelectorAll('.app-card').length > 0 ? 'library' : 'waiting';""")
        if state == "library":
            return
        if state == "pin":
            wd.js("""
                const set = (el, v) => {
                    const p = Object.getOwnPropertyDescriptor(HTMLInputElement.prototype, 'value').set;
                    p.call(el, v);
                    el.dispatchEvent(new Event('input', { bubbles: true }));
                    el.dispatchEvent(new Event('change', { bubbles: true }));
                };
                set(document.getElementById('login-machine-input'), arguments[0]);
                set(document.getElementById('login-pin-input'), arguments[1]);
                const k = document.getElementById('login-remember');
                if (k && !k.checked) k.click();
                // No <form> around the PIN page (06/10/2026, iOS 26.5): its
                // button, by id, then by its label in either language.
                const b = document.getElementById('btn-login-unlock') ||
                    [...document.querySelectorAll('button')].find(e =>
                        /^(unlock|déverrouiller)$/i.test(e.textContent.trim()));
                if (b) b.click();""", "bench-safari", pin)
            time.sleep(8)
            continue
        time.sleep(2)
    raise SystemExit("no host card on the page")


def tile(wd, index):
    inv = wd.js("""
        return [...document.querySelectorAll('.host-card')].map(card => ({
            uuid: card.dataset.uuid || '',
            apps: [...card.querySelectorAll('.app-card')].map(a => a.dataset.appId || '')
        }));""")
    for c in inv:
        if "1000" in c["apps"]:  # only the native host mints a Virtual Display
            phys = sorted((a for a in c["apps"] if a.isdigit() and a != "1000"), key=int)
            if len(phys) > index:
                return '.host-card[data-uuid="%s"] .app-card[data-app-id="%s"]' % (c["uuid"],
                                                                                 phys[index])
    raise SystemExit("no native display %d among %s" % (index, inv))


def one_run(wd, link, pin, index, target, secs, tag):
    wd.go(link)
    time.sleep(4)
    wd.js("""
        localStorage.setItem('mw-lang', 'en');
        if (arguments[0] === '60') localStorage.removeItem('mw_audio_target');
        else localStorage.setItem('mw_audio_target', arguments[0]);""", target)
    wd.go(link)
    time.sleep(4)
    unlock_and_wait(wd, pin)
    sel = tile(wd, index)
    # A WebDriver tap on the tile does nothing on iOS (06/10/2026: neither the
    # element click nor touch actions reached the list's click handler); the
    # element's own click() launches.
    wd.js("const el = document.querySelector(arguments[0]);"
          " el.scrollIntoView({ block: 'center' }); el.click();", sel)
    end = time.time() + 60
    while time.time() < end:
        if wd.js("return !!document.querySelector('canvas, video.stream-video, #stream-video')"):
            break
        time.sleep(1)
    else:
        raise SystemExit("no picture after 60 s")
    info = wd.js("""
        const a = document.getElementById('stream-audio') || document.querySelector('audio');
        return { ua: navigator.userAgent,
                 target: localStorage.getItem('mw_audio_target'),
                 jbt: 'jitterBufferTarget' in RTCRtpReceiver.prototype,
                 audio: a ? { paused: a.paused, muted: a.muted } : null };""")
    print("  streaming:", json.dumps(info), flush=True)
    time.sleep(secs)
    csv = wd.js("return window.mwAudio ? window.mwAudio.csv() : null")
    path = os.path.join(OUT, tag + ".audio.csv")
    if csv:
        with open(path, "w", newline="\n") as f:
            f.write(csv)
        print("  saved", os.path.basename(path), "(%d rows)" % (csv.count("\n") - 1), flush=True)
    else:
        print("  !! no mwAudio on the page", flush=True)
    with open(os.path.join(OUT, tag + ".safari.json"), "w") as f:
        json.dump(info, f)
    wd.js("const b = document.getElementById('btn-stream-quit'); if (b) b.click();")
    time.sleep(6)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--platform", default="iOS", choices=["iOS", "mac"])
    ap.add_argument("--runs", default="60,off,60,off")
    ap.add_argument("--secs", type=int, default=75)
    ap.add_argument("--prefix", default="a2-iphone")
    ap.add_argument("--gpu", default="Arc", help="the screen streamed: the GPU driving it")
    ap.add_argument("--tuning", default="audiolog=1")
    a = ap.parse_args()

    print("safaridriver on the Mac:", ssh("pkill -x safaridriver; (nohup safaridriver -p %d "
                                          ">/tmp/safaridriver.log 2>&1 &) ; sleep 2; "
                                          "pgrep -x safaridriver" % WD_PORT).strip(), flush=True)
    tun = subprocess.Popen(["ssh", "-N", "-o", "ExitOnForwardFailure=yes", "-L",
                            "%d:127.0.0.1:%d" % (WD_PORT, WD_PORT), "mw-mac"],
                           stdin=subprocess.DEVNULL, creationflags=NOWIN)
    time.sleep(3)
    LM.TUNING = a.tuning
    log = os.path.join(OUT, "%s.server.log" % a.prefix)
    wd = None
    try:
        LM.kill_dev()
        LM.launch_dev(0, "client", log)
        link = rendezvous_link(log)
        # The link and the PIN stay in memory: whether there is one, never what it is.
        probe = fleet.probe("local")
        pin = fleet.mint_pin("local", probe)
        index, disp = gpu_load.display_on_gpu(a.gpu, port=8080)
        print("dev up: rendezvous link", "yes" if link else "none", "| PIN", "yes" if pin else "none",
              "| screen", index, disp.get("gpu"), flush=True)
        run.content_start("scroll.html?band=time&px=600", probe=True)
        caps = {"browserName": "Safari", "platformName": a.platform}
        wd = WebDriver(WD_PORT, caps)
        runs = a.runs.split(",")
        # Safari 26.5 on iOS 18.7 has no jitterBufferTarget (06/10/2026): the
        # page's floor never applies there, and one pass says all there is.
        if not wd.js("return 'jitterBufferTarget' in RTCRtpReceiver.prototype"):
            print("no jitterBufferTarget in this Safari: one default pass only", flush=True)
            runs = ["60"]
        for k, target in enumerate(runs):
            tag = "%s-%s-r%d" % (a.prefix, target, k)
            print("==", tag, time.strftime("%H:%M:%S"), flush=True)
            # A PIN is single-use: a fresh one for each run that meets the PIN page.
            pin = fleet.mint_pin("local", fleet.probe("local")) or pin
            one_run(wd, link, pin, index, target, a.secs, tag)
    finally:
        if wd:
            wd.quit()
        try:
            run.content_stop()
        except Exception:
            pass
        LM.kill_dev()
        tun.terminate()
        print("safaridriver stopped:", ssh("pkill -x safaridriver; echo ok").strip(), flush=True)


if __name__ == "__main__":
    main()
