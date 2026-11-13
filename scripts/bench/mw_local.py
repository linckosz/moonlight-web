"""Ask this machine's running MoonlightWeb one request through its local channel.

The server's own command does it: `<exe> [--dev] --local-request "<METHOD> <path>"`,
a body on standard input for a POST, the answer's body on standard output (for
a 2xx only) and "HTTP nnn" on standard error. The exe and its --dev are those of
the instance asked for, found from the process that listens on its port: the
channel is that instance's own, and a request by any other exe reaches another
one, or none.

Nothing here prints what it is given or what it gets: a caller that asks for a
PIN (POST /api/admin/pin/generate) gets it in memory and keeps it there.

    from mw_local import local_request
    status, body = local_request("GET /api/native/status", port=8443)
"""
import json
import os
import re
import subprocess
import sys

_WIN = sys.platform == "win32"
_NO_WINDOW = 0x08000000 if _WIN else 0


class LocalRequestError(RuntimeError):
    """No instance on that port, or no answer from it. The message names neither
    a body nor a value: only what was asked, and why it failed."""


def _windows_instance(port):
    script = (
        "$c = Get-NetTCPConnection -State Listen -LocalPort %d -ErrorAction SilentlyContinue | Select-Object -First 1;"
        "if ($c) { $p = Get-CimInstance Win32_Process -Filter \"ProcessId=$($c.OwningProcess)\";"
        "@{ exe = $p.ExecutablePath; dev = [bool]($p.CommandLine -match '(^|\\s)--dev(\\s|$)') } | ConvertTo-Json -Compress }"
        % int(port))
    r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
                       stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                       timeout=60, creationflags=_NO_WINDOW)
    try:
        found = json.loads(r.stdout.decode("utf-8", "replace") or "{}")
    except ValueError:
        return None
    return (found["exe"], bool(found.get("dev"))) if found.get("exe") else None


def _unix_instance(port):
    r = subprocess.run(["ss", "-lntpH", "sport = :%d" % int(port)], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30)
    m = re.search(r"pid=(\d+)", r.stdout.decode("utf-8", "replace"))
    pids = [int(m.group(1))] if m else []
    if not pids:
        r = subprocess.run(["lsof", "-nP", "-t", "-iTCP:%d" % int(port), "-sTCP:LISTEN"],
                           stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30)
        pids = [int(x) for x in r.stdout.split() if x.isdigit()]
    for pid in pids:
        exe, argv = "", []
        try:
            exe = os.readlink("/proc/%d/exe" % pid)
            argv = open("/proc/%d/cmdline" % pid, "rb").read().split(b"\0")
        except OSError:
            r = subprocess.run(["ps", "-o", "comm=", "-p", str(pid)], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30)
            exe = r.stdout.decode("utf-8", "replace").strip()
            r = subprocess.run(["ps", "-o", "args=", "-p", str(pid)], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=30)
            argv = r.stdout.split()
        if exe:
            return exe, b"--dev" in argv
    return None


_VERB = b"local-request"
_KNOWN = {}
# The names the server's exe goes by. Whatever else listens on a port is never
# started with an argument, whatever its bytes hold.
_NAMES = ("moonlightweb.exe", "moonlightweb", "moonlight-web", "moonlightwebdev.exe", "moonlightwebdev",
          "moonlightweb-dev")


def is_server_exe(exe):
    """Whether @p exe is named as the server's exe is (its file name, any case)."""
    return os.path.basename(exe or "").lower() in _NAMES


def knows_local_request(exe):
    """Whether @p exe has the command, read from its bytes, never by starting it.

    An exe that does not know an option refuses it, and on Windows an application
    with no console says so in a dialog, in front of whoever sits at the machine.
    Its version says nothing sure either (a build of main reads 0.x.y.gHASH-dev
    with the command or without it). The option's name is in the exe, in ASCII
    or in UTF-16, from the build that brought the command on. An exe not named
    as the server's (is_server_exe) has it never.
    """
    if not is_server_exe(exe):
        return False
    try:
        st = os.stat(exe)
    except OSError:
        return False
    key = (exe, st.st_size, st.st_mtime_ns)
    if key not in _KNOWN:
        wide = _VERB.decode("ascii").encode("utf-16-le")
        found, tail = False, b""
        try:
            with open(exe, "rb") as f:
                for chunk in iter(lambda: f.read(1 << 20), b""):
                    block = tail + chunk
                    if _VERB in block or wide in block:
                        found = True
                        break
                    tail = block[-(len(wide) - 1):]
        except OSError:
            found = False
        _KNOWN[key] = found
    return _KNOWN[key]


def instance(port):
    """(exe, dev) of the process that listens on @p port on this machine, or None."""
    try:
        return _windows_instance(port) if _WIN else _unix_instance(port)
    except (OSError, subprocess.TimeoutExpired):
        return None


def local_request(value, port=None, body=None, exe=None, dev=None, timeout=60):
    """(status, body bytes) of one request: "<METHOD> <path>", as the command takes it.

    The instance is the one listening on @p port, or @p exe with @p dev when both
    are given. @p body (bytes or a JSON-able object) goes on standard input. The
    body comes back only for a 2xx, empty otherwise: the command shows no
    refusal's body. Raises LocalRequestError when nothing could be asked, an exe
    without the command included (knows_local_request): it is never started.
    """
    if exe is None:
        if port is None:
            raise LocalRequestError("local request %s: no port and no exe" % value.split(" ")[0])
        found = instance(port)
        if not found:
            raise LocalRequestError("local request: nothing of this machine listens on port %d" % int(port))
        exe, dev = found
    if not knows_local_request(exe):
        raise LocalRequestError("local request: this instance's exe has no --local-request (not started)")
    if body is not None and not isinstance(body, (bytes, bytearray)):
        body = json.dumps(body).encode("utf-8")
    argv = [exe] + (["--dev"] if dev else []) + ["--local-request", value]
    try:
        r = subprocess.run(argv, input=body if body is not None else b"", stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, cwd=os.path.dirname(exe) or None, timeout=timeout,
                           creationflags=_NO_WINDOW)
    except subprocess.TimeoutExpired:
        raise LocalRequestError("local request: no answer in %d s" % timeout) from None
    except OSError:
        raise LocalRequestError("local request: the instance's exe could not be started") from None
    status = 0
    for line in r.stderr.decode("utf-8", "replace").splitlines():
        m = re.fullmatch(r"HTTP (\d{3})", line.strip())
        if m:
            status = int(m.group(1))
    return status, r.stdout


def local_json(value, port=None, body=None, exe=None, dev=None, timeout=60):
    """The answer of local_request as JSON: (status, object or None)."""
    status, raw = local_request(value, port=port, body=body, exe=exe, dev=dev, timeout=timeout)
    try:
        return status, json.loads(raw.decode("utf-8")) if raw.strip() else None
    except ValueError:
        return status, None
