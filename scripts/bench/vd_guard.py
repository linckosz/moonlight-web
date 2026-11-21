"""Stop a --dev's stream the moment another program's window lands on its virtual screen.

Windows lays out the desk's windows again each time a screen is added: the
virtual screen of a stream can get one of them as soon as it exists, before the
bench has put anything on it (14/11, a terminal window filmed ~85 s). This guard
runs before the stream starts. It sees the new screen within one poll, primary
or not, then reads that screen's windows every poll for the whole stream. On the
first window that is neither ours nor the desktop's, it asks the --dev to quit
through its local channel (POST /api/system/quit), and kills it if it is still
there after a grace.

It fails closed: once armed, every way out but "the screen went away and nobody
streams any more" stops the --dev too (a foreign window, no new screen in time,
the watch time running out, an error). The bench starts the stream only after
the "armed" line, and one guard covers one stream launch.

What it may stop is the process it was pointed at, held by a handle from the
start (its pid cannot be reused while the guard lives): it must be the --dev
that listens on --port when armed, and still be that exe when stopped.

It never moves, hides or touches a window. It keeps only a window's class and
program, never its title.

    python scripts/bench/vd_guard.py --port 8443 --pid <the --dev's pid> --own-pid-file <scratchpad>/own.txt

Exit 0: the screen went away and nobody streams (nothing stopped), or the watch
        time ran out (the --dev stopped).
Exit 4: a foreign window was seen: the --dev was stopped.
Exit 2: nothing armed (no --dev on that port, wrong pid, bad own-pid file), or
        armed and no new screen in --appear-timeout: the --dev stopped.
Exit 3: an error once armed: the --dev stopped.
One JSON line per event on standard output.
"""
import argparse
import ctypes
import json
import os
import socket
import struct
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mw_local  # noqa: E402

SHELL_CLASSES = {"Progman", "WorkerW", "Shell_TrayWnd", "Shell_SecondaryTrayWnd"}


def emit(event, **kw):
    now = time.time()
    stamp = time.strftime("%H:%M:%S", time.localtime(now)) + ".%03d" % int(now % 1 * 1000)
    print(json.dumps(dict(kw, event=event, at=stamp)), flush=True)


# ── what Windows shows (Windows only, never called by the tests) ─────────────────
def _win32():
    from ctypes import wintypes as wt
    user32 = ctypes.windll.user32
    dwm = ctypes.windll.dwmapi
    k32 = ctypes.windll.kernel32
    iphlp = ctypes.windll.iphlpapi
    user32.SetProcessDPIAware()  # real pixels, or the bounds of a scaled screen are wrong
    k32.OpenProcess.restype = wt.HANDLE
    k32.OpenProcess.argtypes = [wt.DWORD, wt.BOOL, wt.DWORD]
    k32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
    k32.QueryFullProcessImageNameW.argtypes = [wt.HANDLE, wt.DWORD, wt.LPWSTR, ctypes.POINTER(wt.DWORD)]
    k32.CloseHandle.argtypes = [wt.HANDLE]

    class MONITORINFOEX(ctypes.Structure):
        _fields_ = [("cbSize", wt.DWORD), ("rcMonitor", wt.RECT), ("rcWork", wt.RECT),
                    ("dwFlags", wt.DWORD), ("szDevice", wt.WCHAR * 32)]

    def monitors():
        out = []
        proto = ctypes.WINFUNCTYPE(ctypes.c_int, wt.HMONITOR, wt.HDC, ctypes.POINTER(wt.RECT), wt.LPARAM)

        def cb(h, hdc, rc, lp):
            mi = MONITORINFOEX()
            mi.cbSize = ctypes.sizeof(MONITORINFOEX)
            user32.GetMonitorInfoW(h, ctypes.byref(mi))
            r = mi.rcMonitor
            out.append({"dev": mi.szDevice.rstrip("\0"), "x": r.left, "y": r.top, "w": r.right - r.left,
                        "h": r.bottom - r.top, "primary": bool(mi.dwFlags & 1)})
            return 1
        user32.EnumDisplayMonitors(None, None, proto(cb), 0)
        return out

    def image(h):
        buf = ctypes.create_unicode_buffer(520)
        n = wt.DWORD(520)
        return buf.value if k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n)) else ""

    def exe_of(pid):
        h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return "?"
        try:
            return os.path.basename(image(h)) or "?"
        finally:
            k32.CloseHandle(h)

    def windows():
        """Visible, uncloaked top-level windows: class, pid, program, visible frame."""
        out = []
        proto = ctypes.WINFUNCTYPE(ctypes.c_bool, wt.HWND, wt.LPARAM)

        def cb(hwnd, lp):
            if not user32.IsWindowVisible(hwnd) or user32.IsIconic(hwnd):
                return True
            cloaked = wt.DWORD()
            dwm.DwmGetWindowAttribute(hwnd, 14, ctypes.byref(cloaked), 4)
            if cloaked.value:
                return True
            r = wt.RECT()
            # the visible frame: a maximized window's invisible border runs onto the next screen
            if dwm.DwmGetWindowAttribute(hwnd, 9, ctypes.byref(r), ctypes.sizeof(r)) != 0:
                user32.GetWindowRect(hwnd, ctypes.byref(r))
            pid = wt.DWORD()
            user32.GetWindowThreadProcessId(hwnd, ctypes.byref(pid))
            buf = ctypes.create_unicode_buffer(128)
            user32.GetClassNameW(hwnd, buf, 128)
            out.append({"class": buf.value, "pid": pid.value, "rect": [r.left, r.top, r.right, r.bottom]})
            return True
        user32.EnumWindows(proto(cb), 0)
        for w in out:
            w["exe"] = exe_of(w["pid"])
        return out

    def listeners(port):
        """Pids that listen on TCP @p port, IPv4 and IPv6 (GetExtendedTcpTable, listeners only)."""
        pids = set()
        for family, row, pid_at, port_at in ((socket.AF_INET, 24, 20, 8), (23, 56, 52, 20)):
            size = wt.DWORD(0)
            iphlp.GetExtendedTcpTable(None, ctypes.byref(size), False, family, 3, 0)  # TCP_TABLE_OWNER_PID_LISTENER
            buf = ctypes.create_string_buffer(size.value + 4096)
            size = wt.DWORD(len(buf))
            if iphlp.GetExtendedTcpTable(buf, ctypes.byref(size), False, family, 3, 0) != 0:
                continue
            raw = buf.raw
            n = struct.unpack_from("<I", raw, 0)[0]
            for i in range(n):
                base = 4 + i * row
                p = socket.ntohs(struct.unpack_from("<I", raw, base + port_at)[0] & 0xFFFF)
                if p == port:
                    pids.add(struct.unpack_from("<I", raw, base + pid_at)[0])
        return pids

    class Target:
        """The --dev, held by a handle: its pid stays its own while the guard lives."""

        def __init__(self, pid):
            self.pid = pid
            self.h = k32.OpenProcess(0x1000 | 0x00100000, False, pid)  # query + SYNCHRONIZE
            if not self.h:
                raise OSError("no handle on pid %d" % pid)
            self.exe = image(self.h)

        def alive(self):
            return k32.WaitForSingleObject(self.h, 0) == 0x102  # WAIT_TIMEOUT: not signaled

        def same_exe(self):
            return bool(self.exe) and os.path.normcase(image(self.h)) == os.path.normcase(self.exe)

        def kill_tree(self):
            # the handle keeps this pid from being reused, so /PID names this process only
            if not self.alive() or not self.same_exe():
                return False
            import subprocess
            subprocess.run(["taskkill", "/PID", str(self.pid), "/T", "/F"], stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, creationflags=0x08000000)
            return True

    return monitors, windows, listeners, Target


# ── the rules (pure, tested) ─────────────────────────────────────────────────────
def new_screen(before, now):
    """The first screen of @p now whose device was not in @p before, primary or not, or None.
    The host may make its virtual screen the primary one: that is the case to see most."""
    names = {m["dev"] for m in before}
    for m in now:
        if m["dev"] not in names:
            return m
    return None


def on_screen(rect, mon):
    L, T, R, B = mon["x"], mon["y"], mon["x"] + mon["w"], mon["y"] + mon["h"]
    return min(rect[2], R) - max(rect[0], L) > 0 and min(rect[3], B) - max(rect[1], T) > 0


def foreign(wins, mon, own_pids, allowed):
    """Windows on @p mon that are neither ours, the desktop's, nor an allowed class:program."""
    out = []
    for w in wins:
        if not on_screen(w["rect"], mon) or w["pid"] in own_pids or w["class"] in SHELL_CLASSES:
            continue
        if (w["class"], w["exe"]) in allowed:
            continue
        out.append({"class": w["class"], "exe": w["exe"], "pid": w["pid"]})
    return out


def own_file_ok(path, root=None):
    """The own-pid file can quiet the guard: it must sit in the session scratch area."""
    root = os.path.realpath(root or os.path.join(tempfile.gettempdir(), "claude"))
    real = os.path.realpath(path)
    return os.path.normcase(real).startswith(os.path.normcase(root) + os.sep)


class Guard:
    """One stream launch: wait for the screen, watch it, stop the --dev on anything wrong."""

    def __init__(self, monitors, windows, stop, streaming=lambda: True, poll=0.05,
                 clock=time.monotonic, sleep=time.sleep):
        self.monitors, self.windows, self.stop, self.streaming = monitors, windows, stop, streaming
        self.poll, self.clock, self.sleep = poll, clock, sleep

    def _halt(self, why, code, since=None, **kw):
        t = self.clock()
        emit(why, **kw)
        try:
            how = self.stop()
        except Exception as e:  # noqa: BLE001 — a stop that fails is said, never hidden
            how = "stop failed: %s" % type(e).__name__
        extra = {"ms_after": int((self.clock() - (since if since is not None else t)) * 1000)}
        emit("stopped", how=how, **extra)
        return code

    def run(self, own_pids, allowed, secs, appear_timeout, own_file=None):
        """@p own_file: more pids of ours, one per line, read again at each poll (a window the
        bench opens once the screen is there). A missing or unreadable file adds none."""
        def own():
            if not own_file:
                return own_pids
            try:
                with open(own_file, encoding="utf-8") as f:
                    return own_pids | {int(x) for x in f.read().split() if x.isdigit()}
            except OSError:
                return own_pids
        try:
            before = self.monitors()
        except Exception as e:  # noqa: BLE001 — nothing armed yet: the bench must not start
            emit("error", where="arm", error=type(e).__name__)
            return 2
        emit("armed", screens=[m["dev"] for m in before])
        try:
            t0 = self.clock()
            mon = None
            while mon is None:
                mon = new_screen(before, self.monitors())
                if mon is None:
                    if self.clock() - t0 > appear_timeout:
                        return self._halt("no_screen", 2, after_s=round(self.clock() - t0, 1))
                    self.sleep(self.poll)
            seen = self.clock()
            emit("screen", dev=mon["dev"], primary=mon["primary"], rect=[mon["x"], mon["y"], mon["w"], mon["h"]])
            while self.clock() - seen < secs:
                now = self.monitors()
                cur = next((m for m in now if m["dev"] == mon["dev"]), None)
                if cur is None:
                    if self.streaming():  # a stream that outlives its screen is watched by nobody
                        return self._halt("screen_gone_still_streaming", 3, dev=mon["dev"])
                    emit("screen_gone", dev=mon["dev"])
                    return 0
                mon = cur  # it may move or turn primary while it lives
                hits = foreign(self.windows(), mon, own(), allowed)
                if hits:
                    hit = self.clock()
                    return self._halt("foreign", 4, since=hit, ms_after_screen=int((hit - seen) * 1000),
                                      windows=hits)
                self.sleep(self.poll)
            return self._halt("time_up", 0)
        except Exception as e:  # noqa: BLE001 — fail closed
            return self._halt("error", 3, error=type(e).__name__)


def stopper(exe, dev, target, grace=3.0):
    """Ask the --dev to quit; kill it if it is still there after @p grace seconds."""
    def stop():
        def ask():
            try:
                mw_local.local_request("POST /api/system/quit", exe=exe, dev=dev, body={}, timeout=grace)
            except mw_local.LocalRequestError:
                pass
        if not target.alive():
            return "gone"
        threading.Thread(target=ask, daemon=True).start()
        end = time.monotonic() + grace
        while time.monotonic() < end:
            if not target.alive():
                return "quit"
            time.sleep(0.05)
        return "killed" if target.kill_tree() else "not killed: no longer the --dev"
    return stop


def streaming_of(exe, dev):
    """True unless the --dev says it has no viewer; an unreadable answer counts as streaming."""
    def streaming():
        try:
            status, body = mw_local.local_json("GET /api/server/stream-activity", exe=exe, dev=dev, timeout=5)
        except mw_local.LocalRequestError:
            return True
        return not (status == 200 and isinstance(body, dict) and body.get("count") == 0)
    return streaming


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8443, help="the --dev's HTTPS port")
    ap.add_argument("--pid", type=int, required=True, help="the --dev's process id")
    ap.add_argument("--own-pid", type=int, action="append", default=[], help="a process whose windows are ours")
    ap.add_argument("--own-pid-file", help="more pids of ours, one per line, read at each poll (in the scratchpad)")
    ap.add_argument("--allow", action="append", default=[], help="class:program accepted on the screen")
    ap.add_argument("--secs", type=float, default=900, help="how long to watch once the screen is there")
    ap.add_argument("--appear-timeout", type=float, default=120)
    ap.add_argument("--poll", type=float, default=0.05)
    a = ap.parse_args(argv)
    if a.own_pid_file and not own_file_ok(a.own_pid_file):
        emit("own_file_refused")  # outside the scratch area: anyone could quiet the guard
        return 2
    found = mw_local.instance(a.port)
    if not found:
        emit("no_dev", port=a.port)
        return 2
    exe, dev = found
    if not dev:
        emit("not_dev", port=a.port)  # never the service or a user's instance
        return 2
    monitors, windows, listeners, Target = _win32()
    try:
        target = Target(a.pid)
    except OSError:
        emit("pid_mismatch", pid=a.pid, why="no handle")
        return 2
    if os.path.normcase(target.exe) != os.path.normcase(exe) or a.pid not in listeners(a.port):
        emit("pid_mismatch", pid=a.pid, why="not the --dev that listens on the port")
        return 2
    allowed = {tuple(x.split(":", 1)) for x in a.allow if ":" in x}
    guard = Guard(monitors, windows, stopper(exe, dev, target), streaming=streaming_of(exe, dev), poll=a.poll)
    return guard.run(set(a.own_pid), allowed, a.secs, a.appear_timeout, own_file=a.own_pid_file)


if __name__ == "__main__":
    sys.exit(main())
