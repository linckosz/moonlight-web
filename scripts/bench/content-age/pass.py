"""One content-age pass on this machine: a self-stream, the bench page's time
band on the captured screen, and the client's probe reading it back.

    python pass.py --tag d1-auto --secs 30
    python pass.py --tag d1-120 --fps 120 --vsync on --every 2
    python pass.py --tag d1-detect --autostep --settle 14    # "Auto" with detection

Driven like ../cadence/cadence.py — the acceptance run's environment
(MW_BENCH_LOCAL_PORTS for a --dev instance; MW_BENCH_CLIENT_POS /
MW_BENCH_CLIENT_LUID to put the client on another screen, driven by another GPU
than the encoder's). The host's own keys (MW_NATIVE_TUNING=cadence=…,
MW_VDD_REFRESH) are the instance's environment, set when it was launched.

A client on the same machine shares the host's clock and its compositor: this
checks the instrument and the plumbing. The measurements that count come from
a client on another machine: --client-port names the debugging port of a
Chrome already running there (through an SSH tunnel), --client-url the address
it reaches this host at (plan framerate-hote §4).
"""
import argparse
import json
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(BENCH, "cadence"))
import run, drive, fleet  # noqa: E402
import age  # noqa: E402
sys.path.insert(0, os.path.join(BENCH, "wifi"))
import remote_host  # noqa: E402
from cadence import monitors  # noqa: E402

CONTENT_PORT = 9334


def _ms(v):
    return "-" if v is None else "%.2f" % v


def click_flag(d, n, every_ms=800, tag=None):
    """@p n click → flag samples (frontend LatencyProbe.js, the host's
    LatencyFlag): the whole loop the player feels, input to picture. Started
    without waiting on it — a minute is longer than a DevTools call should
    hang — and read back as it fills. None when the stream has no flag.

    With @p tag, the client's per-frame log of the clicks' minute is saved
    (<tag>.clicks.frames.csv) with the page's time origin: a click's `ts` and
    `latencyMs` then name the frame that showed its flag, which the relay's
    frame log follows back on the host (plan Wi-Fi W1, scripts/bench/wifi)."""
    if d.eval("typeof (window.mwLatency && window.mwLatency.run)") != "function":
        print("  clicks: no click-to-photon probe on this stream (latency_flag_enabled?)",
              flush=True)
        return None
    if tag:
        d.eval("window.mwFrameLog && mwFrameLog.clear()")
    time_origin = d.eval("performance.timeOrigin")
    before = d.eval("(window.mwLatencyResults || []).length") or 0
    d.eval("window.mwLatency.run(%d, %d); 1" % (n, every_ms))
    end = time.time() + n * (every_ms + 300) / 1000 + 30
    while time.time() < end:
        if (d.eval("(window.mwLatencyResults || []).length") or 0) - before >= n:
            break
        time.sleep(2)
    samples = d.json_eval("JSON.stringify((window.mwLatencyResults || []).slice(%d))" % before)
    ok = sorted(s["latencyMs"] for s in samples
                if s.get("ok") and s.get("latencyMs") is not None)
    pick = lambda q: ok[min(len(ok) - 1, int(q * len(ok)))] if ok else None  # noqa: E731
    summary = {"n": len(samples), "ok": len(ok), "medianMs": pick(0.5), "p90Ms": pick(0.9),
               "minMs": ok[0] if ok else None, "maxMs": ok[-1] if ok else None}
    # ASCII only: under local_matrix.py this goes through a cp1252 pipe.
    print("  clicks: %d of %d measured, click -> flag median %s ms (p90 %s, %s to %s)" % (
        summary["ok"], summary["n"], _ms(summary["medianMs"]), _ms(summary["p90Ms"]),
        _ms(summary["minMs"]), _ms(summary["maxMs"])), flush=True)
    # Each click split by the host's answer to its stamp (InputUplink.js, plan
    # radios T7): the way up, the injection, and the rest (flag, capture,
    # encode, the way down, decode, draw).
    for key in ("upMs", "hostInMs", "restMs"):
        vals = sorted(s[key] for s in samples
                      if s.get("ok") and isinstance(s.get(key), (int, float)))
        summary[key] = {"n": len(vals),
                        "median": vals[len(vals) // 2] if vals else None,
                        "p90": vals[min(len(vals) - 1, int(0.9 * len(vals)))] if vals else None,
                        "max": vals[-1] if vals else None}
    if summary["upMs"]["n"]:
        print("  split: up median %s ms (p90 %s, max %s), host %s ms, rest median %s ms (p90 %s)" % (
            _ms(summary["upMs"]["median"]), _ms(summary["upMs"]["p90"]), _ms(summary["upMs"]["max"]),
            _ms(summary["hostInMs"]["median"]), _ms(summary["restMs"]["median"]),
            _ms(summary["restMs"]["p90"])), flush=True)
    if tag and d.eval("typeof window.mwFrameLog === 'object' && !!window.mwFrameLog"):
        path = os.path.join(age.OUT, tag + ".clicks.frames.csv")
        with open(path, "w", newline="") as f:
            f.write(d.eval("mwFrameLog.csv()") or "")
        print("  saved", path, flush=True)
    return {"summary": summary, "samples": samples, "timeOrigin": time_origin}


def uplink_runs(d, spec, tag):
    """The way up alone (frontend InputUplink.js, plan radios T7): for each
    HZ:SECS of @p spec, that many dated messages a second that do nothing on the
    host. None when the stream has no such bench."""
    if not spec:
        return None
    if d.eval("typeof (window.mwUplink && window.mwUplink.run)") != "function":
        print("  uplink: this client has no uplink bench (older page?)", flush=True)
        return None
    runs = []
    for part in spec.split(","):
        hz, secs = (int(x) for x in part.split(":"))
        before = d.eval("(window.mwUplinkResults || []).length") or 0
        d.eval("window.mwUplink.run({hz: %d, secs: %d, label: %s}); 1" % (hz, secs, json.dumps(tag)))
        end = time.time() + secs + 15
        while time.time() < end:
            if (d.eval("(window.mwUplinkResults || []).length") or 0) > before:
                break
            time.sleep(1)
        got = d.json_eval("JSON.stringify((window.mwUplinkResults || []).slice(%d))" % before)
        if not got:
            print("  uplink %d/s: no result" % hz, flush=True)
            continue
        r = got[0]
        up, rtt = r["up"], r["rtt"]
        print("  uplink %d/s for %d s: %d/%d answered, up median %s ms (p90 %s, p99 %s, max %s), "
              "rtt median %s, %d sends queued" % (
                  hz, secs, r["answered"], r["sent"], _ms(up["median"]), _ms(up["p90"]),
                  _ms(up["p99"]), _ms(up["max"]), _ms(rtt["median"]), r["queuedSends"]), flush=True)
        runs.append(r)
    return runs


def stepper_state(d, content_ms):
    """"Auto" with detection (frontend CadenceStepper.js, design §33.10): its
    state and its decisions, each timed from the moment the content began to
    move (@p content_ms, the client's clock) — the gate wants the final step
    within ten seconds. None when it did not run on this pass."""
    st = d.json_eval("JSON.stringify(window.mwCadenceStepper ? {summary: "
                     "window.mwCadenceStepper.summary, events: window.mwCadenceStepper.events}"
                     " : null)")
    if not st:
        return None
    for e in st.get("events") or []:
        e["sinceContentS"] = (round((e["at"] - content_ms) / 1000, 2)
                              if isinstance(content_ms, (int, float)) else None)
    after = [e for e in st["events"] if (e.get("sinceContentS") or 0) >= 0]
    decided = [e for e in after if e["what"] in ("kept", "rejected", "refused", "fallback")]
    kept = [e for e in after if e["what"] == "kept"]
    s = st["summary"]
    st["firstDecisionS"] = decided[0]["sinceContentS"] if decided else None
    st["keptAtS"] = kept[-1]["sinceContentS"] if kept and s.get("stepFps") else None
    print("  steps: %s fps on a ladder %s, %d trials (%d kept, %d given up, %d refused, "
          "%d trips); first decision %s s, last kept %s s after the content moved" % (
              s.get("stepFps") or s.get("base"), s.get("levels"), s.get("trials", 0),
              s.get("kept", 0), s.get("rejected", 0), s.get("refused", 0), s.get("trips", 0),
              _ms(st["firstDecisionS"]), _ms(st["keptAtS"])), flush=True)
    for e in decided:
        facts = ", ".join("%s=%s" % (k, round(v, 2) if isinstance(v, float) else v)
                          for k, v in e.items() if k not in ("at", "what", "sinceContentS"))
        # ASCII only: a reason may carry "→", and under local_matrix.py this
        # goes through a cp1252 pipe.
        line = "    %6.2f s  %-9s %s" % (e["sinceContentS"] or 0, e["what"], facts)
        print(line.replace("→", "->").encode("ascii", "replace").decode(), flush=True)
    return st


def start_load(gpu, name, level=None):
    """mw-gpu-load on @p gpu, calibrated (or at @p level), its window kept off
    the captured content: the tool moves it onto the screen that GPU drives,
    which can be the virtual display under test. Returns (load, level)."""
    import gpu_load
    load = gpu_load.Load(gpu, os.path.join(age.OUT, name), level=level)
    line = load.wait_calibrated()
    moved = keep_off_content(load.proc.pid)
    if moved:
        print("  load window moved off the content:", moved, flush=True)
    return load, level or line.get("level")


def keep_off_content(pid):
    """Move @p pid's windows that overlap MW_BENCH_CONTENT_RECT onto a screen
    that does not, still topmost (a hidden load window stops rendering). Not
    "the primary": while it streams, the product's virtual display can be the
    primary screen itself (U0.3, 04/10/2026: the load sat on the content)."""
    import ctypes
    from ctypes import wintypes
    rect = os.environ.get("MW_BENCH_CONTENT_RECT")
    if not rect:
        return []
    x, y, w, h = (int(v) for v in rect.split(","))
    def apart(m):
        mx, my = (int(v) for v in m[1].split(","))
        mw, mh = (int(v) for v in m[2].split("x"))
        return mx >= x + w or mx + mw <= x or my >= y + h or my + mh <= y

    other = next((m for m in monitors() if len(m) > 2 and apart(m)), None)
    if not other:
        print("  no screen apart from the content for the load window", flush=True)
        return []
    px, py = (int(v) for v in other[1].split(","))
    user32 = ctypes.windll.user32
    found = []

    @ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def each(hwnd, _):
        owner = wintypes.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(owner))
        if owner.value == pid and user32.IsWindowVisible(hwnd):
            found.append(hwnd)
        return True

    user32.EnumWindows(each, 0)
    moved = []
    for hwnd in found:
        r = wintypes.RECT()
        user32.GetWindowRect(hwnd, ctypes.byref(r))
        if r.left < x + w and r.right > x and r.top < y + h and r.bottom > y:
            # HWND_TOPMOST, SWP_NOSIZE | SWP_NOACTIVATE
            user32.SetWindowPos(hwnd, wintypes.HWND(-1), px + 80, py + 80, 0, 0, 0x0001 | 0x0010)
            moved.append((r.left, r.top))
    return moved


def frames_only(d, tag, secs):
    """A pass against a host on another machine (--host): its band is not read,
    calibrating it wants the host's own clock beside the page. The per-frame
    log stands in: each frame's capture → draw on the host's clock, written as
    the band's pass writes it (<tag>.json, <tag>.frames.csv)."""
    d.eval("window.mwFrameLog && mwFrameLog.clear()")
    time.sleep(secs)
    data = {"tag": tag, "remoteHost": True}
    frames = d.eval("window.mwFrameLog ? JSON.stringify(mwFrameLog.summary()) : null")
    os.makedirs(age.OUT, exist_ok=True)
    if frames and frames != "null":
        data["frameLog"] = json.loads(frames)
        with open(os.path.join(age.OUT, tag + ".frames.csv"), "w", newline="") as f:
            f.write(d.eval("mwFrameLog.csv()") or "")
        print("  frames: e2e median %s ms (p99 %s)" % (
            _ms(data["frameLog"].get("medianMs")), _ms(data["frameLog"].get("p99Ms"))),
            flush=True)
    with open(os.path.join(age.OUT, tag + ".json"), "w") as f:
        json.dump(data, f)


def band_seen(d, secs=2.0):
    """Whether the client reads the bench page's band on the stream: two
    seconds of the probe, every frame. False only when it read no age and
    found no band at all (every read "block"), a screen without the page."""
    d.eval("mwContentAge.start({every: 1})")
    time.sleep(secs)
    raw = d.eval("JSON.stringify(mwContentAge.stop())")
    s = json.loads(raw) if raw and raw != "null" else {}
    return (s.get("ages") or 0) > 0 or not (s.get("invalid") or {}).get("block")



def library(d, access, tries=6):
    """The host cards on the page, which is loaded again when they do not come.
    One module lost on the way stops the whole app before it asks for anything:
    a host on Wi-Fi let some of the connections of a page load time out (mw-mac,
    09/10/2026: ERR_CONNECTION_TIMED_OUT, on app.js itself in two loads of
    three; each file is a connection of its own, Connection: close, and a page
    reached at the host's address has no service worker to load from)."""
    for attempt in range(tries):
        if d.wait_library(access.get("name", "bench"), access.get("pin", ""),
                          tries=25 if attempt == 0 else 8):
            return True
        print("  no host card yet: the page loaded again", flush=True)
        d.navigate(access["lan"])
    return False


def ultra_player(d, tag):
    """POC Ultra U4: the page's PyroWave player, where a frame's decode time
    goes (packets parsed, work recorded, GPU done, VideoFrame made); None
    without one. With localStorage mw_ultra_trace=1 (plan « attente » B0),
    every frame's timeline too, beside the pass as <tag>.ultratrace.json, for
    scripts/bench/clickpath/gpuwait.py."""
    raw = d.eval("globalThis.__mwUltraPlayer ? JSON.stringify(__mwUltraPlayer.summary()) : null")
    if not raw:
        return None
    player = json.loads(raw)
    print("  pyrowave p50 ms: " + ", ".join(
        "%s %s" % (k, (player.get(k) or {}).get("p50"))
        for k in ("wait", "parse", "record", "done", "frame", "gpuDecode", "gpuPresent", "spin")), flush=True)
    if player.get("traced"):
        trace = d.eval("JSON.stringify(__mwUltraPlayer.trace)")
        with open(os.path.join(age.OUT, tag + ".ultratrace.json"), "w") as f:
            f.write(trace)
        print("  pyrowave trace: %s records" % player["traced"], flush=True)
        # The audio road's frames through the browser (POC Ultra P-B):
        # [rtp ts, first chunk read, its receive, last chunk's receive,
        # posted by the worker, on the page, bytes], beside it as
        # <tag>.rtptrace.json.
        rtp = d.eval("globalThis.__mwRtp && __mwRtp.trace ? JSON.stringify(__mwRtp.trace) : null")
        if rtp:
            with open(os.path.join(age.OUT, tag + ".rtptrace.json"), "w") as f:
                f.write(rtp)
            print("  audio road trace: %d frames" % len(json.loads(rtp)), flush=True)
    return player


def ticks_origin(d):
    """The page's time origin on the browser's TimeTicks clock, in µs, or None
    (MW_BENCH_TICKS_ORIGIN=1, POC Ultra U3.7, the end of the chain). Read from
    marks the page sets during a short trace: a mark's trace event carries its
    moment on TimeTicks, unclamped, and the page its startTime. On Windows
    TimeTicks is QueryPerformanceCounter in µs, so a trace taken outside the
    browser on that clock (PresentMon --qpc_time on the client) joins the
    page's frame log: QPC µs = page ms × 1000 + this."""
    import base64
    import websocket
    c = d.c
    try:
        c.call("Tracing.start", transferMode="ReturnAsStream",
               traceConfig={"includedCategories": ["blink.user_timing"],
                            "recordMode": "recordUntilFull"})
        marks = d.json_eval("JSON.stringify([0, 1, 2, 3, 4].map(i => { const m = "
                            "performance.mark('mw-ticks-' + i); return [m.name, m.startTime]; }))")
        c.n += 1
        c.ws.send(json.dumps({"id": c.n, "method": "Tracing.end"}))
        stream, deadline = None, time.time() + 20
        while stream is None and time.time() < deadline:
            c.ws.settimeout(max(0.1, deadline - time.time()))
            try:
                msg = json.loads(c.ws.recv())
            except websocket.WebSocketTimeoutException:
                break
            if msg.get("method") == "Tracing.tracingComplete":
                stream = (msg.get("params") or {}).get("stream")
        if not stream:
            print("  ticks origin: the trace did not come back", flush=True)
            return None
        data = b""
        while True:
            r = c.call("IO.read", handle=stream, size=1 << 20)
            chunk = r.get("data", "")
            data += base64.b64decode(chunk) if r.get("base64Encoded") else chunk.encode()
            if r.get("eof"):
                break
        c.call("IO.close", handle=stream)
        trace = json.loads(data.decode("utf-8", "replace"))
        events = trace.get("traceEvents", []) if isinstance(trace, dict) else trace
        at = {e["name"]: e["ts"] for e in events
              if str(e.get("name", "")).startswith("mw-ticks-") and "ts" in e}
        offsets = sorted(at[n] - s * 1000 for n, s in marks if n in at)
    except (Exception, SystemExit) as e:
        print("  ticks origin: %s" % e, flush=True)
        return None
    if not offsets:
        print("  ticks origin: no mark in the trace", flush=True)
        return None
    us = offsets[len(offsets) // 2]
    print("  ticks origin: %.1f us (%d marks, spread %.1f us)" % (
        us, len(offsets), offsets[-1] - offsets[0]), flush=True)
    return {"us": us, "marks": len(offsets), "spreadUs": offsets[-1] - offsets[0]}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fps", type=int, default=0, help="stream_fps; 0 = Auto")
    ap.add_argument("--codec", choices=["h264", "hevc", "av1"], default="",
                    help="video_codec; the bench's own (HEVC) otherwise")
    ap.add_argument("--vsync", choices=["on", "off"], default="off",
                    help="on = tearing off: the client paints on its refresh")
    ap.add_argument("--secs", type=float, default=30)
    ap.add_argument("--every", type=int, default=1)
    ap.add_argument("--settle", type=float, default=6)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--target", default="display", help="display | vdisplay")
    ap.add_argument("--display-index", default="0")
    ap.add_argument("--px", type=int, default=600, help="the page's scroll speed")
    ap.add_argument("--game-fps", default="",
                    help="the page's rate, like a game's: 50, or 49-53 drawn at random")
    ap.add_argument("--client-port", type=int, default=0,
                    help="a client Chrome on another machine, its debugging port tunnelled here")
    ap.add_argument("--client-url", default="", help="the address that client reaches this host at")
    ap.add_argument("--bitrate", type=int, default=0,
                    help="kbps; 0 = the automatic one, sized for the client's rate")
    ap.add_argument("--hold", type=int, default=0,
                    help="seconds of stream held on the virtual display with no bench page "
                         "over it, for a game driven apart; no content-age reading")
    ap.add_argument("--uplink", default="",
                    help="HZ:SECS[,HZ:SECS] dated input messages, the way up alone (plan radios T7)")
    ap.add_argument("--clicks", type=int, default=0,
                    help="click → flag samples after the content-age window (needs "
                         "latency_flag_enabled in the instance's settings.json)")
    ap.add_argument("--local-storage", action="append", default=[], metavar="KEY=VALUE",
                    help="a bench switch the page reads at launch (mw_decodequeue=pending)")
    ap.add_argument("--setting", action="append", default=[], metavar="KEY=VALUE",
                    help="a streaming setting over the bench's, the value read as JSON when it "
                         "parses (video_enhancement=\"on\", video_enhancement_algo=\"video\")")
    ap.add_argument("--fullscreen", action="store_true",
                    help="the client's browser window full screen once the picture is up "
                         "(CDP Browser.setWindowBounds), instead of maximized")
    ap.add_argument("--gpu-load", default="",
                    help="mw-gpu-load on this GPU (a part of its name: Arc, RTX, AMD) under the "
                         "measurement, then again under the clicks: the tool stops at 60 s, a run "
                         "each (acceptance/gpu_load.py)")
    ap.add_argument("--autostep", action="store_true",
                    help="\"Auto\" with detection on (localStorage mw_autostep=1; design "
                         "§33.10): the stream may step above the client's rate. Off otherwise "
                         "(mw_autostep=0): on is the product's default, and a pass without this "
                         "flag stays the Auto from before it, the bench's reference")
    ap.add_argument("--host", default="local",
                    help="the host: this machine (local), or a fleet machine's DEV edition "
                         "(um790pro, mw-mac; ../wifi/remote_host.py): its page put up over "
                         "SSH, its band not read")
    a = ap.parse_args()
    remote = a.client_port > 0
    rh = remote_host.for_machine(a.host) if a.host != "local" else None
    # Another session's Chrome may already hold the client kiosk's debugging
    # port (9333 on 01/10/2026): the kiosk would not get it, and this pass would
    # drive that other browser. MW_BENCH_DEBUG_PORT moves the kiosk's.
    if os.environ.get("MW_BENCH_DEBUG_PORT"):
        run.DEBUG_PORT = int(os.environ["MW_BENCH_DEBUG_PORT"])

    access = dict(run.access_map().get(a.host) or {})
    probe = fleet.probe(a.host)
    pin = (probe.get("pin") or {}).get("pin")
    if pin:
        access["pin"] = pin
    access["lan"] = fleet.lan_url(a.host, probe) or access.get("lan")
    # MW_BENCH_VIA=rendezvous: the page reached through the host's rendezvous
    # address, as from the Internet, its interface from the bootstrap's cache
    # and its requests through the tunnel; the stream is the same WebRTC. For
    # mw-mac on 09/10/2026, whose page at its own address hung on a connection
    # that never opened (see library()).
    rdv = ((probe.get("internet") or {}).get("rendezvous") or {}).get("url")
    if os.environ.get("MW_BENCH_VIA") == "rendezvous":
        if not rdv:
            raise SystemExit("MW_BENCH_VIA=rendezvous but %s has no rendezvous address" % a.host)
        access["lan"] = rdv
    # A remote client reaches this machine's --dev at the address it is told,
    # a remote host at its own. With both, the remote client is the one driven:
    # until 04/10/2026 the kiosk on this machine stood in for it (every --host
    # pass before that date had DualRTX on Ethernet as its client).
    if remote:
        if not rh:
            access["lan"] = a.client_url or access["lan"]
        d = drive.Driver(a.client_port)
    else:
        run.kiosk_start(access["lan"])
        d = drive.Driver(run.DEBUG_PORT)
    shown = False
    try:
        d.navigate(access["lan"])
        # The --dev instance serves frontend/ from the sources, but the service
        # worker keeps the last version it cached: an edited probe would not
        # run (memory frontend-sw-cache-stale-tests).
        d.eval("(async () => { await caches.delete('mw-shell'); for (const r of await "
               "navigator.serviceWorker.getRegistrations()) await r.unregister(); return 1; })()")
        d.navigate(access["lan"])
        library(d, access)
        # The bench profile keeps its localStorage from one pass to the next:
        # a switch not asked for this time is taken away.
        d.eval("localStorage.removeItem('mw_decodequeue')")
        # The detection is on by default (UA.4): off is said, for the
        # reference modes (client, host-guarded) to stay today's Auto.
        d.eval("localStorage.setItem('mw_autostep', %s)" % json.dumps("1" if a.autostep else "0"))
        # Each pass is this device's first stream: the steps it failed at in
        # the passes before are forgotten (mw_autostep_failed;
        # --local-storage puts them back for a pass that wants them).
        d.eval("localStorage.removeItem('mw_autostep_failed')")
        for kv in a.local_storage:
            k, _, v = kv.partition("=")
            d.eval("localStorage.setItem(%s, %s)" % (json.dumps(k), json.dumps(v)))
        settings = dict(run.load_matrix()["base"])
        settings.update({"stream_fps": a.fps, "tearing_default_v2": True,
                         "tearing_enabled": a.vsync == "off"})
        if a.codec:
            settings["video_codec"] = a.codec
        for kv in a.setting:
            k, _, v = kv.partition("=")
            try:
                settings[k] = json.loads(v)
            except ValueError:
                settings[k] = v
        if a.bitrate > 0:
            settings.update({"stream_bitrate_auto": False, "stream_bitrate": a.bitrate})
        d.apply_settings(settings)
        library(d, access)
        os.environ["MW_BENCH_DISPLAY"] = a.display_index
        card, app = d.pick_tile(a.target)
        print("tile", card.get("name"), "/", app.get("name"), flush=True)
        before = {m[0] for m in monitors()}
        d.launch(card, app)
        d.wait_picture(timeout=60)
        if a.fullscreen:
            # The window, not the page's element: a page may only go full screen
            # on a user gesture, the browser's window on CDP's word.
            win = d.call("Browser.getWindowForTarget")
            d.call("Browser.setWindowBounds", windowId=win["windowId"],
                   bounds={"windowState": "fullscreen"})
            time.sleep(2)
            print("  client window full screen", flush=True)
        # The Virtual Display only exists once the stream is up: it is the
        # screen that was not there before the launch. A remote host's is
        # on that machine, where its own tools find it.
        if a.target == "vdisplay" and not rh:
            time.sleep(2)
            vdd = [m for m in monitors() if m[0] not in before]
            if not vdd:
                raise SystemExit("no virtual display appeared: %s" % monitors())
            x, y = vdd[0][1].split(",")
            w, h = vdd[0][2].split("x")
            os.environ["MW_BENCH_CONTENT_RECT"] = "%s,%s,%s,%s" % (x, y, w, h)
            print("virtual display", " ".join(vdd[0]), flush=True)
        target = None
        if a.hold > 0 and os.environ.get("MW_BENCH_CLICK_TARGET") and rh:
            # The same on a remote host (plan « attente », AM0): the tool's
            # path is the host's, its log fetched at the end. A physical
            # display too (AL0, KMS): the host's tool finds its screen itself
            # (MW_BENCH_CLICK_TARGET_DISPLAY on Linux).
            print("  " + rh.click_target_start(
                a.tag, int(a.hold) + 300, os.environ["MW_BENCH_CLICK_TARGET"],
                os.environ.get("MW_BENCH_CLICK_TARGET_ARGS", "").split()), flush=True)
            target = rh
        elif a.hold > 0 and a.target == "vdisplay" and os.environ.get("MW_BENCH_CLICK_TARGET"):
            # Plan « attente » A1: mw-click-target over the virtual display, the
            # click's ideal game drawing the flag itself (the host's kept off
            # every screen: MW_LATENCY_FLAG_SKIP=*); its log beside the pass.
            target = subprocess.Popen(
                [os.environ["MW_BENCH_CLICK_TARGET"], "--display", vdd[0][0], "--out",
                 os.path.join(age.OUT, a.tag + ".target.jsonl"), "--duration",
                 str(int(a.hold) + 300)] + os.environ.get("MW_BENCH_CLICK_TARGET_ARGS", "").split())
            time.sleep(3)
            print("  click target on %s: %s" % (vdd[0][0], "running" if target.poll() is None
                                                  else "exited %s" % target.returncode), flush=True)
        if a.hold > 0:
            # A game on the virtual display instead of the bench page (RE9,
            # driven by a script of its own): the stream held, nothing drawn
            # over it, the overlay read at the end. The frame log of the held
            # time, read before the clicks clear it: each frame's age, host to
            # drawn, under the game. Every frame of it too (<tag>.frames.csv)
            # with the page's time origin, for clicks given by the client's OS
            # (POC Ultra U3.7: mw-click-sound). MW_BENCH_HOLD_MARK names a file
            # written as the hold begins, for whatever drives them: this
            # pass's output is read only at its end (local_matrix.py).
            d.eval("window.mwFrameLog && mwFrameLog.clear()")
            if os.environ.get("MW_BENCH_HOLD_MARK"):
                with open(os.environ["MW_BENCH_HOLD_MARK"], "w") as f:
                    f.write("%s %d\n" % (a.tag, a.hold))
            time.sleep(a.hold)
            frame_log = d.eval("window.mwFrameLog ? JSON.stringify(mwFrameLog.summary()) : null")
            time_origin = d.eval("performance.timeOrigin")
            if frame_log and frame_log != "null":
                with open(os.path.join(age.OUT, a.tag + ".frames.csv"), "w", newline="") as f:
                    f.write(d.eval("mwFrameLog.csv()") or "")
            # A still screen: the way up with almost no video coming down.
            uplink = uplink_runs(d, a.uplink, a.tag)
            clicks = click_flag(d, a.clicks, tag=a.tag) if a.clicks > 0 else None
            ticks = ticks_origin(d) if os.environ.get("MW_BENCH_TICKS_ORIGIN") == "1" else None
            if target is rh and rh:
                print("  " + rh.click_target_stop(os.path.join(age.OUT, a.tag + ".target.jsonl")),
                      flush=True)
            elif target:
                target.terminate()
            stats = d.stats()
            player = ultra_player(d, a.tag)
            with open(os.path.join(age.OUT, a.tag + ".json"), "w") as f:
                json.dump({"tag": a.tag, "overlay": stats, "args": vars(a),
                           "uplink": uplink, "clicks": clicks,
                           "frameLog": json.loads(frame_log) if frame_log else None,
                           "timeOrigin": time_origin,
                           "ultraPlayer": player, "ticksOrigin": ticks,
                           "env": {k: os.environ.get(k, "")
                                   for k in ("MW_NATIVE_TUNING", "MW_VDD_REFRESH")}}, f)
            print("  held %d s; %s" % (a.hold, ((stats or {}).get("rows") or {}).get(
                "Framerate:", "")), flush=True)
            return
        if d.eval("typeof (window.mwContentAge && window.mwContentAge.onDecoded)") != "function":
            raise SystemExit("the page runs an older content-age probe")
        page = "scroll.html?band=time&px=%d%s" % (a.px, "&fps=" + a.game_fps if a.game_fps else "")
        if rh:
            print("  " + rh.content_start(page), flush=True)
            shown = True
            content_ms = d.eval("performance.now()")
            time.sleep(4 + a.settle)
            frames_only(d, a.tag, a.secs)
            grid = None
            stepper = stepper_state(d, content_ms)
            clicks = click_flag(d, a.clicks, tag=a.tag) if a.clicks > 0 else None
            uplink = uplink_runs(d, a.uplink, a.tag)
            d.expand_latency_detail()
            stats = d.stats()
            path = os.path.join(age.OUT, a.tag + ".json")
            with open(path) as f:
                data = json.load(f)
            data.update({"overlay": stats, "grid": grid, "stepper": stepper, "clicks": clicks,
                         "uplink": uplink, "args": vars(a), "host": a.host})
            with open(path, "w") as f:
                json.dump(data, f)
            return
        run.content_start(page, probe=False, debug_port=CONTENT_PORT)
        shown = True
        # When the content began to move, on the client's clock: what the
        # detection's decisions are timed from (it tries nothing on a still
        # desktop).
        content_ms = d.eval("performance.now()")
        time.sleep(4)
        age.calibrate(argparse.Namespace(port=CONTENT_PORT, tries=40))
        # The kiosk can stay a blank white window (run.content_start), and its
        # own check cannot read the virtual display: the stream is looked at
        # instead. A band the client cannot read at all is a page not on the
        # screen; it is launched again (U0.3, 03/10/2026: one pass lost so).
        for _again in range(2):
            if band_seen(d):
                break
            print("  the stream shows no band (a blank kiosk?): the page launched again",
                  flush=True)
            run.content_start(page, probe=False, debug_port=CONTENT_PORT)
            time.sleep(4)
            age.calibrate(argparse.Namespace(port=CONTENT_PORT, tries=40))
        time.sleep(a.settle)
        load_seen = {}
        level = None
        if a.gpu_load:
            # Calibration (8 s) and the measurement fit the tool's 60 s.
            a.secs = min(a.secs, 45)
            load, level = start_load(a.gpu_load, a.tag + ".load.jsonl")
        age.run(argparse.Namespace(client="localhost:%d" % (a.client_port or run.DEBUG_PORT),
                                   needle="", secs=a.secs, every=a.every, tag=a.tag,
                                   local=not remote))
        if a.gpu_load:
            load_seen["measure"] = load.snapshot(a.secs)
            load.stop()
        # cadence=deadline: the client's side of the grid — whether the host
        # followed it, the lead it asked for, and how many frames came late.
        grid = d.eval("window.mwVsyncGrid && window.mwVsyncGrid.running ? "
                      "window.mwVsyncGrid.summary : null")
        # "Auto" with detection: where it stands and what it decided, read
        # before the clicks (they move nothing on the screen's content).
        stepper = stepper_state(d, content_ms)
        if a.gpu_load and a.clicks > 0:
            # The same cost again, for the clicks: the level found above.
            load, _ = start_load(a.gpu_load, a.tag + ".load-clicks.jsonl", level)
        clicks = click_flag(d, a.clicks, tag=a.tag) if a.clicks > 0 else None
        if a.gpu_load and a.clicks > 0:
            load_seen["clicks"] = load.snapshot(30)
            load.stop()
        uplink = uplink_runs(d, a.uplink, a.tag)
        d.expand_latency_detail()
        stats = d.stats()
        path = os.path.join(age.OUT, a.tag + ".json")
        with open(path) as f:
            data = json.load(f)
        data["overlay"] = stats
        data["grid"] = grid
        data["stepper"] = stepper
        data["clicks"] = clicks
        data["uplink"] = uplink
        # POC Ultra U1.1: the synthetic Ultra stream's sink, when the page ran
        # one (localStorage mw_ultra_sink): its totals and every second.
        ultra = d.eval("globalThis.__mwUltra ? JSON.stringify(__mwUltra) : null")
        if ultra:
            data["ultra"] = json.loads(ultra)
            last = data["ultra"].get("last") or {}
            print("  ultra: %s frames, %s lost, %s Mbit/s, spread p95 %s ms, delay p95 %s ms" % (
                data["ultra"]["totals"].get("frames"), data["ultra"]["totals"].get("lost"),
                last.get("mbps"), (last.get("spreadMs") or {}).get("p95"),
                (last.get("extraDelayMs") or {}).get("p95")), flush=True)
        # The audio road's (aroad) repair counters since the page loaded: frames
        # given up as lost (each followed by a forced IDR), chunks NACKed, whole
        # frames asked for (none of their chunks came) and chunks asked again.
        rtp = d.eval("globalThis.__mwRtp ? JSON.stringify({lost: __mwRtp.lost || 0, "
                     "nacked: __mwRtp.nacked || 0, whole: __mwRtp.whole || 0, "
                     "reasked: __mwRtp.reasked || 0}) : null")
        if rtp:
            data["rtp"] = json.loads(rtp)
            print("  rtp: %(lost)s frames lost, %(nacked)s chunks NACKed, %(whole)s whole frames "
                  "asked, %(reasked)s asked again" % data["rtp"], flush=True)
        player = ultra_player(d, a.tag)
        if player:
            data["ultraPlayer"] = player
        if load_seen:
            data["load"] = load_seen
            print("  load: " + " | ".join("%s %s fps, GPU %s ms, level %s%s" % (
                k, v.get("fps"), v.get("gpuMs"), v.get("level"),
                "" if v.get("running") else ", ENDED " + str((v.get("end") or {}).get("reason")))
                for k, v in load_seen.items()), flush=True)
        if grid:
            print("  grid: followed %s, lead %s ms, margin %s ms, %s misses in %s frames, "
                  "slack median %s ms (p5 %s)" % (
                      grid.get("followed"), _ms(grid.get("leadMs")), _ms(grid.get("marginMs")),
                      grid.get("misses"), grid.get("frames"), _ms(grid.get("slackMedianMs")),
                      _ms(grid.get("slackP5Ms"))), flush=True)
        data["args"] = vars(a)
        data["env"] = {k: os.environ.get(k, "") for k in ("MW_NATIVE_TUNING", "MW_VDD_REFRESH")}
        with open(path, "w") as f:
            json.dump(data, f)
        rows = (stats or {}).get("rows") or {}
        for k in rows:
            if any(n in k for n in ("Latency", "Framerate", "Resolution", "Codec", "Decode",
                                    "Render", "Host")):
                print("  %-28s %s" % (k, rows[k]))
    finally:
        try:
            d.stop()
        except Exception:
            pass
        if shown:
            if rh:
                print("  " + rh.content_stop(), flush=True)
            else:
                run.content_stop()
        if not remote:
            run.kiosk_stop()


if __name__ == "__main__":
    main()
