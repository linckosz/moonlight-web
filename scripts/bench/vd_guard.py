"""Stop a --dev's stream the moment another program's window lands on its virtual screen.

Windows lays out the desk's windows again each time a screen is added: the
virtual screen of a stream can get one of them as soon as it exists, before the
bench has put anything on it (14/11, a terminal window filmed ~85 s). This guard
runs before the stream starts. It sees the new screen within one poll, then reads
that screen's windows every poll for the whole stream. On the first window that
is neither ours nor the desktop's, it asks the --dev to quit through its local
channel (POST /api/system/quit), and kills it if it is still there after a grace.

It never moves, hides or touches a window. It keeps only a window's class and
program, never its title.

    python scripts/bench/vd_guard.py --port 8443 --pid <the --dev's pid> --own-pid-file own.txt --secs 900

Exit 0: the time ran out, or the virtual screen went away, with nothing else on it.
Exit 4: a foreign window was seen and the --dev was stopped (or asked to).
Exit 2: no --dev on that port, or no new screen in --appear-timeout.
One JSON line per event on standard output.
"""
import argparse
import ctypes
import json
import os
import sys
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
    user32.SetProcessDPIAware()  # real pixels, or the bounds of a scaled screen are wrong

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

    def exe_of(pid, full=False):
        h = k32.OpenProcess(0x1000, False, pid)  # PROCESS_QUERY_LIMITED_INFORMATION
        if not h:
            return "?"
        try:
            buf = ctypes.create_unicode_buffer(520)
            n = wt.DWORD(520)
            k32.QueryFullProcessImageNameW(h, 0, buf, ctypes.byref(n))
            return buf.value if full else os.path.basename(buf.value)
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

    def alive(pid):
        h = k32.OpenProcess(0x1000, False, pid)
        if not h:
            return False
        try:
            code = wt.DWORD()
            k32.GetExitCodeProcess(h, ctypes.byref(code))
            return code.value == 259  # STILL_ACTIVE
        finally:
            k32.CloseHandle(h)

    def kill_tree(pid):
        import subprocess
        subprocess.run(["taskkill", "/PID", str(pid), "/T", "/F"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, creationflags=0x08000000)

    return monitors, windows, alive, kill_tree, exe_of


# ── the rules (pure, tested) ─────────────────────────────────────────────────────
def new_screen(before, now):
    """The first screen of @p now that was not in @p before and is not primary, or None."""
    names = {m["dev"] for m in before}
    for m in now:
        if m["dev"] not in names and not m["primary"]:
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


class Guard:
    """One run: wait for the screen, watch it, stop the --dev on a foreign window."""

    def __init__(self, monitors, windows, stop, poll=0.05, clock=time.monotonic, sleep=time.sleep):
        self.monitors, self.windows, self.stop = monitors, windows, stop
        self.poll, self.clock, self.sleep = poll, clock, sleep

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
        before = self.monitors()
        emit("armed", screens=[m["dev"] for m in before])
        t0 = self.clock()
        mon = None
        while mon is None:
            mon = new_screen(before, self.monitors())
            if mon is None:
                if self.clock() - t0 > appear_timeout:
                    emit("no_screen", after_s=round(self.clock() - t0, 1))
                    return 2
                self.sleep(self.poll)
        seen = self.clock()
        emit("screen", dev=mon["dev"], rect=[mon["x"], mon["y"], mon["w"], mon["h"]])
        while self.clock() - seen < secs:
            now = self.monitors()
            if not any(m["dev"] == mon["dev"] for m in now):
                emit("screen_gone", dev=mon["dev"])
                return 0
            hits = foreign(self.windows(), mon, own(), allowed)
            if hits:
                hit = self.clock()
                emit("foreign", ms_after_screen=int((hit - seen) * 1000), windows=hits)
                how = self.stop()
                emit("stopped", how=how, ms_after_hit=int((self.clock() - hit) * 1000))
                return 4
            self.sleep(self.poll)
        emit("time_up")
        return 0


def stopper(exe, dev, pid, alive, kill_tree, grace=3.0):
    """Ask the --dev to quit; kill it if it is still there after @p grace seconds."""
    def stop():
        asked = {"status": None}

        def ask():
            try:
                asked["status"] = mw_local.local_request("POST /api/system/quit", exe=exe, dev=dev, body={},
                                                         timeout=grace)[0]
            except mw_local.LocalRequestError:
                pass
        threading.Thread(target=ask, daemon=True).start()
        end = time.monotonic() + grace
        while time.monotonic() < end:
            if not alive(pid):
                return "quit"
            time.sleep(0.05)
        kill_tree(pid)
        return "killed"
    return stop


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--port", type=int, default=8443, help="the --dev's HTTPS port")
    ap.add_argument("--pid", type=int, required=True, help="the --dev's process id")
    ap.add_argument("--own-pid", type=int, action="append", default=[], help="a process whose windows are ours")
    ap.add_argument("--own-pid-file", help="more pids of ours, one per line, read at each poll")
    ap.add_argument("--allow", action="append", default=[], help="class:program accepted on the screen")
    ap.add_argument("--secs", type=float, default=900, help="how long to watch once the screen is there")
    ap.add_argument("--appear-timeout", type=float, default=120)
    ap.add_argument("--poll", type=float, default=0.05)
    a = ap.parse_args(argv)
    found = mw_local.instance(a.port)  # looked up once, now: never on the hot path
    if not found:
        emit("no_dev", port=a.port)
        return 2
    exe, dev = found
    if not dev:
        emit("not_dev", port=a.port)  # never the service or a user's instance
        return 2
    monitors, windows, alive, kill_tree, exe_of = _win32()
    if not alive(a.pid) or os.path.normcase(exe_of(a.pid, full=True)) != os.path.normcase(exe):
        emit("pid_mismatch", pid=a.pid)  # the guard only ever stops the --dev it was pointed at
        return 2
    allowed = {tuple(x.split(":", 1)) for x in a.allow if ":" in x}
    guard = Guard(monitors, windows, stopper(exe, dev, a.pid, alive, kill_tree), poll=a.poll)
    return guard.run(set(a.own_pid), allowed, a.secs, a.appear_timeout, own_file=a.own_pid_file)


if __name__ == "__main__":
    sys.exit(main())
