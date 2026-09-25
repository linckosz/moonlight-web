"""A game-like GPU load under a pass: mw-gpu-load on the encoder's own GPU.

A pass that carries `"load": "gpu"` starts the tool before the stream, waits
for it to calibrate (about 45 fps, then the level is frozen), streams as usual,
and records what the tool saw while the stream was measured. Compared with the
same pass without the load, the difference is what a game that takes the whole
GPU costs the stream (Resident Evil 9, 21/09).

The GPU is the one MoonlightWeb names for the target display in
/api/native/status: the same GPU as the encoder, by construction. The tool
itself refuses to run hotter than its limit and never runs longer than 60 s,
so the pass fits inside one run: ~9 s of calibration, then the stream.

On this desk (DualRTX) the tool is built with
`cmake -S backend/native-host/tools/gpu-load -B build-gpuload`; MW_GPU_LOAD
points at another binary. A Windows host of the fleet gets a packaged copy
(deploy_windows), a Mac builds its own from HEAD's sources (deploy_macos).
"""
import json
import os
import subprocess
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DEFAULT_TOOL = os.path.join(REPO, "build-gpuload", "mw-gpu-load.exe")


class Unavailable(Exception):
    """The load cannot run for this pass; the reason says why."""


def _tool():
    tool = os.environ.get("MW_GPU_LOAD", DEFAULT_TOOL)
    if not os.path.exists(tool):
        raise Unavailable("mw-gpu-load is not built (%s)" % tool)
    return tool


def encoder_gpu(target, port=48080):
    """The GPU that encodes `target` on this machine, as MoonlightWeb names it."""
    if target != "display":
        # The virtual display exists only once a stream has turned it on, and
        # the load has to be running before that stream starts.
        raise Unavailable("the load needs a physical display target, not %r" % target)
    with urllib.request.urlopen("http://127.0.0.1:%d/api/native/status" % port, timeout=5) as r:
        status = json.load(r)
    displays = status.get("displays") or []
    if not displays or not displays[0].get("gpu"):
        raise Unavailable("the native host names no GPU for its first display")
    # pick_tile("display") streams the first physical display.
    return displays[0]["gpu"]


def display_on_gpu(name, port=None):
    """The native display that a named GPU encodes, as (index, status entry).

    `name` is a part of the GPU's name ("RTX", "Arc", "AMD"). The index counts
    the physical displays in the order /api/native/status lists them, which is
    the order of their tiles (app ids 1, 2, 3...): pick_tile's `index`.
    """
    port = port or int((os.environ.get("MW_BENCH_LOCAL_PORTS") or "48080").split(",")[0])
    with urllib.request.urlopen("http://127.0.0.1:%d/api/native/status" % port, timeout=5) as r:
        status = json.load(r)
    for i, disp in enumerate(status.get("displays") or []):
        if name.lower() in (disp.get("gpu") or "").lower():
            return i, disp
    raise Unavailable("no display of the native host is driven by a GPU named %r" % name)


def read(path):
    try:
        with open(path, encoding="utf-8") as f:
            return [json.loads(line) for line in f if line.strip()]
    except (OSError, ValueError):
        return []


class Load:
    def __init__(self, gpu, json_path, level=None):
        self.gpu = gpu
        self.path = json_path
        os.makedirs(os.path.dirname(json_path), exist_ok=True)
        # --topmost: a window started from a script on a desk somebody is
        # using does not get the foreground. It rendered at 60 fps behind a
        # browser, and the pass streamed that browser at 2 fps as "under load"
        # (22/09/2026).
        argv = [_tool(), "--gpu", gpu, "--autostart", "--topmost", "--json", json_path]
        if level:
            argv += ["--level", str(level)]
        self.proc = subprocess.Popen(argv)
        self.started = time.time()

    def wait_calibrated(self, timeout=25):
        deadline = time.time() + timeout
        while time.time() < deadline:
            lines = read(self.path)
            for line in lines:
                if line.get("event") == "calibrated":
                    return line
                if line.get("event") == "start" and line.get("autoTune") is False:
                    # A fixed --level never calibrates: warm up a few seconds instead.
                    if len([l for l in lines if l.get("phase") == "locked"]) >= 3:
                        return line
                if line.get("event") in ("refused", "end"):
                    raise Unavailable("mw-gpu-load: %s %s" % (
                        line.get("reason", ""), line.get("detail", "")))
            if self.proc.poll() is not None:
                raise Unavailable("mw-gpu-load exited (%s) before calibrating" % self.proc.returncode)
            time.sleep(0.5)
        raise Unavailable("mw-gpu-load did not calibrate within %d s" % timeout)

    def snapshot(self, seconds):
        """What the tool saw over the last `seconds`: its frame rate, its GPU
        time, the heat, and whether it was still running at all."""
        lines = read(self.path)
        ticks = [l for l in lines if "fps" in l and l.get("phase") == "locked"]
        recent = ticks[-max(1, int(seconds)):]
        end = next((l for l in lines if l.get("event") == "end"), None)
        calibrated = (next((l for l in lines if l.get("event") == "calibrated"), None)
                      or next((l for l in lines if l.get("event") == "start"), {}))

        def mean(key):
            vals = [l[key] for l in recent if l.get(key) is not None]
            return round(sum(vals) / len(vals), 1) if vals else None

        return {
            "gpu": self.gpu,
            "level": round(calibrated.get("level", 0), 1),
            "fps": mean("fps"),
            "gpuMs": mean("gpuMs"),
            "tempC": max((l.get("tempC", -1) for l in recent), default=None),
            "running": end is None and self.proc.poll() is None,
            "end": end,
        }

    def stop(self):
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                self.proc.kill()


# ── a Windows host of the fleet ─────────────────────────────────────────────
#
# The same tool, deployed as a folder (the exe beside the Qt DLLs windeployqt
# put there) and started in the CONSOLE session through an Interactive
# scheduled task: an ssh session lands in session 0, which has no desktop, so
# a window started from there would never render (install.py does the same for
# the app). Its JSON log is read back over ssh.

REMOTE_DIR = r"C:\mw-gpu-load"
REMOTE_JSON = REMOTE_DIR + r"\run.jsonl"
TASK = "MWBenchGpuLoad"


def _fleet():
    import fleet  # the orchestrator's module; imported late so local runs need no fleet
    return fleet


def deploy_windows(mid, package_zip):
    """Push the packaged tool (a zip of the windeployqt'd build folder)."""
    f = _fleet()
    f.push_file(mid, package_zip, "C:/Users/Public/gpuload-win.zip")
    rc, out, err = f.run_script(mid, r"""
$d = '%s'
Get-Process -Name 'mw-gpu-load' -ErrorAction SilentlyContinue | Stop-Process -Force
if (Test-Path $d) { Remove-Item $d -Recurse -Force }
Expand-Archive C:\Users\Public\gpuload-win.zip -DestinationPath $d -Force
Write-Output ('DEPLOYED ' + (Test-Path "$d\mw-gpu-load.exe"))
""" % REMOTE_DIR, timeout=300)
    if "DEPLOYED True" not in (out or ""):
        raise Unavailable("mw-gpu-load could not be deployed on %s" % mid)


def remote_encoder_gpu(mid, target, port=48080):
    if target != "display":
        raise Unavailable("the load needs a physical display target, not %r" % target)
    rc, out, err = _fleet().run_script(
        mid, "(& curl.exe -s -m 5 http://127.0.0.1:%d/api/native/status) -join ''" % port, timeout=60)
    try:
        status = json.loads((out or "").strip().splitlines()[-1])
        return status["displays"][0]["gpu"]
    except (ValueError, KeyError, IndexError):
        raise Unavailable("%s names no GPU for its first display" % mid)


class RemoteLoad(Load):
    def __init__(self, mid, gpu, level=None, allow_no_sensor=False):
        self.mid = mid
        self.gpu = gpu
        args = ['--gpu', '"%s"' % gpu, '--autostart', '--json', REMOTE_JSON]
        if level:
            args += ["--level", str(level)]
        if allow_no_sensor:
            args.append("--allow-no-sensor")
        rc, out, err = _fleet().run_script(mid, r"""
Get-Process -Name 'mw-gpu-load' -ErrorAction SilentlyContinue | Stop-Process -Force
Remove-Item '%s' -ErrorAction SilentlyContinue
$who = (Get-CimInstance Win32_ComputerSystem).UserName
if (-not $who) { $who = "$env:USERDOMAIN\$env:USERNAME" }
$act = New-ScheduledTaskAction -Execute '%s\mw-gpu-load.exe' -Argument '%s'
$pr  = New-ScheduledTaskPrincipal -UserId $who -LogonType Interactive -RunLevel Highest
Register-ScheduledTask -TaskName '%s' -Action $act -Principal $pr -Force | Out-Null
Start-ScheduledTask -TaskName '%s'
Write-Output ('STARTED as ' + $who)
""" % (REMOTE_JSON, REMOTE_DIR, " ".join(args), TASK, TASK), timeout=120)
        if "STARTED" not in (out or ""):
            raise Unavailable("mw-gpu-load could not be started on %s" % mid)
        self.started = time.time()

    def _lines(self):
        rc, out, err = _fleet().run_script(
            self.mid, "Get-Content -Encoding utf8 '%s' -ErrorAction SilentlyContinue" % REMOTE_JSON,
            timeout=60)
        lines = []
        for raw in (out or "").splitlines():
            raw = raw.strip().lstrip("\ufeff")
            if raw.startswith("{"):
                try:
                    lines.append(json.loads(raw))
                except ValueError:
                    pass
        return lines

    def wait_calibrated(self, timeout=40):
        deadline = time.time() + timeout
        while time.time() < deadline:
            lines = self._lines()
            for line in lines:
                if line.get("event") == "calibrated":
                    return line
                if line.get("event") == "start" and line.get("autoTune") is False:
                    # A fixed --level never calibrates: warm up a few seconds instead.
                    if len([l for l in lines if l.get("phase") == "locked"]) >= 3:
                        return line
                if line.get("event") in ("refused", "end"):
                    raise Unavailable("mw-gpu-load on %s: %s %s" % (
                        self.mid, line.get("reason", ""), line.get("detail", "")))
            time.sleep(2)
        raise Unavailable("mw-gpu-load did not calibrate on %s within %d s" % (self.mid, timeout))

    def snapshot(self, seconds):
        lines = self._lines()
        ticks = [l for l in lines if "fps" in l and l.get("phase") == "locked"]
        recent = ticks[-max(1, int(seconds)):]
        end = next((l for l in lines if l.get("event") == "end"), None)
        calibrated = (next((l for l in lines if l.get("event") == "calibrated"), None)
                      or next((l for l in lines if l.get("event") == "start"), {}))

        def mean(key):
            vals = [l[key] for l in recent if l.get(key) is not None]
            return round(sum(vals) / len(vals), 1) if vals else None

        return {"gpu": self.gpu, "level": round(calibrated.get("level", 0), 1),
                "fps": mean("fps"), "gpuMs": mean("gpuMs"),
                "tempC": max((l.get("tempC", -1) for l in recent), default=None),
                "running": end is None, "end": end}

    def stop(self):
        _fleet().run_script(self.mid, r"""
Get-Process -Name 'mw-gpu-load' -ErrorAction SilentlyContinue | Stop-Process -Force
Start-Sleep -Seconds 1
Unregister-ScheduledTask -TaskName '%s' -Confirm:$false -ErrorAction SilentlyContinue
""" % TASK, timeout=60)


# ── a macOS host of the fleet ───────────────────────────────────────────────
#
# The Mac builds its own copy, from the tool's committed sources at this
# desk's HEAD, with the Qt it already has (qtshadertools, and qtmultimedia for
# the music). The build is redone only when the tool's tree changed, and is
# incremental then. The run goes through a launchd agent in the GUI session:
# a program started by ssh has no window server, and `launchctl asuser` is
# refused from ssh. Everything lives under ~/mw-gpu-load, never in /tmp, which
# other sessions share on that machine.

MAC_DIR = "$HOME/mw-gpu-load"
MAC_TOOL = MAC_DIR + "/build/mw-gpu-load"
MAC_JSON = MAC_DIR + "/run.jsonl"
MAC_LABEL = "com.moonlightweb.bench.gpuload"
_mac_ready = set()


def _tool_tree():
    """The git tree id of the tool's sources: what a remote build is keyed on."""
    p = subprocess.run(["git", "-C", REPO, "rev-parse", "HEAD:backend/native-host/tools/gpu-load"],
                       capture_output=True, text=True)
    if p.returncode != 0:
        raise Unavailable("cannot read the tool's sources from git")
    return p.stdout.strip()


def deploy_macos(mid):
    """Build the tool on a Mac from HEAD's sources, unless it already has them."""
    if mid in _mac_ready:
        return
    import tempfile
    import uuid
    f = _fleet()
    tree = _tool_tree()
    rc, out, err = f.run_script(mid, 'cat "%s/src.tree" 2>/dev/null; test -x "%s" && echo BUILT'
                                % (MAC_DIR, MAC_TOOL), timeout=60)
    if tree in (out or "") and "BUILT" in (out or ""):
        _mac_ready.add(mid)
        return
    name = "mwgl-src-%s.tgz" % uuid.uuid4().hex[:8]
    local = os.path.join(tempfile.gettempdir(), name)
    subprocess.run(["git", "-C", REPO, "archive", "--format=tar.gz", "-o", local, tree], check=True)
    try:
        f.push_file(mid, local, name)  # lands in the remote home directory
    finally:
        os.unlink(local)
    rc, out, err = f.run_script(mid, r"""
D="%(dir)s"
mkdir -p "$D"; rm -rf "$D/src"; mkdir "$D/src"
tar -xzf "$HOME/%(name)s" -C "$D/src" && rm -f "$HOME/%(name)s"
# The Qt of this machine: the newest one under ~/Qt that has the shader tools.
QT=$(ls -d "$HOME"/Qt/6.*/macos 2>/dev/null | sort -V | tail -1)
CMAKE=$(command -v cmake || ls "$HOME"/tools/cmake-*/CMake.app/Contents/bin/cmake 2>/dev/null | tail -1)
export PATH="$HOME/tools/bin:$PATH"
GEN=""; command -v ninja >/dev/null && GEN="-G Ninja"
[ -f "$D/build/CMakeCache.txt" ] || "$CMAKE" -S "$D/src" -B "$D/build" $GEN \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$QT" >"$D/build.log" 2>&1
"$CMAKE" --build "$D/build" >>"$D/build.log" 2>&1 && echo "%(tree)s" >"$D/src.tree"
test -x "%(tool)s" && echo BUILT
otool -L "%(tool)s" 2>/dev/null | grep -q QtMultimedia && echo MUSIC
""" % {"dir": MAC_DIR, "name": name, "tree": tree, "tool": MAC_TOOL}, timeout=900)
    if "BUILT" not in (out or ""):
        raise Unavailable("mw-gpu-load did not build on %s (see ~/mw-gpu-load/build.log)" % mid)
    if "MUSIC" not in (out or ""):
        # Still a valid load, only a silent one: say it rather than refuse.
        print("  [%s] mw-gpu-load built without Qt Multimedia: no music" % mid, flush=True)
    _mac_ready.add(mid)


def remote_encoder_gpu_macos(mid, target, port=48080):
    if target != "display":
        raise Unavailable("the load needs a physical display target, not %r" % target)
    rc, out, err = _fleet().run_script(
        mid, "curl -s -m 5 http://127.0.0.1:%d/api/native/status" % port, timeout=60)
    try:
        return json.loads((out or "").strip().splitlines()[-1])["displays"][0]["gpu"]
    except (ValueError, KeyError, IndexError):
        raise Unavailable("%s names no GPU for its first display" % mid)


class MacLoad(RemoteLoad):
    def __init__(self, mid, gpu, level=None, allow_no_sensor=False):
        self.mid = mid
        self.gpu = gpu
        # --topmost as on Windows: the stream must capture the tool, not
        # whatever the Mac's user left in front.
        args = [MAC_TOOL, "--gpu", gpu, "--autostart", "--topmost", "--json", MAC_JSON]
        if level:
            args += ["--level", str(level)]
        if allow_no_sensor:
            args.append("--allow-no-sensor")
        xml = "".join("<string>%s</string>" % a.replace("&", "&amp;").replace("<", "&lt;")
                      for a in args)
        rc, out, err = _fleet().run_script(mid, r"""
# Not in ~/Library/LaunchAgents: a plist written there makes macOS show
# "Background Items Added" on the user's screen, twice (23/09/2026).
P="%(dir)s/agent.plist"
launchctl bootout gui/$(id -u)/%(label)s 2>/dev/null
pkill -x mw-gpu-load 2>/dev/null
rm -f "%(json)s"
cat >"$P" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>Label</key><string>%(label)s</string>
<key>ProgramArguments</key><array>%(args)s</array>
<key>RunAtLoad</key><true/>
<key>StandardErrorPath</key><string>%(dir)s/run.err</string>
</dict></plist>
PLIST
launchctl bootstrap gui/$(id -u) "$P" && echo STARTED
""" % {"label": MAC_LABEL, "json": MAC_JSON, "args": xml, "dir": MAC_DIR}, timeout=60)
        if "STARTED" not in (out or ""):
            raise Unavailable("mw-gpu-load could not be started on %s" % mid)
        self.started = time.time()

    def _lines(self):
        rc, out, err = _fleet().run_script(self.mid, 'cat "%s" 2>/dev/null' % MAC_JSON, timeout=60)
        lines = []
        for raw in (out or "").splitlines():
            raw = raw.strip()
            if raw.startswith("{"):
                try:
                    lines.append(json.loads(raw))
                except ValueError:
                    pass
        return lines

    def stop(self):
        _fleet().run_script(self.mid, r"""
launchctl bootout gui/$(id -u)/%(label)s 2>/dev/null
pkill -x mw-gpu-load 2>/dev/null
rm -f "%(dir)s/agent.plist"
""" % {"label": MAC_LABEL, "dir": MAC_DIR}, timeout=60)
