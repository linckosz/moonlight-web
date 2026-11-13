"""The owner's latency while guests watch the same native host.

    python owner_load.py --tag rtx-s0 --display-gpu RTX --client-gpu AMD --guest-gpu Arc
    python owner_load.py --tag n95-s0 --host-url https://10.0.0.8:48443/ --ssh bench-intel \\
                         --client-gpu AMD --guest-gpu Arc

Plan « flux commun des invités » (S0 then S9): does a guest cost the owner
anything, and how much? One owner stream on the display a GPU drives, then
invited guests joining on the share board's slots 2-4 by the door a person
uses — the invitation's link, its PIN, the Join button — in a schedule of
windows (--guests 0,1,3,0: the reference replayed at the end, so a drift of the
machine itself shows).

Each window reads, for the owner:
- the host's own stages, frame by frame (present → acquire → convert → encode →
  queue → send, and the total), from the stats messages the host sends the
  owner's page every second (hook.js, a prototype patch — nothing in the app
  changes);
- the overlay's end-to-end latency and frame rate;
and, for the machine: how many stream workers run, NVENC's own session count
when the GPU is NVIDIA, and the video-encode engines' load (local host only).
The guests' own stages are read the same way, from their pages.

Clients: the owner's Chrome decodes on --client-gpu (its screen), the guests'
on --guest-gpu, three windows side by side on that GPU's screen; neither is the
encoder's GPU (docs/bench-campaign.md §4). The captured screen shows a moving
bench page (content/scroll.html) for the whole run.

A local host is the --dev instance (MW_BENCH_LOCAL_PORTS, as the acceptance run
reads it). A remote one (--host-url) is reached over the LAN; its PIN is minted
over SSH (--ssh) by fleet.py, and so is its worker count.
"""
import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(BENCH))
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
import run, drive, fleet  # noqa: E402

OUT = os.path.join(REPO, "bench-out", "shared-feed")
HOOK = open(os.path.join(HERE, "hook.js"), encoding="utf-8").read()
STAGES = ("acquire", "convert", "encode", "queue", "send", "total")
GUEST_PORT = 9340  # + slot
GUEST_W, GUEST_H = 1280, 720


# ── screens and GPUs ────────────────────────────────────────────────────────

def monitors():
    """{"\\\\.\\DISPLAY5": (x, y, w, h)}, physical pixels (monitors.ps1)."""
    out = subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(BENCH, "monitors.ps1")],
                         capture_output=True, text=True).stdout
    rects = {}
    for line in out.splitlines():
        m = re.match(r"(\S+) (-?\d+),(-?\d+) (\d+)x(\d+)", line.strip())
        if m:
            rects[m.group(1)] = tuple(int(m.group(i)) for i in range(2, 6))
    return rects


def gpu(name):
    """(full name, "high,low" LUID, (x, y, w, h) of its screen) for the GPU
    whose name contains `name`. A GPU with no screen is refused: a client there
    would present through another GPU (bench-campaign §4)."""
    g = next((g for g in run.list_gpus() if name.lower() in g[0].lower()), None)
    if not g:
        raise SystemExit("no GPU named like %r" % name)
    rect = monitors().get(g[2]) if g[2] else None
    if not rect:
        raise SystemExit("the %s drives no screen on the desktop" % g[0])
    return g[0], g[1], rect


# ── the machine ─────────────────────────────────────────────────────────────

def dev_server_pid():
    """The --dev instance's server: a MoonlightWeb.exe in the console session
    whose parent is not one — an elevated one hides its command line."""
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-CimInstance Win32_Process -Filter \"Name='MoonlightWeb.exe'\" | "
                          "ForEach-Object { '{0} {1} {2} {3}' -f $_.ProcessId, $_.ParentProcessId, "
                          "$_.SessionId, $_.ExecutablePath }"],
                         capture_output=True, text=True).stdout
    procs = []
    for line in out.splitlines():
        parts = line.strip().split(" ", 3)
        if len(parts) >= 3:
            procs.append((int(parts[0]), int(parts[1]), int(parts[2]),
                          parts[3] if len(parts) > 3 else ""))
    pids = {p[0] for p in procs}
    for pid, ppid, session, path in procs:
        if session != 0 and ppid not in pids and "Program Files" not in path:
            return pid, [p[0] for p in procs if p[1] == pid]
    return None, []


def worker_count(ssh):
    if ssh:
        # The edition installed there streams through its SYSTEM launcher: the
        # workers are the MoonlightWebDev.exe whose parent is one too (the
        # service). A script copied there beforehand (README): quotes do not
        # survive two shells on the way.
        out = subprocess.run(["ssh", ssh, "powershell -NoProfile -ExecutionPolicy Bypass -File "
                              r"C:\Users\Public\mw-run\count-workers.ps1 < NUL"],
                             capture_output=True, text=True, timeout=60).stdout.strip()
        try:
            return int(out.splitlines()[-1])
        except (ValueError, IndexError):
            return None
    _pid, children = dev_server_pid()
    return len(children)


WORKER_LOGS = os.path.join(os.environ.get("APPDATA", ""), "MoonlightWeb", "MoonlightWeb-dev", "logs")


def sessions(since):
    """The native sessions of the --dev instance's workers since `since`:
    what each one encodes, and whether it still runs (a worker writes its stage
    summary when its session ends)."""
    import glob
    out = []
    for p in sorted(glob.glob(os.path.join(WORKER_LOGS, "moonlightweb-worker-*.log")),
                    key=os.path.getmtime):
        if os.path.getmtime(p) < since:
            continue
        with open(p, encoding="utf-8", errors="replace") as f:
            text = f.read()
        # A PID comes back: only the lines written since this run started.
        lines = [l for l in text.splitlines() if l[1:20] >= time.strftime(
            "%Y-%m-%d %H:%M:%S", time.localtime(since))]
        sess = [l for l in lines if "[native] session:" in l]
        if not sess:
            continue
        streaming = [l for l in lines if "[NativeMediaEngine] streaming" in l]
        out.append({
            "log": os.path.basename(p),
            "session": sess[-1].split("[native] session:", 1)[1].strip(),
            "streaming": streaming[-1].split("streaming", 1)[1].strip() if streaming else "",
            "ended": any("host stages over" in l for l in lines if l >= sess[-1][:25]),
        })
    return out


def nvenc():
    out = subprocess.run(["nvidia-smi", "--query-gpu=name,encoder.stats.sessionCount,"
                          "encoder.stats.averageFps,encoder.stats.averageLatency",
                          "--format=csv,noheader,nounits"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        f = [x.strip() for x in line.split(",")]
        if len(f) == 4:
            return {"gpu": f[0], "sessions": int(f[1]), "fps": int(f[2]), "latencyUs": int(f[3])}
    return None


def encode_engines():
    """The video engines' load per adapter, in percent (one WMI read, which
    Windows itself averages over its last second): {luid: {engine type: %}}.
    Every "Video…" type is kept: Intel's encoder shows as Video Decode, not
    Video Encode (bench §8n, the N95)."""
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Get-CimInstance Win32_PerfFormattedData_GPUPerformanceCounters_GPUEngine | "
                          "Where-Object { $_.Name -like '*engtype_Video*' } | "
                          "ForEach-Object { '{0} {1}' -f $_.Name, $_.UtilizationPercentage }"],
                         capture_output=True, text=True).stdout
    load = {}
    for line in out.splitlines():
        m = re.search(r"luid_0x([0-9A-Fa-f]+)_0x([0-9A-Fa-f]+)_.*engtype_(\S+) (\d+)$",
                      line.strip())
        if m:
            luid = "%d,%d" % (int(m.group(1), 16), int(m.group(2), 16))
            engines = load.setdefault(luid, {})
            engines[m.group(3)] = engines.get(m.group(3), 0) + int(m.group(4))
    return load


# ── what the pages say ──────────────────────────────────────────────────────

def summarize(windows):
    """The host's stats windows of one measurement window, folded: the mean
    weighted by frames, the median of the per-second p50/p95/p99, the worst
    p99 and the worst frame."""
    out = {"seconds": len(windows)}
    for stage in STAGES:
        rows = [w["stages"][stage] for _t, w in windows
                if isinstance(w.get("stages"), dict) and stage in w["stages"]]
        rows = [r for r in rows if r.get("n", 0) > 0]
        if not rows:
            continue
        n = sum(r["n"] for r in rows)
        out[stage] = {
            "frames": n,
            "avgMs": round(sum(r["avg"] * r["n"] for r in rows) / n / 1000, 3),
            "p50Ms": round(statistics.median(r["p50"] for r in rows) / 1000, 3),
            "p95Ms": round(statistics.median(r["p95"] for r in rows) / 1000, 3),
            "p99Ms": round(statistics.median(r["p99"] for r in rows) / 1000, 3),
            "p99WorstMs": round(max(r["p99"] for r in rows) / 1000, 3),
            "maxMs": round(max(r["max"] for r in rows) / 1000, 3),
        }
    return out


def overlay(d):
    try:
        s = d.stats()
    except drive.PassFailed:
        return {}
    rows = s.get("rows") or {}
    keep = {k: v for k, v in rows.items()
            if any(n in k for n in ("Latency", "Framerate", "Resolution", "Codec", "Encoder",
                                    "Decode", "Bitrate", "Host"))}
    keep["_legs"] = s.get("legs") or {}
    keep["_visibility"] = s.get("visibility")
    return keep


def first_number(text):
    m = re.search(r"(-?\d+(?:\.\d+)?)", text or "")
    return float(m.group(1)) if m else None


# ── the guests ──────────────────────────────────────────────────────────────

class Guest:
    def __init__(self, slot, rect, luid, patience=40, no_hevc=False):
        self.slot = slot
        self.patience = patience
        # A browser that decodes no HEVC: its join turns the guests' shared
        # feed H.264 for everyone (S6), the others' pages rejoining in it.
        self.no_hevc = no_hevc
        self.rect = rect
        self.luid = luid
        self.port = GUEST_PORT + slot
        self.profile = ".chrome-guest-%d" % slot
        self.d = None

    def join(self, owner, base_url):
        """Open the row on the board (owner's page), then have this guest's
        own Chrome open the link, enter the PIN and press Join."""
        act = owner.json_eval("""(async () => {
            const r = await fetch('/api/share/slots/%d/activate', {method: 'POST',
                credentials: 'same-origin', headers: {'Content-Type': 'application/json'},
                body: JSON.stringify({ttl_secs: 3600})});
            return JSON.stringify(Object.assign({status: r.status}, await r.json()));
        })()""" % self.slot)
        link = act.get("url", "")
        m = re.search(r"/p/([^/?#]+)", link) or re.search(r"#t=([^&]+)", link)
        if not m or not act.get("pin"):
            raise drive.PassFailed("slot %d did not open: %s" % (self.slot, act))
        url = base_url.rstrip("/") + "/p/" + m.group(1)
        x, y, w, h = self.rect
        subprocess.run(["powershell", "-NoProfile", "-File", os.path.join(BENCH, "kiosk.ps1"),
                        "-Url", url, "-X", str(x), "-Y", str(y), "-W", str(w), "-H", str(h),
                        "-DebugPort", str(self.port), "-ChromeProfile", self.profile,
                        "-AdapterLuid", self.luid, "-Windowed"]
                       + (["-DisableFeatures", "PlatformHEVCDecoderSupport"] if self.no_hevc else []),
                       capture_output=True, text=True)
        self.d = drive.Driver(self.port)
        # On every document this Chrome opens from now on, then on this one:
        # the Join that creates the DataChannel comes after the PIN step.
        self.d.call("Page.addScriptToEvaluateOnNewDocument", source=HOOK)
        self.d.eval(HOOK)
        if not self._wait("!!document.querySelector('.player-pin-input')", self.patience):
            raise drive.PassFailed("guest %d: the page never asked for the PIN" % self.slot)
        self.d.eval("(() => { const i = document.querySelector('.player-pin-input'); i.value = %s; "
                    "i.form.requestSubmit(); return 1; })()" % json.dumps(act["pin"]))
        if not self._wait("!!document.querySelector('.player-join-btn')", self.patience):
            raise drive.PassFailed("guest %d: no Join button after the PIN" % self.slot)
        self.d.eval(HOOK)  # idempotent; the PIN step may have reloaded the page
        pos = self.d.json_eval("""(() => { const e = document.querySelector('.player-join-btn');
            const r = e.getBoundingClientRect();
            return JSON.stringify({x: r.left + r.width / 2, y: r.top + r.height / 2}); })()""")
        for kind in ("mouseMoved", "mousePressed", "mouseReleased"):
            self.d.call("Input.dispatchMouseEvent", type=kind, x=pos["x"], y=pos["y"],
                        button="left", clickCount=1)
            time.sleep(0.05)
        if not self._wait("!!document.querySelector('canvas')", self.patience + 20):
            raise drive.PassFailed("guest %d: no picture after Join" % self.slot)

    def _wait(self, js, secs):
        end = time.time() + secs
        while time.time() < end:
            try:
                if "true" in str(self.d.eval("JSON.stringify(%s)" % js)).lower():
                    return True
            except drive.PassFailed:
                pass
            time.sleep(1)
        return False

    def take(self):
        try:
            return self.d.json_eval("JSON.stringify(window.__mwS0 ? window.__mwS0.take() : [])")
        except drive.PassFailed:
            return []

    def codec(self):
        """What each of this page's joins was answered (the codec its worker
        streams), and the shared feed's `feedcodec` notices it got."""
        try:
            return self.d.json_eval("JSON.stringify(window.__mwS0 ? {joins: window.__mwS0.joins, "
                                    "notices: window.__mwS0.notices.length, "
                                    "logs: window.__mwS0.logs.splice(0)} : {})")
        except drive.PassFailed:
            return {}

    def leave(self, owner):
        """The owner closes the row: the worker goes at once (a guest who only
        closes the window is noticed when the link times out)."""
        try:
            owner.eval("fetch('/api/share/slots/%d/deactivate', {method: 'POST', credentials: "
                       "'same-origin', headers: {'Content-Type': 'application/json'}, body: '{}'})"
                       ".then(r => r.status)" % self.slot)
        except drive.PassFailed:
            pass
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
                        "Where-Object { $_.CommandLine -like '*%s*' } | "
                        "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }" % self.profile],
                       capture_output=True, text=True)


# ── one run ─────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--tag", required=True)
    ap.add_argument("--display-gpu", default="", help="the local display to stream: its GPU (RTX, Arc, AMD)")
    ap.add_argument("--client-gpu", required=True, help="where the owner's Chrome decodes")
    ap.add_argument("--guest-gpu", required=True, help="where the guests' Chromes decode")
    ap.add_argument("--guests", default="0,1,3,0", help="guests present in each window, in order")
    ap.add_argument("--secs", type=float, default=30, help="one measurement window")
    ap.add_argument("--settle", type=float, default=10, help="after the guests changed")
    ap.add_argument("--content", default="scroll.html?px=600",
                    help="the bench page over the captured screen (local host)")
    ap.add_argument("--host-url", default="", help="a remote host's address (LAN)")
    ap.add_argument("--ssh", default="", help="SSH alias of the remote host (PIN, worker count)")
    ap.add_argument("--fleet-id", default="", help="its id in hosts.local.json, for fleet.probe")
    ap.add_argument("--display-index", default="0", help="remote host: which physical display")
    ap.add_argument("--feed-heights", default="",
                    help="the guests' picture height the owner picks before each window, in "
                         "order (\"1080,720,1440\"; empty keeps it): a feed with guests on it is "
                         "resized under them")
    ap.add_argument("--no-hevc-slots", default="",
                    help="guests whose Chrome decodes no HEVC, by slot (\"4\"): the shared feed "
                         "goes H.264 when one joins")
    ap.add_argument("--join-patience", type=int, default=40,
                    help="seconds a guest's page may take at each step of joining: a host that "
                         "is already on its knees serves its page slowly")
    a = ap.parse_args()
    schedule = [int(x) for x in a.guests.split(",")]
    no_hevc = {int(x) for x in a.no_hevc_slots.split(",") if x.strip()}
    feed_heights = [int(x) if x.strip() else 0 for x in a.feed_heights.split(",")]         if a.feed_heights else []
    remote = bool(a.host_url)
    os.makedirs(OUT, exist_ok=True)

    client_name, client_luid, client_rect = gpu(a.client_gpu)
    guest_name, guest_luid, guest_rect = gpu(a.guest_gpu)
    if client_luid == guest_luid:
        print("  note: the owner's client and the guests decode on the same GPU", flush=True)

    if remote:
        mid = a.fleet_id or "mw-intel"
        probe = fleet.probe(mid)
        # In memory only (fleet.mint_pin).
        pin = fleet.mint_pin(mid, probe)
        base = a.host_url
        access = {"name": "bench", "pin": pin}
        display_index = int(a.display_index)
        encoder = {"gpu": "remote", "luid": None}
    else:
        access = dict(run.access_map().get("local") or {})
        probe = fleet.probe("local")
        pin = fleet.mint_pin("local", probe)
        if pin:
            access["pin"] = pin
        base = fleet.lan_url("local", probe) or access.get("lan")
        enc_name, enc_luid, enc_rect = gpu(a.display_gpu)
        if enc_luid in (client_luid, guest_luid):
            raise SystemExit("the encoder's GPU (%s) would also decode: move a client" % enc_name)
        encoder = {"gpu": enc_name, "luid": enc_luid}
        http_port = os.environ.get("MW_BENCH_LOCAL_PORTS", "18080,18443").split(",")[0]
        import gpu_load
        status = gpu_load.native_status(int(http_port))
        displays = sorted(status.get("displays") or [], key=lambda x: x["id"])
        display_index = next((i for i, x in enumerate(displays)
                              if a.display_gpu.lower() in x.get("gpu", "").lower()), None)
        if display_index is None:
            raise SystemExit("the native host offers no display on the %s: %s" % (enc_name, displays))
        encoder["display"] = displays[display_index]
        os.environ["MW_BENCH_CONTENT_RECT"] = "%d,%d,%d,%d" % enc_rect

    result = {"tag": a.tag, "args": vars(a), "encoder": encoder,
              "client": {"gpu": client_name, "luid": client_luid},
              "guests": {"gpu": guest_name, "luid": guest_luid}, "windows": []}
    print("encoder %s · owner decodes on %s · guests on %s" % (encoder["gpu"], client_name,
                                                                guest_name), flush=True)

    # Owner: the acceptance run's client, on its own GPU's screen.
    started = time.time()
    run.kiosk_start(base, on_gpu=a.client_gpu)
    d = drive.Driver(run.DEBUG_PORT)
    guests = {}
    shown = False
    try:
        d.navigate(base)
        d.eval("(async () => { await caches.delete('mw-shell'); for (const r of await "
               "navigator.serviceWorker.getRegistrations()) await r.unregister(); return 1; })()")
        d.navigate(base)
        d.wait_library(access.get("name", "bench"), access.get("pin", ""), tries=25)
        settings = dict(run.load_matrix()["base"])
        settings.update({"stream_fps": 60})
        d.apply_settings(settings)
        d.wait_library(access.get("name", "bench"), access.get("pin", ""), tries=25)
        # The library and the stream are one document: the hook goes in now,
        # before the launch creates the DataChannel. A script registered on the
        # DevTools session would not survive the reconnects drive.py makes.
        d.eval(HOOK)
        os.environ["MW_BENCH_DISPLAY"] = str(display_index)
        card, app = d.pick_tile("display")
        print("tile", card.get("name"), "/", app.get("name"), flush=True)
        if not remote:
            run.content_start(a.content, probe=False)
            shown = True
        d.launch(card, app)
        d.wait_picture(timeout=60)
        if d.eval("typeof window.__mwS0") != "object":
            raise SystemExit("the stats hook is not in the owner's page")
        time.sleep(a.settle)

        gx, gy, _gw, _gh = guest_rect
        slots = [2, 3, 4]
        spots = [(gx, gy), (gx + GUEST_W, gy), (gx, gy + GUEST_H)]
        for w_index, want in enumerate(schedule):
            height = feed_heights[w_index] if w_index < len(feed_heights) else 0
            if height:
                # The owner's pick on the board, as its button sends it.
                got = d.json_eval("""(async () => {
                    const r = await fetch('/api/share/feed', {method: 'POST',
                        credentials: 'same-origin', headers: {'Content-Type': 'application/json'},
                        body: JSON.stringify({height: %d})});
                    return JSON.stringify(Object.assign({status: r.status}, await r.json()));
                })()""" % height)
                print("  guests' picture -> %dp (%s)" % (height, got.get("status")), flush=True)
            # Guests arrive and leave in slot order: 1 = slot 2, 3 = slots 2-4.
            for slot in slots[want:]:
                if slot in guests:
                    guests.pop(slot).leave(d)
            for i, slot in enumerate(slots[:want]):
                if slot not in guests:
                    g = Guest(slot, (spots[i][0], spots[i][1], GUEST_W, GUEST_H), guest_luid,
                              a.join_patience, slot in no_hevc)
                    print("  guest %d joins%s" % (slot, " (no HEVC)" if g.no_hevc else ""),
                          flush=True)
                    try:
                        g.join(d, base)
                    except drive.PassFailed:
                        # Its window and its row go with the run.
                        g.leave(d)
                        raise
                    guests[slot] = g
            time.sleep(a.settle)
            d.json_eval("JSON.stringify(window.__mwS0.take().length)")
            for g in guests.values():
                g.take()
            t0 = time.time()
            engines = {}
            if not remote:
                sampler = threading.Thread(target=lambda: engines.update(encode_engines()))
                time.sleep(a.secs / 2 - 3)
                sampler.start()
                time.sleep(a.secs / 2 + 3)
                sampler.join(timeout=30)
            else:
                time.sleep(a.secs)
            owner_stats = d.json_eval("JSON.stringify(window.__mwS0.take())")
            win = {
                "guests": want, "at": t0, "feedHeight": height or None,
                "owner": summarize(owner_stats),
                "ownerOverlay": overlay(d),
                "guestStages": {slot: summarize(g.take()) for slot, g in guests.items()},
                "guestOverlay": {slot: overlay(g.d) for slot, g in guests.items()},
                "guestCodec": {slot: g.codec() for slot, g in guests.items()},
                "workers": worker_count(a.ssh if remote else ""),
                "sessions": [] if remote else [s for s in sessions(started) if not s["ended"]],
                "nvenc": nvenc() if "NVIDIA" in encoder["gpu"] else None,
                "encodeEngines": engines,
            }
            result["windows"].append(win)
            line(win)
            with open(os.path.join(OUT, a.tag + ".json"), "w") as f:
                json.dump(result, f, indent=1)
    finally:
        for g in list(guests.values()):
            g.leave(d)
        try:
            d.stop()
        except Exception:
            pass
        if shown:
            run.content_stop()
        run.kiosk_stop()
    print("saved", os.path.join(OUT, a.tag + ".json"))


def fmt(v, w=6):
    return ("%*.2f" % (w, v)) if isinstance(v, (int, float)) else " " * (w - 1) + "-"


def line(win):
    o = win["owner"]
    g = lambda s, k: (o.get(s) or {}).get(k)  # noqa: E731
    ov = win["ownerOverlay"]
    lat = first_number(ov.get("Latency:"))
    fps = first_number(ov.get("Framerate:"))
    nv = win.get("nvenc") or {}
    guest_enc = [((s.get("encode") or {}).get("p50Ms")) for s in win["guestStages"].values()]
    guest_fps = [(s.get("total") or {}).get("frames", 0) / max(1, s.get("seconds", 1))
                 for s in win["guestStages"].values()]
    guest_codec = ["%s%s" % ("/".join(j[1] for j in (c.get("joins") or [])) or "?",
                             " +%d notice(s)" % c["notices"] if c.get("notices") else "")
                   for c in (win.get("guestCodec") or {}).values()]
    print("  guests %d | owner total p50 %s p99 %s | encode p50 %s p99 %s | acquire p50 %s | "
          "queue p99 %s | E2E %s ms %s fps | workers %s sessions %s%s | guests' encode p50 %s "
          "at %s fps (%s)" % (
              win["guests"], fmt(g("total", "p50Ms")), fmt(g("total", "p99Ms")),
              fmt(g("encode", "p50Ms")), fmt(g("encode", "p99Ms")), fmt(g("acquire", "p50Ms")),
              fmt(g("queue", "p99Ms")), fmt(lat), fmt(fps, 5), win["workers"],
              len(win.get("sessions") or []) or "-",
              (" | NVENC sessions %s avg %s us" % (nv.get("sessions"), nv.get("latencyUs"))) if nv else "",
              ",".join(fmt(x, 5) for x in guest_enc) or "-",
              ",".join("%.0f" % x for x in guest_fps) or "-",
              ", ".join(guest_codec) or "-"), flush=True)


if __name__ == "__main__":
    main()
