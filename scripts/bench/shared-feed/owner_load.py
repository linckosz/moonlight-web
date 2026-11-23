"""The owner's latency while guests watch the same native host.

    python owner_load.py --tag rtx-s0 --display-gpu RTX --client-gpu AMD --guest-gpu Arc
    python owner_load.py --tag n95-s0 --host-url https://10.0.0.8:48443/ --fleet-id mw-intel \\
                         --client-gpu AMD --guest-gpu Arc
    python owner_load.py ... --client-gpu RTX --client-screen \\\\.\\DISPLAY5 \\
                             --guest-gpu RTX --guest-screen \\\\.\\DISPLAY5 --guests 0,1,0

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
over SSH by fleet.py (--fleet-id), and its workers are not counted.

--client-screen and --guest-screen put the windows on a given screen of their
GPU instead of its first one; a guest window that would not fit inside the
guests' screen stops the run before any window opens.
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
import guest_link as gl  # noqa: E402

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


FORBIDDEN_SCREEN = run.FORBIDDEN_SCREEN
overlaps = run.overlaps


def gpu(name, screen=""):
    """(full name, "high,low" LUID, (x, y, w, h) of its screen, the screen's
    name, whether that rectangle is in physical pixels) for the GPU whose name
    contains `name`. A GPU with no screen is refused: a client there would
    present through another GPU (bench-campaign §4). So is FORBIDDEN_SCREEN.

    `screen` ("\\\\.\\DISPLAY5") puts the windows on that screen rather than the
    GPU's first one; the GPU is then the adapter behind it, by identity
    (run.gpu_on_screen). Its rectangle is in the coordinates kiosk.ps1 takes
    (unaware of the scaling), which the guests' windows are opened with."""
    if screen:
        run.check_screen(screen)
        if screen.lower() == FORBIDDEN_SCREEN.lower():
            raise SystemExit("%s is refused: no bench window on that screen" % screen)
        full, luid = run.gpu_on_screen(name, screen)
        rect = run.screen_bounds(screen)
        if not rect:
            raise SystemExit("%s is not on the desktop" % screen)
        return full, luid, rect, screen, False
    g = run.gpu_by_name(name)
    if not g:
        raise SystemExit("no GPU named like %r" % name)
    if g[2] and g[2].lower() == FORBIDDEN_SCREEN.lower():
        raise SystemExit("the %s's first screen is %s, which is refused: name another of its "
                         "screens (--client-screen / --guest-screen)" % (g[0], g[2]))
    rect = monitors().get(g[2]) if g[2] else None
    if not rect:
        raise SystemExit("the %s drives no screen on the desktop" % g[0])
    return g[0], g[1], rect, g[2], True


def check_windows(owner_screen, guest_rect, guest_physical, guests):
    """Every window the run opens, before any opens: the owner's at the size
    kiosk_start gives it, and each guest's, inside its own screen and clear of
    FORBIDDEN_SCREEN. The owner is placed in unaware coordinates; the guests in
    those of guest_rect (physical on the default path, as before)."""
    owner = run.kiosk_rect(run.screen_bounds(owner_screen))
    if guests > 3:
        raise SystemExit("at most 3 guests: the board has slots 2-4")
    gx, gy, gw, gh = guest_rect
    spots = [(gx, gy), (gx + GUEST_W, gy), (gx, gy + GUEST_H)][:guests]
    forbidden_unaware = run.screen_bounds(FORBIDDEN_SCREEN)
    forbidden_guest = (monitors().get(FORBIDDEN_SCREEN) if guest_physical
                       else forbidden_unaware)
    windows = [("the owner's window", owner, run.screen_bounds(owner_screen), forbidden_unaware)]
    windows += [("guest window %d" % (i + 2), (sx, sy, GUEST_W, GUEST_H), guest_rect,
                 forbidden_guest) for i, (sx, sy) in enumerate(spots)]
    for label, r, screen, forbidden in windows:
        x, y, w, h = r
        sx, sy, sw, sh = screen
        if x < sx or y < sy or x + w > sx + sw or y + h > sy + sh:
            raise SystemExit("%s at %d,%d %dx%d would leave its screen %d,%d %dx%d: fewer "
                             "guests, or another screen" % (label, x, y, w, h, sx, sy, sw, sh))
        if forbidden and overlaps(r, forbidden):
            raise SystemExit("%s at %d,%d %dx%d would reach %s" % (label, x, y, w, h,
                                                                  FORBIDDEN_SCREEN))
    return spots


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


def worker_count():
    """The local --dev instance's stream workers. A remote host's are not
    counted: that took one more SSH per window, and the script it ran counted
    the DEV edition's processes whatever edition was measured."""
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


def parse_feed_height(text):
    """One --feed-heights entry: None keeps the owner's pick, 0 is Auto (ShareManager::kFeedAuto)."""
    text = text.strip().lower()
    if not text:
        return None
    return FEED_AUTO if text == "auto" else int(text)


FEED_AUTO = 0


def feed_height(d):
    """The guests' picture height the host keeps (0 = Auto). Only that field leaves the page:
    the status also carries the board's rows."""
    return d.json_eval("""(async () => {
        const r = await fetch('/api/share/status', {credentials: 'same-origin'});
        const j = r.ok ? await r.json() : {};
        return JSON.stringify({status: r.status, height: j.feed_height});
    })()""")


def pick_feed_height(d, height):
    """The owner's pick on the board, as its button sends it."""
    return d.json_eval("""(async () => {
        const r = await fetch('/api/share/feed', {method: 'POST',
            credentials: 'same-origin', headers: {'Content-Type': 'application/json'},
            body: JSON.stringify({height: %d})});
        const j = r.ok ? await r.json() : {};
        return JSON.stringify({status: r.status, height: j.feed_height});
    })()""" % height)


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
        own Chrome open the link, enter the PIN and press Join. The link and
        the PIN stay in memory (guest_link)."""
        share = gl.activate(owner, self.slot)
        gl.open_kiosk(self.port, self.profile, self.rect,
                      ["-AdapterLuid", self.luid, "-Windowed"]
                      + (["-DisableFeatures", "PlatformHEVCDecoderSupport"] if self.no_hevc else []))
        self.d = drive.Driver(self.port)
        gl.go(self.port, base_url, share)
        # On every document this Chrome opens from now on, then on this one:
        # the Join that creates the DataChannel comes after the PIN step.
        self.d.call("Page.addScriptToEvaluateOnNewDocument", source=HOOK)
        self.d.eval(HOOK)
        if not self._wait("!!document.querySelector('.player-pin-input')", self.patience):
            raise drive.PassFailed("guest %d: the page never asked for the PIN" % self.slot)
        gl.enter_pin(self.d, share)
        share = None
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
        """The owner closes the row: the worker goes at once, and the host
        revokes the link and the PIN (a guest who only closes the window is
        noticed when the link times out). Then the guest's Chrome closes and
        its profile goes. Returns the warnings, if any."""
        warnings = []
        if not gl.deactivate(owner, self.slot):
            warnings.append("slot %d may still be open: close it on the share board" % self.slot)
        left = gl.close_kiosk(self.profile)
        if left:
            warnings.append(left)
        return warnings


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
    ap.add_argument("--fleet-id", default="", help="its id in hosts.local.json, for fleet.probe "
                                                    "and the PIN (fleet.py's own SSH)")
    ap.add_argument("--client-screen", default="",
                    help="the owner's window on this screen (\"\\\\.\\DISPLAY5\"), decoded by the "
                         "adapter behind it, whose name must contain --client-gpu")
    ap.add_argument("--guest-screen", default="",
                    help="the guests' windows on this screen, the same way with --guest-gpu")
    ap.add_argument("--display-index", default="0", help="remote host: which physical display")
    ap.add_argument("--feed-heights", default="",
                    help="the guests' picture height the owner picks before each window, in "
                         "order (\"1080,720,1440\", \"auto\"; empty keeps it): a feed with guests "
                         "on it is resized under them. The host keeps the owner's pick, so a run "
                         "that picked one puts Auto back at its end, failed or not")
    ap.add_argument("--no-hevc-slots", default="",
                    help="guests whose Chrome decodes no HEVC, by slot (\"4\"): the shared feed "
                         "goes H.264 when one joins")
    ap.add_argument("--join-patience", type=int, default=40,
                    help="seconds a guest's page may take at each step of joining: a host that "
                         "is already on its knees serves its page slowly")
    a = ap.parse_args()
    schedule = [int(x) for x in a.guests.split(",")]
    no_hevc = {int(x) for x in a.no_hevc_slots.split(",") if x.strip()}
    feed_heights = [parse_feed_height(x) for x in a.feed_heights.split(",")] if a.feed_heights else []
    remote = bool(a.host_url)
    os.makedirs(OUT, exist_ok=True)

    client_name, client_luid, _client_rect, client_screen, _ = gpu(a.client_gpu, a.client_screen)
    guest_name, guest_luid, guest_rect, _guest_screen, guest_physical = gpu(a.guest_gpu,
                                                                             a.guest_screen)
    if client_luid == guest_luid:
        print("  note: the owner's client and the guests decode on the same GPU", flush=True)
    # Before any window opens: one that overflows lands on the next screen.
    spots = check_windows(client_screen, guest_rect, guest_physical, max(schedule + [0]))

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
        enc_name, enc_luid, enc_rect, _enc_screen, _ = gpu(a.display_gpu)
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
    gl.keep_output()
    warnings = []
    run.kiosk_start(base, on_gpu=a.client_gpu, screen=a.client_screen or None)
    d = drive.Driver(run.DEBUG_PORT)
    guests = {}
    shown = False
    feed_picked = False
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

        slots = [2, 3, 4]
        for w_index, want in enumerate(schedule):
            height = feed_heights[w_index] if w_index < len(feed_heights) else None
            if height is not None:
                if not feed_picked:
                    was = feed_height(d)
                    print("  guests' picture before the run's pick: %s (%s)"
                          % (was.get("height"), was.get("status")), flush=True)
                # Set before the POST: a pick the host took but whose answer was lost still
                # needs Auto put back.
                feed_picked = True
                got = pick_feed_height(d, height)
                print("  guests' picture -> %s (%s)" % ("Auto" if height == FEED_AUTO
                                                        else "%dp" % height, got.get("status")),
                      flush=True)
            # Guests arrive and leave in slot order: 1 = slot 2, 3 = slots 2-4.
            for slot in slots[want:]:
                if slot in guests:
                    warnings += guests.pop(slot).leave(d)
            for i, slot in enumerate(slots[:want]):
                if slot not in guests:
                    g = Guest(slot, (spots[i][0], spots[i][1], GUEST_W, GUEST_H), guest_luid,
                              a.join_patience, slot in no_hevc)
                    print("  guest %d joins%s" % (slot, " (no HEVC)" if g.no_hevc else ""),
                          flush=True)
                    try:
                        g.join(d, base)
                    except BaseException:
                        # Its window, its profile and its row go with the run.
                        warnings += g.leave(d)
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
                "guests": want, "at": t0, "feedHeight": height,
                "owner": summarize(owner_stats),
                "ownerOverlay": overlay(d),
                "guestStages": {slot: summarize(g.take()) for slot, g in guests.items()},
                "guestOverlay": {slot: overlay(g.d) for slot, g in guests.items()},
                "guestCodec": {slot: g.codec() for slot, g in guests.items()},
                "workers": None if remote else worker_count(),
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
            warnings += g.leave(d)
        # The host keeps the owner's pick on disk (share.json): a run's 720 would
        # otherwise size every later guest (22/11: Auto lost since the run before).
        # Before the logout, which takes the owner's right to change it.
        if feed_picked:
            try:
                back = pick_feed_height(d, FEED_AUTO)
                now = feed_height(d)
                print("  guests' picture put back to Auto: %s (%s), now %s"
                      % (back.get("height"), back.get("status"), now.get("height")), flush=True)
                if now.get("height") != FEED_AUTO:
                    raise RuntimeError("the host reads %r" % now.get("height"))
            except Exception as e:
                warnings.append("the guests' picture could not be put back to Auto (%s): pick "
                                "Auto on the share board" % e)
        # The owner's session ends with the run, as the page ends it (the host
        # revokes it): the PIN pairing of this run would otherwise stay in the
        # host's list of sessions. After the guests, whose slots it closes.
        try:
            import owner_kiosk
            ended = owner_kiosk.logout(run.DEBUG_PORT)
        except Exception:
            ended = False
        print("owner session ended by the page: %s" % ended, flush=True)
        if not ended:
            warnings.append("the owner session could not be ended by the page: "
                            "revoke it in Active Sessions")
        try:
            d.stop()
        except Exception:
            pass
        if shown:
            run.content_stop()
        run.kiosk_stop()
        gl.proof(OUT, started, dev_logs=not remote)
        for w in warnings:
            print("WARNING:", w, flush=True)
    print("saved", os.path.join(OUT, a.tag + ".json"))
    # Said again last, where they cannot be lost in the windows' lines.
    for w in warnings:
        print("WARNING:", w, flush=True)


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
