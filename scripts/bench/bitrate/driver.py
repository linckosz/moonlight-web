"""Auto-bitrate bench driver: one combination, a list of bitrates, one run each.

    py -E -X utf8 driver.py --geo 1920x1080 --fps 60 --codec hevc --kbps 2000,4000 \
        --host-url https://<host>:8443/ --client-cdp 9424 --out runs\\A-1080p60-hevc \
        --scenes phase1

Topology (bras A): this machine runs the --dev (NVENC) and, on the Virtual Display the
stream creates, a lab content Chrome showing content/bench.html. The client Chrome
(a second machine) is reached over an ssh tunnel (-L client-cdp, -R 18471 for the
collector). tap.js is injected in the client page before it loads.

Per bitrate: settings -> reload -> Virtual Display tile -> wait for the new screen ->
content Chrome on it, fullscreen -> scenes -> content Chrome closed (so nothing lands on
the desk's screens when the virtual display goes) -> tap flushed -> stream stopped.
Nothing here clicks or types on the host desktop: the content Chrome is only placed.
"""
import argparse
import base64
import ctypes
import ctypes.wintypes as wt
import json
import os
import subprocess
import sys
import time
import urllib.request
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nowin  # noqa: E402,F401  no child console window, ever

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "acceptance"))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
import cdp  # noqa: E402
import drive  # noqa: E402

CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
PROFILE = os.environ.get("MW_BENCH_CONTENT_PROFILE", os.path.join(HERE, ".chrome-content"))
CONTENT_CDP = 9361
CONTENT_URL = "http://127.0.0.1:18470/bench.html"
SINK = "http://127.0.0.1:18471"

user32 = ctypes.windll.user32
try:
    user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(-4))  # per-monitor v2: physical px
except Exception:
    user32.SetProcessDPIAware()


# ── host screens and the content window ──────────────────────────────────────
class MONITORINFOEX(ctypes.Structure):
    _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT),
                ("dwFlags", wt.DWORD), ("szDevice", wt.WCHAR * 32)]


def monitors():
    out = []
    proto = ctypes.WINFUNCTYPE(ctypes.c_int, wt.HMONITOR, wt.HDC, ctypes.POINTER(wt.RECT), wt.LPARAM)

    def cb(h, dc, r, lp):
        mi = MONITORINFOEX()
        mi.cbSize = ctypes.sizeof(MONITORINFOEX)
        user32.GetMonitorInfoW(h, ctypes.byref(mi))
        rc = mi.rcMonitor
        out.append({"dev": mi.szDevice, "x": rc.left, "y": rc.top, "w": rc.right - rc.left,
                    "h": rc.bottom - rc.top, "primary": bool(mi.dwFlags & 1)})
        return 1
    user32.EnumDisplayMonitors(None, None, proto(cb), 0)
    return out


def wait_new_monitor(before, w, h, timeout=30):
    names = {m["dev"] for m in before}
    end = time.time() + timeout
    while time.time() < end:
        for m in monitors():
            if m["dev"] not in names and m["w"] == w and m["h"] == h:
                return m
        time.sleep(0.5)
    raise drive.PassFailed("no new %dx%d screen after %ds (have %s)" % (w, h, timeout, monitors()))


def windows_of(pids):
    found = []
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

    def cb(hwnd, lp):
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in pids and user32.IsWindowVisible(hwnd):
            buf = ctypes.create_unicode_buffer(64)
            user32.GetClassNameW(hwnd, buf, 64)
            if buf.value == "Chrome_WidgetWin_1":
                found.append(hwnd)
        return True
    user32.EnumWindows(proto(cb), 0)
    return found


SHELL_CLASSES = {"Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd"}
user32.SetWindowPos.argtypes = [wt.HWND, wt.HWND, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_int, ctypes.c_uint]
user32.GetWindowLongW.argtypes = [wt.HWND, ctypes.c_int]
HWND_TOPMOST = wt.HWND(-1)
dwm = ctypes.windll.dwmapi
k32 = ctypes.windll.kernel32


def exe_of(pid):
    h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
    if not h:
        return "?"
    try:
        buf = ctypes.create_unicode_buffer(520)
        n = wt.DWORD(520)
        k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
        return os.path.basename(buf.value)
    finally:
        k32.CloseHandle(h)


def foreign_windows(mon, own_pids):
    """Visible, uncloaked top-level windows on the virtual screen that are not the content
    Chrome nor the desktop/taskbar. Only class and program are kept: never a title."""
    found = []
    own_seen = []  # EnumWindows goes top to bottom: a window listed before ours is above it
    proto = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)
    L, T, R, B = mon["x"], mon["y"], mon["x"] + mon["w"], mon["y"] + mon["h"]

    def cb(hwnd, lp):
        if not user32.IsWindowVisible(hwnd):
            return True
        cloaked = wt.DWORD()
        dwm.DwmGetWindowAttribute(hwnd, 14, ctypes.byref(cloaked), 4)
        if cloaked.value:
            return True
        r = wt.RECT()
        # the visible frame (DWMWA_EXTENDED_FRAME_BOUNDS): a maximized window's invisible
        # border runs ~9 px onto the next screen and is not on the picture
        if dwm.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
            user32.GetWindowRect(hwnd, ctypes.byref(r))
        if min(r.right, R) - max(r.left, L) <= 0 or min(r.bottom, B) - max(r.top, T) <= 0:
            return True
        pid = wt.DWORD()
        user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
        if pid.value in own_pids:
            own_seen.append(hwnd)
            return True
        buf = ctypes.create_unicode_buffer(128)
        user32.GetClassNameW(hwnd, buf, 128)
        if buf.value in SHELL_CLASSES:
            return True
        ex = user32.GetWindowLongW(hwnd, -20)
        found.append({"class": buf.value, "pid": pid.value, "exe": exe_of(pid.value),
                      "transparent": bool(ex & 0x20), "topmost": bool(ex & 0x8), "layered": bool(ex & 0x80000), "above_page": not own_seen,
                      "rect": [r.left, r.top, r.right, r.bottom]})
        return True
    user32.EnumWindows(proto(cb), 0)
    if not own_seen:  # no page on the screen yet: nothing to be above
        for w in found:
            w["above_page"] = False
    return found


class Foreign(Exception):
    pass


# windows accepted below the TOPMOST page, only with the orchestrator's OK (--allow-nvidia-overlay)
ALLOWED = set()
NVIDIA_OVERLAY = ("CEF-OSC-WIDGET", "NVIDIA Overlay.exe")
ALLOW_INK = []  # [True] with --allow-handwriting-canvas
ALLOW_COVERED = []  # [True] with --allow-covered: the desk's windows under the TOPMOST page (07/11)


def accepted(w):
    if ALLOW_COVERED and not w["topmost"] and not w["above_page"]:
        return True
    if (w["class"], w["exe"]) in ALLOWED and not w["topmost"] and not w["above_page"]:
        return True
    # Windows' pen canvas: topmost, but click-through and see-through; draws only pen ink
    return bool(ALLOW_INK) and w["exe"] == "TabTip.exe" and w["class"].startswith("ShellHandwritingCanvas")         and w["transparent"] and w["layered"]


class Watch:
    """Polls the virtual screen for other windows during the whole stream."""

    def __init__(self, mon):
        import threading
        self.mon = mon
        self.own = set()
        self.hits = []
        self.notes = []
        self._stop = threading.Event()
        self._t = threading.Thread(target=self._run, daemon=True)
        self._t.start()

    def _run(self):
        while not self._stop.is_set():
            try:
                for w in foreign_windows(self.mon, self.own):
                    key = (w["class"], w["exe"])
                    if accepted(w):
                        if key not in [(n["class"], n["exe"]) for n in self.notes]:
                            self.notes.append(dict(w, at=time.strftime("%H:%M:%S")))
                    elif key not in [(h["class"], h["exe"]) for h in self.hits]:
                        self.hits.append(dict(w, at=time.strftime("%H:%M:%S")))
            except Exception as e:  # noqa: BLE001
                self.notes.append({"error": str(e)})
            self._stop.wait(0.2)

    def check(self):
        if self.hits:
            raise Foreign("other window(s) on the virtual screen: %s" % json.dumps(self.hits))

    def nap(self, secs, extra=None):
        end = time.time() + secs
        while True:
            self.check()
            if extra:
                extra()
            left = end - time.time()
            if left <= 0:
                return
            time.sleep(min(0.25, left))

    def stop(self):
        self._stop.set()
        self._t.join(2)


def chrome_pids(profile):
    r = subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | Where-Object { $_.CommandLine -like '*%s*' } | ForEach-Object { $_.ProcessId }" % profile],
                       capture_output=True, text=True, creationflags=0x08000000)  # CREATE_NO_WINDOW
    return {int(x) for x in r.stdout.split() if x.strip().isdigit()}


class Content:
    """The lab content Chrome on the virtual display (pinned, muted, own profile)."""

    def __init__(self, luid):
        self.luid = luid
        self.p = None
        self.c = None

    def topmost(self):
        """TOPMOST without activation; returns the windows and whether each one has it."""
        out = []
        for hwnd in windows_of(chrome_pids(PROFILE)):
            user32.SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, 0x0013)  # NOMOVE|NOSIZE|NOACTIVATE
            out.append(bool(user32.GetWindowLongW(hwnd, -20) & 0x8))
        if not out or not all(out):
            raise drive.PassFailed("content window not TOPMOST: %s" % out)
        return out

    def start(self, mon, url=None, watch=None):
        url = url or CONTENT_URL
        os.makedirs(PROFILE, exist_ok=True)
        args = [CHROME, "--user-data-dir=" + PROFILE, "--use-adapter-luid=" + self.luid, "--mute-audio",
                "--no-first-run", "--no-default-browser-check", "--disable-features=Translate,CalculateNativeWinOcclusion",
                "--disable-backgrounding-occluded-windows", "--disable-renderer-backgrounding",
                "--disable-background-timer-throttling", "--remote-debugging-port=%d" % CONTENT_CDP,
                "--window-position=%d,%d" % (mon["x"] + 40, mon["y"] + 40), "--window-size=800,600",
                "--kiosk", url]
        self.p = subprocess.Popen(args)
        if watch is not None:
            watch.own = {self.p.pid}  # the browser process owns its windows
        end = time.time() + 20
        while time.time() < end:
            try:
                urllib.request.urlopen("http://127.0.0.1:%d/json/version" % CONTENT_CDP, timeout=2).read()
                break
            except Exception:
                time.sleep(0.4)
        time.sleep(1.5)
        if watch is not None:
            watch.own = chrome_pids(PROFILE) | {self.p.pid}
        # place it on the virtual display (physical px), TOPMOST, then fullscreen there
        for hwnd in windows_of(chrome_pids(PROFILE)):
            user32.ShowWindow(hwnd, 4)  # SW_SHOWNOACTIVATE
            user32.SetWindowPos(hwnd, HWND_TOPMOST, mon["x"] + 40, mon["y"] + 40, 800, 600, 0x0010)  # NOACTIVATE
        self.c = cdp.Cdp(CONTENT_CDP)
        win = self.c.call("Browser.getWindowForTarget")
        wid = (win.get("result") or win)["windowId"]
        self.c.call("Browser.setWindowBounds", windowId=wid, bounds={"windowState": "normal"})
        self.c.call("Browser.setWindowBounds", windowId=wid, bounds={"windowState": "fullscreen"})
        self.c.call("Emulation.setFocusEmulationEnabled", enabled=True)
        time.sleep(1.0)
        self.c.call("Page.reload")
        time.sleep(2.0)
        if watch is not None:
            watch.own = chrome_pids(PROFILE) | {self.p.pid}
        self.topmost()

    def ev(self, js, await_promise=False):
        r = self.c.call("Runtime.evaluate", expression=js, returnByValue=True, awaitPromise=await_promise)
        if r.get("exceptionDetails"):
            raise drive.PassFailed("content page: %s" % json.dumps(r["exceptionDetails"])[:400])
        return r.get("result", {}).get("value")

    def stop(self):
        try:
            if self.c:
                self.c.call("Browser.close")
        except Exception:
            pass
        time.sleep(1.0)
        for pid in chrome_pids(PROFILE):
            subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], capture_output=True, creationflags=0x08000000)
        self.c = None


TAP = open(os.path.join(HERE, "tap.js"), encoding="utf-8").read()  # loaded disarmed
HDR_SHIM = """(() => { const mm = window.matchMedia.bind(window);
  window.matchMedia = (q) => /dynamic-range:\\s*high/.test(q)
    ? { matches: true, media: q, onchange: null, addEventListener() {}, removeEventListener() {},
        addListener() {}, removeListener() {}, dispatchEvent() { return false; } }
    : mm(q); })();"""
HDR_SHIM_ADDED = []
GRAB = [100 + 200 * k for k in range(15)]


def ensure_tap(cl):
    """The injected script lives as long as the CDP session: after a reconnect it is gone.
    It is loaded DISARMED: nothing is kept before armOnKey + the keyframe it waits for."""
    if cl.eval("JSON.stringify(!!(window.__mwTap && window.__mwTap.armOnKey))") == "true":
        return
    cl.call("Page.enable")
    cl.call("Page.addScriptToEvaluateOnNewDocument", source=TAP)
    cl.call("Page.reload")
    time.sleep(4)
    if cl.eval("JSON.stringify(!!(window.__mwTap && window.__mwTap.armOnKey && !window.__mwTap.armed))") != "true":
        raise drive.PassFailed("tap.js (disarmed) is not in the client page")


def tap_state(cl):
    return json.loads(cl.eval("JSON.stringify(window.__mwTap ? window.__mwTap.stats() : null)") or "null")


def start_recording(cl, watch, timeout=8.0):
    """Arm on the next keyframe, ask for one, wait until its picture is read: it must
    carry the bench page's barcode."""
    print("arm:", cl.eval("window.__mwTap.armOnKey(%s)" % json.dumps(GRAB)), flush=True)
    end = time.time() + timeout
    asked = 0.0
    while time.time() < end:
        watch.check()
        if time.time() - asked > 1.2:
            r = cl.eval("window.__mwTap.idr()")
            if r != "asked":
                raise drive.PassFailed("IDR request: %s" % r)
            asked = time.time()
        st = tap_state(cl)
        if st.get("bad"):
            raise BadPicture(st["bad"])
        if st["armed"] and any(i == 0 for i, _ in st["bars"]):
            v = [b for i, b in st["bars"] if i == 0][0]
            print("recording from the keyframe, barcode %s" % v, flush=True)
            return v
        time.sleep(0.1)
    raise drive.PassFailed("no keyframe within %.0fs of the request (%s)" % (timeout, tap_state(cl)))


class BadPicture(Exception):
    pass


def wipe_run(run, why):
    """Keep only what carries no picture: run.json/meta.json/notes; everything else goes."""
    if not os.path.isdir(run):
        return
    for name in os.listdir(run):
        if name.endswith((".bin", ".raw", ".hevc", ".h264", ".obu", ".nv12", ".yuv", ".png")):
            os.remove(os.path.join(run, name))
    json.dump({"why": why, "at": time.strftime("%H:%M:%S")}, open(os.path.join(run, "wiped.json"), "w", encoding="utf-8"))


def check_bars(run):
    """Offline: every grabbed decoder frame must carry the barcode."""
    sys.path.insert(0, HERE)
    import common as cm
    return [(idx, cm.read_bar(y)) for idx, y in grabbed_lumas(run)]


def check_white(run):
    """Offline, SDR only: why the run is refused if the screen was dimmed (the lowest barcode
    white of the grabbed frames below common.DIM_WHITE), else None. A prompt's dimmed backdrop is
    seen neither by the watch nor by the barcode reader (12/11, runs/HS)."""
    sys.path.insert(0, HERE)
    import common as cm
    low = cm.dimmed([cm.bar_white(y) for _, y in grabbed_lumas(run)])
    if low is None:
        return None
    return "screen dimmed: barcode white at %.0f, below %d (a prompt's backdrop?)" % (low, cm.DIM_WHITE)


def grabbed_lumas(run):
    """(idx, luma plane as HxW uint8) of each decoder frame tap.js grabbed."""
    sys.path.insert(0, HERE)
    import numpy as np
    meta = json.load(open(os.path.join(run, "meta.json"), encoding="utf-8"))
    out = []
    for fr in meta["frames"]:
        b = np.fromfile(os.path.join(run, "frame-%06d.raw" % fr["idx"]), dtype=np.uint8)
        w, h = fr["vis"]["w"], fr["vis"]["h"]
        off, stride = fr["layout"][0]["offset"], fr["layout"][0]["stride"]
        y = b[off:off + stride * h].reshape(h, stride)
        fm = fr.get("format") or ""
        if fm in ("RGBA", "RGBX", "BGRA", "BGRX"):  # a 4:4:4 stream, as Chrome hands it over
            y = y[:, :4 * w].reshape(h, w, 4)[:, :, 1].copy()
        elif "P010" in fm or "P10" in fm or "P12" in fm:  # two bytes a sample, little-endian
            y16 = y[:, :2 * w].copy().view("<u2")
            y = (y16 >> (8 if fm == "P010" else 2)).astype(np.uint8)
        else:
            y = y[:, :w]
        out.append((fr["idx"], y))
    return out


PULL_SLICE = 4 << 20


def pull(cl, run, tag):
    """Read what tap.js recorded over CDP, in slices: the page sends nothing on the network
    (Chrome's Local Network Access refuses a LAN page's fetch to loopback)."""
    meta = cl.eval("""JSON.stringify((() => { const T = window.__mwTap; return {
        tag: %s, ua: navigator.userAgent, configs: T.configs,
        chunks: T.chunks.map(c => ({ type: c.type, ts: c.ts, n: c.b.byteLength, at: c.at })),
        frames: T.frames.map(f => ({ idx: f.idx, ts: f.ts, format: f.format, codedW: f.codedW, codedH: f.codedH,
                                     vis: f.vis, layout: f.layout, colorSpace: f.colorSpace, n: f.b.byteLength })) }; })())""" % json.dumps(tag))
    open(os.path.join(run, "meta.json"), "w", encoding="utf-8").write(meta)
    total = int(cl.eval("""(() => { const T = window.__mwTap;
        const u = new Uint8Array(T.chunks.reduce((s, c) => s + c.b.byteLength, 0)); let o = 0;
        for (const c of T.chunks) { u.set(c.b, o); o += c.b.byteLength; }
        window.__mwPull = { bitstream: u }; T.frames.forEach(f => { window.__mwPull['f' + f.idx] = f.b; });
        return String(u.byteLength); })()"""))

    def read(key, n, path):
        with open(path, "wb") as f:
            for o in range(0, n, PULL_SLICE):
                b64 = cl.eval("""(() => { const b = window.__mwPull[%s].subarray(%d, %d); let s = '';
                    for (let i = 0; i < b.length; i += 32768) s += String.fromCharCode.apply(null, b.subarray(i, i + 32768));
                    return btoa(s); })()""" % (json.dumps(key), o, min(n, o + PULL_SLICE)))
                f.write(base64.b64decode(b64))
        if os.path.getsize(path) != n:
            raise drive.PassFailed("pulled %d of %d bytes of %s" % (os.path.getsize(path), n, key))

    read("bitstream", total, os.path.join(run, "bitstream.bin"))
    for fr in json.loads(meta)["frames"]:
        read("f%d" % fr["idx"], fr["n"], os.path.join(run, "frame-%06d.raw" % fr["idx"]))
    cl.eval("(() => { window.__mwPull = null; return 'freed'; })()")
    return {"bytes": total, "frames": len(json.loads(meta)["frames"])}


# ── scene plans ──────────────────────────────────────────────────────────────
# hdrcal.py's bands, in light (1.0 = SDR white): below, at and above the SDR white, up to where a
# virtual display's headroom may stop.
CAL_BANDS = [0.5, 1.0, 1.5, 2.0, 2.5, 3.0, 4.0, 5.0]


def plan(name, geo, fps, h, peak=3.0, gain=1.0):
    step = max(1, round(h / 120 * 60 / fps))  # same speed in px/s at any frame rate
    src = lambda f: "src/%s/%s.png" % (geo, f)  # noqa: E731
    still = 3.0
    if name == "phase1":
        scenes = [{"id": 1, "mode": "still", "src": src("junkshop")},
                  {"id": 2, "mode": "scroll", "src": src("google"), "step": step, "pause": 0},
                  {"id": 3, "mode": "scroll", "src": src("google"), "step": step, "pause": fps // 2}]
        seq = [(1, still), (2, 5.0), (3, 6.0)]
    elif name == "main":
        scenes = [{"id": 1, "mode": "still", "src": src("junkshop")},
                  {"id": 4, "mode": "still", "src": src("gears")},
                  {"id": 2, "mode": "scroll", "src": src("google"), "step": step, "pause": 0},
                  {"id": 3, "mode": "scroll", "src": src("google"), "step": step, "pause": fps // 2},
                  {"id": 5, "mode": "scroll", "src": src("web"), "step": step, "pause": 0}]
        seq = [(1, still), (4, still), (2, 5.0), (3, 6.0), (5, 5.0)]
        if h >= 2160:
            scenes.append({"id": 6, "mode": "still", "src": src("cp4k")})
            seq.append((6, still))
    elif name == "hdrpeak":
        # HDR with highlights over the SDR white (bench.html's WebGPU canvas, "hdr" key): the
        # renders' bright parts raised to `peak` x SDR white. Scene 7 first: bands of known
        # light, for hdrcal.py to check nothing is clipped. Needs --fmt hdr.
        # gain: Chrome's extended canvas puts its 1.0 below the desktop's SDR white (86 nits for
        # 240 on the 11/11 slot); everything drawn is multiplied by it (--hdr-gain), so that 1.0
        # lands on the desktop's white, as in a game. hdrcal.py checks it run by run.
        hp = {"k": peak, "t": 0.6, "g": gain}
        scenes = [{"id": 7, "mode": "calib", "src": src("junkshop"), "bands": CAL_BANDS, "g": gain},
                  {"id": 1, "mode": "still", "src": src("junkshop"), "hdr": hp},
                  {"id": 4, "mode": "still", "src": src("gears"), "hdr": hp},
                  {"id": 2, "mode": "scroll", "src": src("jsgears"), "step": step, "pause": 0, "hdr": hp},
                  {"id": 3, "mode": "scroll", "src": src("jsgears"), "step": step, "pause": fps // 2,
                   "hdr": hp}]
        seq = [(7, 2.0), (1, still), (4, still), (2, 5.0), (3, 6.0)]
    elif name == "rendersdr":
        # hdrpeak's twin in SDR (same renders, same ids, drawn by the 2D canvas): the baseline that
        # the HDR factor is measured against, content for content.
        scenes = [{"id": 1, "mode": "still", "src": src("junkshop")},
                  {"id": 4, "mode": "still", "src": src("gears")},
                  {"id": 2, "mode": "scroll", "src": src("jsgears"), "step": step, "pause": 0},
                  {"id": 3, "mode": "scroll", "src": src("jsgears"), "step": step, "pause": fps // 2}]
        seq = [(1, still), (4, still), (2, 5.0), (3, 6.0)]
    else:
        raise SystemExit("unknown plan " + name)
    return scenes, seq


# ── one run ──────────────────────────────────────────────────────────────────
def settings_for(w, h, fps, kbps, codec, fmt="420"):
    return {"stream_resolution": "custom", "stream_custom_width": w, "stream_custom_height": h,
            "stream_height": h, "stream_fps": fps, "stream_bitrate": kbps, "stream_bitrate_auto": False,
            "video_codec": codec, "video_enhancement": "off", "hdr_enabled": fmt == "hdr",
            "chroma_444_enabled": fmt == "444", "video_worker": "off", "transport_mode": "webrtc-dc-udp"}


DEV_LOG = os.path.join(HERE, "dev.log")


def host_log_since(mark):
    """The --dev's log lines written after byte offset `mark`."""
    try:
        with open(DEV_LOG, "rb") as f:
            f.seek(mark)
            return f.read().decode("utf-8", "replace")
    except OSError:
        return ""


def host_format(log, fmt):
    """What the host says it streams: (ok, sdr_nits, why). A 4:4:4 or HDR run that the host
    turned back into SDR 4:2:0 is refused here, before anything is recorded."""
    import re
    if "NVENC ready" not in log:
        return False, None, "no NVENC ready line in the host log"
    if fmt == "444":
        if "4:4:4 requested but" in log or "4:4:4 and HDR cannot" in log:
            return False, None, "the host did not grant 4:4:4"
        return True, None, ""
    if fmt == "hdr":
        if "HDR was negotiated but the display handed over 8-bit frames" in log:
            return False, None, "the display is SDR: the host streams SDR"
        if "Main10 (BT.2020 PQ)" not in log:
            return False, None, "NVENC is not on Main10 PQ"
        m = re.findall(r"the desktop's SDR white is (\d+) nits", log)
        if not m:
            return False, None, "no SDR white in the host log"
        return True, float(m[-1]), ""
    return True, None, ""


def one(cl, content, a, w, h, kbps, out):
    tag = "%dx%dp%d-%s%s-%05d" % (w, h, a.fps, a.codec, "" if a.fmt == "420" else "-" + a.fmt, kbps)
    run = os.path.join(out, tag)
    if os.path.exists(os.path.join(run, "run.json")):
        print(tag, "done already")
        return
    if a.scenes == "hdrpeak" and a.fmt != "hdr":
        raise SystemExit("--scenes hdrpeak needs --fmt hdr")
    scenes, seq = plan(a.scenes, "%dx%d" % (w, h), a.fps, h, a.hdr_peak, a.hdr_gain)
    if a.fmt == "hdr" and not HDR_SHIM_ADDED:
        # The lab client is headless: no screen, so no HDR screen, and the page asks the native
        # host for HDR only when its screen can show it. Bench only, this Chrome only: it is
        # told its screen is HDR. Decoding stays the real decoder's.
        cl.call("Page.enable")
        cl.call("Page.addScriptToEvaluateOnNewDocument", source=HDR_SHIM)
        HDR_SHIM_ADDED.append(True)
    cl.navigate(a.host_url)
    cl.apply_settings(settings_for(w, h, a.fps, kbps, a.codec, a.fmt))
    log_mark = os.path.getsize(DEV_LOG) if os.path.exists(DEV_LOG) else 0
    ensure_tap(cl)
    if not cl.wait_library(tries=15):
        raise drive.PassFailed("no host card on the client page (PIN page?)")
    card, app = cl.pick_tile("vdisplay")
    before = monitors()
    t_launch = time.time()
    cl.launch(card, app)
    cl.wait_picture(timeout=45)
    mon = wait_new_monitor(before, w, h)
    sdr_nits = None
    if a.fmt != "420":
        time.sleep(2.0)
        ok, sdr_nits, why = host_format(host_log_since(log_mark), a.fmt)
        if not ok:
            cl.stop()
            raise drive.PassFailed("%s: %s" % (a.fmt, why))
    watch = Watch(mon)
    tapst = None

    def tap_ok():
        s = tap_state(cl)
        if s and s.get("bad"):
            raise BadPicture(s["bad"])

    try:
        try:
            content.start(mon, watch=watch)
            watch.check()
            st = json.loads(content.ev("JSON.stringify(mw.state())"))
            if st["W"] != w or st["H"] != h:
                raise drive.PassFailed("content page is %sx%s (dpr %s) on %s, not %dx%d" % (st["W"], st["H"], st["dpr"], mon, w, h))
            content.ev("mw.rate(%d)" % a.fps)
            content.ev("mw.load(%s).then(r => JSON.stringify(r))" % json.dumps(
                [{k: v for k, v in s.items()} for s in scenes]), await_promise=True)
            content.ev("mw.grey()")
            watch.nap(1.5)
            content.topmost()
            first_bar = start_recording(cl, watch)
            watch.nap(1.0)
            timeline = []
            for sid, dur in seq:
                timeline.append((sid, time.time()))
                content.ev("mw.show(%d)" % sid)
                watch.nap(dur, tap_ok)
                content.ev("mw.grey()")
                watch.nap(1.0, tap_ok)
            end_state = json.loads(content.ev("JSON.stringify(mw.state())"))
            # stop recording BEFORE the page goes: the bare virtual screen is never kept
            tapst = json.loads(cl.eval("JSON.stringify(window.__mwTap.disarm())") or "null")
            watch.check()
        except (Foreign, BadPicture) as e:
            cl.eval("window.__mwTap && window.__mwTap.wipe(%s)" % json.dumps(type(e).__name__))
            raise
        finally:
            if tapst is None:
                try:
                    cl.eval("window.__mwTap && (window.__mwTap.disarm(), window.__mwTap.wipe('run aborted'))")
                except Exception:  # noqa: BLE001
                    pass
            content.stop()
        stats = cl.stats()
    finally:
        cl.stop()
        watch.stop()
    if watch.hits:
        raise Foreign("other window(s) on the virtual screen: %s" % json.dumps(watch.hits))
    if tapst.get("bad") or any(b is None for _, b in tapst.get("bars", [])):
        cl.eval("window.__mwTap.wipe('bad bar')")
        raise BadPicture("barcode missing: %s %s" % (tapst.get("bad"), tapst.get("bars")))
    if not tapst.get("chunks"):
        raise drive.PassFailed("tap recorded nothing: %r" % (tapst,))
    os.makedirs(run, exist_ok=True)
    fl = pull(cl, run, tag)
    cl.eval("window.__mwTap.wipe('pulled')")
    bars = check_bars(run)
    if not bars or any(v is None for _, v in bars):
        wipe_run(run, "offline barcode check failed: %s" % bars)
        raise BadPicture("offline barcode check failed: %s" % bars)
    if a.fmt != "hdr":  # HDR: hdrcal.py's bands say what light was carried
        why = check_white(run)
        if why:
            wipe_run(run, why)
            raise BadPicture(why)
    spec = {"W": w, "H": h, "fps": a.fps, "kbps": kbps, "codec": a.codec, "scenes": scenes, "seq": seq,
            "monitor": mon, "content_state": end_state, "tap": tapst, "flush": str(fl)[:300],
            "overlay": stats, "launched": t_launch, "timeline": timeline, "arm": a.arm,
            "first_bar": first_bar, "bars": bars, "watch_notes": watch.notes, "protections": 3,
            "fmt": a.fmt, "sdr_nits": sdr_nits}
    if not os.path.exists(os.path.join(run, "bitstream.bin")):
        raise drive.PassFailed("collector got no bitstream for %s (flush said %s)" % (tag, str(fl)[:200]))
    json.dump(spec, open(os.path.join(run, "run.json"), "w", encoding="utf-8"), indent=1)
    print(tag, "ok", tapst.get("chunks"), "chunks", tapst.get("bytes"), "bytes, bars", [v for _, v in bars],
          "notes", [(n.get("class"), n.get("exe")) for n in watch.notes], flush=True)


def pair_dev(cl, host_url, api="http://127.0.0.1:8080"):
    """Pair this client with the local --dev when its PIN page is up: one PIN generated on the
    --dev's loopback API, kept in memory, typed once (failures lock the address out)."""
    cl.navigate(host_url)
    time.sleep(4)
    if "true" not in str(cl.eval("JSON.stringify(!!document.getElementById('login-pin-input'))")):
        return "no PIN page"
    tok = json.loads(urllib.request.urlopen(api + "/api/admin/token", timeout=5).read())["token"]
    req = urllib.request.Request(api + "/api/admin/pin/generate", data=b"{}", method="POST",
                                 headers={"X-MW-Admin-Key": tok, "Content-Type": "application/json"})
    pin = json.loads(urllib.request.urlopen(req, timeout=5).read()).get("pin", "")
    tok = None
    if not (pin.isdigit() and 4 <= len(pin) <= 8):
        raise SystemExit("PIN generation gave no PIN")
    cl.unlock("bench-d1-m", pin)
    pin = None
    return "paired" if cl.wait_library(tries=10) else "pairing failed"


def only_vscode(e):
    """True when a Foreign stop names Code.exe windows and nothing else."""
    w = str(e)
    return isinstance(e, Foreign) and w.count('"exe": ') > 0 and w.count('"exe": ') == w.count('"exe": "Code.exe"')


def note_incident(out, e, kbps, n, tag=None, skip=False):
    """Every stop of the watch or the barcode check goes in incidents.jsonl (class and program
    only); the third on one point writes STOPPED.json and the bench stops."""
    rec = {"kind": type(e).__name__, "why": str(e), "at": time.strftime("%H:%M:%S"), "kbps": kbps, "n": n, "tag": tag}
    with open(os.path.join(out, "incidents.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + chr(10))
    if n >= 3 and skip:
        with open(os.path.join(out, "SKIPPED.jsonl"), "a", encoding="utf-8") as f:
            f.write(json.dumps(rec) + chr(10))
    elif n >= 3:
        json.dump(rec, open(os.path.join(out, "STOPPED.json"), "w", encoding="utf-8"))
    print("INCIDENT %d (%s): %s" % (n, type(e).__name__, e), flush=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--geo", required=True)
    ap.add_argument("--fps", type=int, default=60)
    ap.add_argument("--codec", default="hevc")
    ap.add_argument("--kbps", required=True)
    ap.add_argument("--host-url", required=True)
    ap.add_argument("--client-cdp", type=int, default=9424)
    ap.add_argument("--content-luid", default=os.environ.get("MW_BENCH_CONTENT_LUID", ""))  # the encoding GPU (see README)
    ap.add_argument("--scenes", default="phase1")
    ap.add_argument("--arm", default="A")
    ap.add_argument("--fmt", default="420", choices=["420", "444", "hdr"])
    ap.add_argument("--hdr-peak", type=float, default=3.0,
                    help="hdrpeak plan: the highlights' light, x SDR white (hdrcal.py says what fits)")
    ap.add_argument("--hdr-gain", type=float, default=1.0,
                    help="hdrpeak plan: gain on all the canvas draws (desktop SDR white / Chrome's white)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--allow-nvidia-overlay", action="store_true")
    ap.add_argument("--allow-handwriting-canvas", action="store_true")
    ap.add_argument("--allow-covered", action="store_true",
                    help="accept any non-TOPMOST window below the page (the virtual screen is primary)")
    ap.add_argument("--pair-dev", action="store_true", help="pair with the local --dev first (PIN in memory)")
    ap.add_argument("--deadline", default="", help="HH:MM (morning): no new point starts after it")
    ap.add_argument("--skip-vscode-point", action="store_true",
                    help="3 stops on one point, all for Code.exe windows only: mark the point skipped, go on")
    a = ap.parse_args()
    if a.allow_nvidia_overlay:
        ALLOWED.add(NVIDIA_OVERLAY)
    if a.allow_handwriting_canvas:
        ALLOW_INK.append(True)
    if a.allow_covered:
        ALLOW_COVERED.append(True)
    w, h = map(int, a.geo.split("x"))
    out = os.path.abspath(a.out)
    os.makedirs(out, exist_ok=True)
    if os.path.exists(os.path.join(out, "STOPPED.json")):
        raise SystemExit("STOPPED.json in %s: the orchestrator decides first" % out)
    cl = drive.Driver(a.client_cdp)
    if a.pair_dev:
        print("pairing:", pair_dev(cl, a.host_url), flush=True)
    content = Content(a.content_luid)
    for kbps in [int(x) for x in a.kbps.split(",")]:
        if os.path.exists(os.path.join(os.path.dirname(os.path.abspath(__file__)), "PAUSE")):
            print("PAUSE flag: stopping", flush=True)
            return
        hm = time.strftime("%H:%M")
        if a.deadline and a.deadline <= hm < "12:00":
            print("deadline %s reached: stopping" % a.deadline, flush=True)
            open(os.path.join(out, "DEADLINE"), "w").write(hm)
            return
        fails = incidents = 0
        vsc = True
        while True:
            try:
                one(cl, content, a, w, h, kbps, out)
                break
            except (Foreign, BadPicture) as e:
                # the point is wiped at once; it is redone after a minute, 3 times at most
                run = os.path.join(out, "%dx%dp%d-%s%s-%05d" % (w, h, a.fps, a.codec,
                                                               "" if a.fmt == "420" else "-" + a.fmt, kbps))
                wipe_run(run, str(e))
                content.stop()
                cl.stop()
                incidents += 1
                vsc = vsc and only_vscode(e)
                skip = a.skip_vscode_point and vsc
                note_incident(out, e, kbps, incidents, skip=skip)
                if incidents >= 3 and skip:
                    print("kbps %d skipped: Code.exe on the virtual screen 3 times" % kbps, flush=True)
                    break
                if incidents >= 3:
                    sys.exit(4)
                time.sleep(60)
            except drive.PassFailed as e:
                fails += 1
                print("kbps %d attempt %d failed: %s" % (kbps, fails, e), flush=True)
                content.stop()
                cl.stop()
                if fails >= 2:
                    break
                time.sleep(5)

if __name__ == "__main__":
    main()
