"""Reach the bench fleet, and install on it.

Nothing in scripts/bench/ knows how to log into a machine: discover.ps1 tests
port 22 and stops there. The transport has always been a pair of hands. This
module is that pair of hands, and it is the whole reason the acceptance run can
be unattended.

Three ways in, because the fleet has three:

  ssh           an entry of ~/.ssh/config, or a key file — the good case
  wsl-sshpass   a password-only box, reached through WSL's sshpass because the
                Windows OpenSSH client has no way to feed one
  local         this machine, through PowerShell

Two rules learned the hard way and enforced here:

  * A command is never interpolated into `ssh "..."`. PowerShell 5.1 eats the
    double quotes before ssh sees them, and the remote shell on a Windows box
    is cmd.exe, which eats what is left. Every script is written to a file,
    copied, and run from there.
  * A Windows SSH session lands in session 0. It has no desktop, so anything
    that must touch the screen goes through a scheduled task, and an installer
    that launches the app never gives the shell back — it is started detached.
"""
import json
import os
import shlex
import subprocess
import tempfile
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
SSH_OPTS = ["-o", "StrictHostKeyChecking=no", "-o", "ConnectTimeout=12",
            "-o", "ServerAliveInterval=20"]

# What each machine IS — never where it lives or how to log into it.
#
# The split is the one scripts/bench/hosts.json already makes, and for the same
# reason: a bench fleet is a description of one room, true there and false
# everywhere else, and a repository is the wrong place for somebody else's
# hardware. Addresses, accounts and passwords come from hosts.local.json, which
# git ignores. Putting them here, even briefly, is how a password reaches a
# public remote.
RECIPES = {
    "local": {
        "label": "the machine running the campaign — reference host and measuring client",
        "os": "windows", "perf": True,
    },
    "mw-mac": {"label": "MacBook Pro M1 Pro, VideoToolbox", "os": "macos", "perf": True},
    "mw-intel": {"label": "Intel N95 / UHD Graphics, the only QuickSync bench",
                 "os": "windows", "arch": "x64", "perf": True},
    "mw-arm": {"label": "Snapdragon 7c, Windows on ARM. Compatibility only",
               "os": "windows", "arch": "arm64", "perf": False},
    "um790pro": {"label": "mini PC, Radeon 780M / VA-API under Linux", "os": "linux",
                 "perf": True},
    "mw-debian": {"label": "Debian in a VM, no GPU. Compatibility only", "os": "linux",
                  "perf": False},
}

# An OS guess for a machine the recipes do not name, from what discover.ps1
# style hints are available.
_OS_HINTS = {"windows": "windows", "windows-arm64": "windows", "macos": "macos",
             "linux": "linux", "darwin": "macos"}


def _load_fleet():
    """Merge the recipes above with the addresses in hosts.local.json.

    Missing file, or a machine with no address: that machine is simply absent
    from the run, and the report says so, rather than the harness failing to
    import.
    """
    here = os.path.dirname(HERE)
    out = {}
    local = {}
    path = os.path.join(here, "hosts.local.json")
    if os.path.exists(path):
        try:
            with open(path, encoding="utf-8") as f:
                data = json.load(f)
            for entry in data.get("machines", []):
                if entry.get("id"):
                    local[entry["id"]] = entry
        except (ValueError, OSError):
            local = {}

    for mid, recipe in RECIPES.items():
        entry = local.get(mid, {})
        m = dict(recipe)
        expect = entry.get("expect") or {}
        m["os"] = _OS_HINTS.get(expect.get("os", ""), m["os"])
        m["address"] = entry.get("address", "127.0.0.1" if mid == "local" else "")
        kind = entry.get("kind") or ("local" if mid == "local" else "ssh")
        if entry.get("sshAlias"):
            m["kind"] = "ssh"
            m["ssh"] = entry["sshAlias"]
        elif kind == "local":
            m["kind"] = "local"
        elif entry.get("password"):
            # No sshpass on a Windows desk; WSL has one. This says nothing about
            # the remote's OS — the dual-boot mini PC answers here under Windows
            # too, and run_script picks the shell from m["os"], not from this.
            m["kind"] = "wsl-sshpass"
            m["user"] = entry.get("user", "")
            m["password"] = entry["password"]
        else:
            m["kind"] = "ssh"
            m["ssh"] = "%s@%s" % (entry.get("user", ""), m["address"])
        if entry.get("keyFile"):
            m["key"] = os.path.expanduser(entry["keyFile"])
        if entry.get("password"):
            m["sudo_password"] = entry["password"]
        if entry.get("sudoPassword"):
            m["sudo_password"] = entry["sudoPassword"]
        if m["kind"] == "local" or m["address"]:
            out[mid] = m
    return out


MACHINES = _load_fleet()

DEV_HTTP = 48080
DEV_HTTPS = 48443

# The acceptance run probes the DEV edition, which installs beside production.
# A pre-release campaign measures the hidden PROD staging build instead:
# MW_BENCH_EDITION=prod points every probe at MoonlightWeb, not MoonlightWebDev.
EDITION = os.environ.get("MW_BENCH_EDITION", "dev").strip().lower()

_PROD_NAMES = [
    # Most specific first: the bare "MoonlightWebDev" would match inside them.
    ("/Applications/MoonlightWebDev.app/Contents/MacOS/MoonlightWebDev",
     "/Applications/MoonlightWeb.app/Contents/MacOS/MoonlightWeb"),
    ("com.moonlightweb.agent.dev", "com.moonlightweb.agent"),
    ("MoonlightWeb/MoonlightWebDev/logs", "MoonlightWeb/MoonlightWeb/logs"),
    ("Logs/MoonlightWebDev/", "Logs/MoonlightWeb/"),
    ("moonlightweb-dev", "moonlightweb"),
    ("MoonlightWebDev", "MoonlightWeb"),
]


def _for_edition(script, os_name):
    """The probe script as written for DEV, renamed for PROD when asked."""
    if EDITION != "prod":
        return script
    # The process pattern: "MoonlightWeb" alone would also match a DEV beside it.
    pattern = "MoonlightWeb.app/" if os_name == "macos" else "/opt/moonlightweb/"
    script = script.replace("python3 - MoonlightWebDev", "python3 - " + pattern)
    for dev, prod in _PROD_NAMES:
        script = script.replace(dev, prod)
    # The guessed ports, when the process's own are not readable (lsof without
    # root on Linux): the DEV defaults would land on a DEV running beside it.
    script = script.replace("for guess in (48080, 48443, 18080, 18443, 80, 443):",
                            "for guess in (49080, 49443, 80, 443):")
    return script


class RemoteError(Exception):
    pass


def _run(argv, timeout=300, check=False):
    p = subprocess.run(argv, capture_output=True, text=True, timeout=timeout,
                       encoding="utf-8", errors="replace")
    if check and p.returncode != 0:
        raise RemoteError("%s -> %d\n%s\n%s" % (argv[0], p.returncode, p.stdout, p.stderr))
    return p


def _wsl_path(win_path):
    """C:\\x -> /mnt/c/x, so WSL can read a file this process just wrote."""
    p = os.path.abspath(win_path).replace("\\", "/")
    if len(p) > 1 and p[1] == ":":
        return "/mnt/" + p[0].lower() + p[2:]
    return p


def ssh_argv(m):
    argv = ["ssh"] + SSH_OPTS
    if m.get("key"):
        argv += ["-i", m["key"]]
    argv.append(m["ssh"])
    return argv


# A Windows box's default ssh shell is cmd.exe on some of these machines and
# PowerShell on others, and the difference is not cosmetic: `< NUL` and `type`
# are cmd builtins, and PowerShell answers "The '<' operator is reserved for
# future use" and then reads a file that was never written — which looks
# exactly like a script that printed nothing. Probed once per machine, because
# it cannot change under us mid-run. `%COMPUTERNAME%` is the tell: cmd expands
# it, PowerShell hands it back verbatim.
_SHELL_CACHE = {}


def _windows_shell(mid, runner):
    if mid in _SHELL_CACHE:
        return _SHELL_CACHE[mid]
    probe = runner("echo %COMPUTERNAME%", 60)
    out = (probe.stdout or "").strip()
    shell = "powershell" if "%COMPUTERNAME%" in out else "cmd"
    _SHELL_CACHE[mid] = shell
    return shell


def _win_run_cmd(script_path, log_path, shell):
    """The one-liner that runs a .ps1 to a log file, severing inherited handles.

    An installer hands its handles to a process that outlives the ssh session,
    and ssh waits for every one of them: without the redirection the channel
    hangs for the full timeout with the work long since finished.
    """
    inner = ('powershell -NoProfile -ExecutionPolicy Bypass -File "%s" '
             '> "%s" 2>&1 < NUL' % (script_path, log_path))
    if shell == "cmd":
        return inner
    # --% stops PowerShell parsing the rest, so cmd receives it verbatim.
    return "cmd --% /c " + inner


def _win_run_direct(script_path, shell):
    """The same one-liner with its output on the channel: nothing written on the
    machine but the script itself (run_script's `direct`)."""
    inner = 'powershell -NoProfile -ExecutionPolicy Bypass -File "%s" < NUL' % script_path
    return inner if shell == "cmd" else "cmd --% /c " + inner


def _win_cat(path, shell):
    return ('type "%s"' % path) if shell == "cmd" else ('Get-Content -Raw "%s"' % path)


def _win_del(paths, shell):
    if shell == "cmd":
        return "del /q " + " ".join('"%s"' % p for p in paths)
    return "Remove-Item -Force -ErrorAction SilentlyContinue " + ", ".join('"%s"' % p for p in paths)


def run_script(mid, script, timeout=600, shell=None, direct=False):
    """Run a script on a machine. Always by file — never interpolated into ssh.

    `shell` defaults to what the machine's OS uses: powershell on Windows,
    /bin/sh elsewhere. Returns (rc, stdout, stderr).

    `direct`: on Windows, the output comes back through the ssh channel itself
    instead of a file written on the machine and fetched after. For a script
    whose output must never sit in a file (a PIN, the rendezvous address), and
    which starts nothing that outlives it: what the file is there for is an
    installer's inherited handles.
    """
    m = MACHINES[mid]
    shell = shell or ("powershell" if m["os"] == "windows" else "sh")
    suffix = ".ps1" if shell == "powershell" else ".sh"
    name = "mwacc-%s%s" % (uuid.uuid4().hex[:8], suffix)
    local = os.path.join(tempfile.gettempdir(), name)
    # LF and UTF-8 without a BOM: a BOM makes PowerShell report a phantom
    # "Missing closing '}'", and CRLF makes /bin/sh choke on every line.
    with open(local, "w", encoding="utf-8", newline="\n") as f:
        f.write(script)

    try:
        if m["kind"] == "local":
            p = _run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass",
                      "-File", local], timeout=timeout)
            return p.returncode, p.stdout, p.stderr

        if m["kind"] == "wsl-sshpass":
            # The transport and the remote OS are independent. sshpass is what
            # this desk has for a machine that only takes a password, and one
            # of those machines is a Windows box: sending it `sh script` would
            # fail in a way that reads like the script itself being wrong.
            windows = m["os"] == "windows"
            remote = ("C:/Windows/Temp/" + name) if windows else ("/tmp/" + name)
            sent = _run(["wsl", "-u", "root", "--", "bash", "-c",
                         "sshpass -p %s scp %s %s %s@%s:%s" % (
                             shlex.quote(m["password"]), " ".join(SSH_OPTS),
                             shlex.quote(_wsl_path(local)), m["user"], m["address"], remote)],
                        timeout=timeout)
            if sent.returncode != 0:
                raise RemoteError("scp to %s failed: %s" % (mid, sent.stderr.strip()))

            def _sshpass(cmd, tmo):
                return _run(["wsl", "-u", "root", "--", "bash", "-c",
                             "sshpass -p %s ssh %s %s@%s %s" % (
                                 shlex.quote(m["password"]), " ".join(SSH_OPTS),
                                 m["user"], m["address"], shlex.quote(cmd))],
                            timeout=tmo)

            if windows and direct:
                win = remote.replace("/", "\\")
                sh = _windows_shell(mid, _sshpass)
                p = _sshpass(_win_run_direct(win, sh), timeout)
                _sshpass(_win_del([win], sh), 60)
                return p.returncode, p.stdout, p.stderr
            if windows:
                # Same inherited-handle trap as the plain-ssh Windows path: run
                # to a file, then fetch the file in a second session.
                # Backslashes throughout — cmd.exe reads a leading forward
                # slash as the start of an option.
                win = remote.replace("/", "\\")
                log = win + ".out"
                sh = _windows_shell(mid, _sshpass)
                p = _sshpass(_win_run_cmd(win, log, sh), timeout)
                back = _sshpass(_win_cat(log, sh), 120)
                _sshpass(_win_del([win, log], sh), 60)
                return p.returncode, back.stdout, (p.stderr or "") + (back.stderr or "")

            p = _sshpass("sh %s; rc=$?; rm -f %s; exit $rc" % (remote, remote), timeout)
            return p.returncode, p.stdout, p.stderr

        # plain ssh
        remote = ("C:/Windows/Temp/" + name) if m["os"] == "windows" else ("/tmp/" + name)
        scp = ["scp"] + SSH_OPTS
        if m.get("key"):
            scp += ["-i", m["key"]]
        scp += [local, "%s:%s" % (m["ssh"], remote)]
        sent = _run(scp, timeout=timeout)
        if sent.returncode != 0:
            raise RemoteError("scp to %s failed: %s" % (mid, sent.stderr.strip()))

        if m["os"] == "windows" and direct:
            win = remote.replace("/", "\\")
            sh = _windows_shell(mid, lambda c, t: _run(ssh_argv(m) + [c], timeout=t))
            p = _run(ssh_argv(m) + [_win_run_direct(win, sh)], timeout=timeout)
            _run(ssh_argv(m) + [_win_del([win], sh)], timeout=60)
            return p.returncode, p.stdout, p.stderr
        if m["os"] == "windows":
            # ssh waits for every inherited handle to close, not just for the
            # command to exit. An installer that launches the app hands those
            # handles to a process that outlives the session, and the channel
            # then hangs for the full timeout with the work long since done.
            # Redirecting to a file, with stdin from NUL, severs the
            # inheritance. The output is then fetched by a SECOND session:
            # wrapping both in one `cmd /c "..."` looks tidier and silently
            # produces nothing, because the inner quotes break cmd's parsing.
            # cmd.exe reads a forward slash as the start of an option, so the
            # redirection target and `type` both need backslashes — with slashes
            # the file is written nowhere and `type` reports it missing, which
            # reads exactly like a script that printed nothing.
            win = remote.replace("/", "\\")
            log = win + ".out"
            sh = _windows_shell(mid, lambda c, t: _run(ssh_argv(m) + [c], timeout=t))
            p = _run(ssh_argv(m) + [_win_run_cmd(win, log, sh)], timeout=timeout)
            back = _run(ssh_argv(m) + [_win_cat(log, sh)], timeout=120)
            _run(ssh_argv(m) + [_win_del([win, log], sh)], timeout=60)
            return p.returncode, back.stdout, (p.stderr or "") + (back.stderr or "")

        cmd = "sh %s; rc=$?; rm -f %s; exit $rc" % (remote, remote)
        p = _run(ssh_argv(m) + [cmd], timeout=timeout)
        return p.returncode, p.stdout, p.stderr
    finally:
        try:
            os.unlink(local)
        except OSError:
            pass


def push_file(mid, local_path, remote_path, timeout=900):
    """Copy one file to a machine. The installers are 40-120 MB, hence the timeout."""
    m = MACHINES[mid]
    if m["kind"] == "local":
        return remote_path
    if m["kind"] == "wsl-sshpass":
        p = _run(["wsl", "-u", "root", "--", "bash", "-c",
                  "sshpass -p %s scp %s %s %s@%s:%s" % (
                      shlex.quote(m["password"]), " ".join(SSH_OPTS),
                      shlex.quote(_wsl_path(local_path)), m["user"], m["address"],
                      remote_path)], timeout=timeout)
        if p.returncode != 0:
            raise RemoteError("scp %s -> %s: %s" % (local_path, mid, p.stderr.strip()))
        return remote_path
    scp = ["scp"] + SSH_OPTS
    if m.get("key"):
        scp += ["-i", m["key"]]
    scp += [local_path, "%s:%s" % (m["ssh"], remote_path)]
    p = _run(scp, timeout=timeout)
    if p.returncode != 0:
        raise RemoteError("scp %s -> %s: %s" % (local_path, mid, p.stderr.strip()))
    return remote_path


def reachable(mid):
    rc, out, _ = run_script(mid, "echo MW_OK", timeout=60)
    return rc == 0 and "MW_OK" in out


# ── what the host says about itself, from its own shell ─────────────────────
# /api/native/status and /api/internet/status redact everything interesting for
# any caller that is not local, and /api/admin/pin/generate refuses outright.
# That is why these go through the machine's own shell rather than over the LAN,
# and there through the instance's local channel (`--local-request`) when its
# exe has it: from a build that has it on, an address alone opens none of them.
#
# The port is DISCOVERED, never assumed. 48080/48443 are only the defaults: a
# settings.json decides, a silent uninstall keeps that settings.json, and a
# fresh install therefore inherits whatever was there. The dev instance on this
# very desk answers on 18080/18443. Asking the process which ports it listens
# on, and then asking each of them, is the only reading that cannot be wrong.

_PROBE_PY = r'''
import hashlib, json, os, socket, subprocess, sys, urllib.request

APP = sys.argv[1] if len(sys.argv) > 1 else "MoonlightWebDev"


def listening_ports(pattern):
    """Ports held by a process whose command line matches, by whatever tool is here."""
    pids = set()
    for argv in (["pgrep", "-f", pattern], ["pgrep", pattern]):
        try:
            p = subprocess.run(argv, capture_output=True, text=True, timeout=20)
            pids.update(int(x) for x in p.stdout.split() if x.isdigit())
        except Exception:
            pass
    ports = set()
    for pid in pids:
        for argv in (["lsof", "-nP", "-a", "-p", str(pid), "-iTCP", "-sTCP:LISTEN"],
                     ["ss", "-lntp"]):
            try:
                p = subprocess.run(argv, capture_output=True, text=True, timeout=20)
            except Exception:
                continue
            for line in p.stdout.splitlines():
                if argv[0] == "ss" and ("pid=%d," % pid) not in line:
                    continue
                for tok in line.replace("\t", " ").split():
                    if ":" in tok:
                        tail = tok.rsplit(":", 1)[-1].strip("()[]")
                        if tail.isdigit() and 1 <= int(tail) <= 65535:
                            ports.add(int(tail))
            if ports:
                break
    return sorted(ports)


def open_port(port):
    """A 300 ms socket test. A filtered port can hold an HTTP client for minutes."""
    s = socket.socket()
    s.settimeout(0.3)
    try:
        s.connect(("127.0.0.1", port))
        return True
    except Exception:
        return False
    finally:
        s.close()


def answers(port, scheme="http"):
    if not open_port(port):
        return None
    url = "%s://127.0.0.1:%d/api/native/status" % (scheme, port)
    try:
        ctx = None
        if scheme == "https":
            import ssl
            ctx = ssl.create_default_context()
            ctx.check_hostname = False
            ctx.verify_mode = ssl.CERT_NONE
        with urllib.request.urlopen(url, timeout=6, context=ctx) as r:
            body = r.read().decode("utf-8", "replace")
        json.loads(body)
        return body
    except Exception:
        return None


out = {"appName": APP}
candidates = listening_ports(APP)
# The defaults come last: a discovered port is a fact, a default is a guess.
for guess in (48080, 48443, 18080, 18443, 80, 443):
    if guess not in candidates:
        candidates.append(guess)

http_port = https_port = 0
for port in candidates:
    if not http_port and answers(port, "http"):
        http_port = port
    if not https_port and answers(port, "https"):
        https_port = port
    if http_port and https_port:
        break
out["listening"] = candidates[:12]
out["httpPort"] = http_port
out["httpsPort"] = https_port

if http_port:
    out["localChannel"] = channel_of(http_port)
    out["native"] = json.dumps(read("/api/native/status", http_port))
    out["internet"] = json.dumps(reduced_internet(read("/api/internet/status", http_port)))
    out["vdisplay"] = json.dumps(reduced_vdisplay(read("/api/native/virtual-display", http_port)))
else:
    out["native"] = json.dumps({"error": "nothing answered on any listening port"})

print("MWPROBE " + json.dumps(out))
'''

# What the probes of a Unix machine read with: this folder's mw_local.py itself,
# sent along, and the reads built on it. The local channel when the instance's exe
# has the command (read from its bytes: an exe that does not know an option is
# never handed it); the loopback address otherwise, which an older build answers.
# What leaves the machine is cut down to what a record may hold (reduced_*): the
# rendezvous address and the PIN never do, they have their own calls (secret()).
_BENCH = os.path.dirname(HERE)


def _mw_local_py():
    with open(os.path.join(_BENCH, "mw_local.py"), encoding="utf-8") as f:
        return f.read()


def _mw_local_ps():
    with open(os.path.join(_BENCH, "mw-local.ps1"), encoding="utf-8") as f:
        return f.read()


_READS_PY = r'''
import json as _json, urllib.request as _urlreq


def channel_of(port):
    found = instance(port)
    return bool(found and knows_local_request(found[0]))


def read(path, port):
    """One GET of this machine's instance on @p port, as a JSON object."""
    found = instance(port)
    if found and knows_local_request(found[0]):
        try:
            st, doc = local_json("GET " + path, exe=found[0], dev=found[1])
        except LocalRequestError:
            return {"error": "the local channel did not answer"}
        return doc if st == 200 and isinstance(doc, dict) else {"error": "local channel: HTTP %d" % st}
    try:
        with _urlreq.urlopen("http://127.0.0.1:%d%s" % (port, path), timeout=20) as r:
            return _json.loads(r.read().decode("utf-8", "replace"))
    except Exception as e:
        return {"error": type(e).__name__}


def rendezvous_of(internet):
    """The address an owner reconnects on, out of /api/internet/status: kept in
    memory, never in what a record holds."""
    rdv = (internet or {}).get("rendezvous") or {}
    for key in ("url", "link", "owner_url"):
        if rdv.get(key):
            return rdv[key]
    ident = rdv.get("id") or rdv.get("instance_id") or rdv.get("instanceId")
    host = rdv.get("host") or rdv.get("server") or "stream.dev.moonlightweb.top"
    if ident:
        host = host.replace("https://", "").replace("http://", "").rstrip("/")
        return "https://%s/%s" % (host, ident)
    return ""


def reduced_internet(internet):
    """What a record may hold of the internet link: whether it is up, whether a
    rendezvous address is known, and the server's host name. Never the address,
    the instance's id, the public IP or the permanent id."""
    if not isinstance(internet, dict) or internet.get("error"):
        return {"error": (internet or {}).get("error", "no answer")}
    url = rendezvous_of(internet)
    host = url.split("://", 1)[-1].split("/", 1)[0] if url else ""
    return {"active": internet.get("active"), "rendezvousKnown": bool(url), "rendezvousHost": host}


def reduced_vdisplay(vd):
    """The virtual display's booleans and its job's state. Never the job's error
    (a helper's or Windows' text, which may hold a local path) nor its display."""
    if not isinstance(vd, dict) or vd.get("error"):
        return {"error": (vd or {}).get("error", "no answer")}
    job = vd.get("job") if isinstance(vd.get("job"), dict) else {}
    out = {k: vd.get(k) for k in ("supported", "installed", "enabled", "active")}
    out["job"] = {"state": job.get("state")}
    return out
'''


_READS_PS = r"""
# The reads of a Windows machine: mw-local.ps1 above, then these. Same rules as the
# Unix ones (_READS_PY): the local channel when the exe has the command, the
# loopback address otherwise; records hold the reduced forms only.
function Read-MwObject($path, $port) {
    try { return Get-MwLocalApi "http://127.0.0.1:$port" $path 20 }
    catch { return [pscustomobject]@{ error = 'no answer' } }
}
function Read-MwJson($path, $port) { (Read-MwObject $path $port) | ConvertTo-Json -Compress -Depth 8 }
function Get-MwRendezvous($internet) {
    $rdv = $internet.rendezvous
    if (-not $rdv) { return '' }
    foreach ($k in @('url', 'link', 'owner_url')) { if ($rdv.$k) { return [string]$rdv.$k } }
    $id = $rdv.id; if (-not $id) { $id = $rdv.instance_id }; if (-not $id) { $id = $rdv.instanceId }
    $h = $rdv.host; if (-not $h) { $h = $rdv.server }; if (-not $h) { $h = 'stream.dev.moonlightweb.top' }
    if ($id) { return 'https://' + (($h -replace '^https?://', '').TrimEnd('/')) + '/' + $id }
    ''
}
function Get-MwReducedInternet($internet) {
    if (-not $internet -or $internet.error) { return [pscustomobject]@{ error = 'no answer' } }
    $url = Get-MwRendezvous $internet
    $h = if ($url) { ($url -replace '^[a-z]+://', '') -replace '/.*$', '' } else { '' }
    [pscustomobject]@{ active = $internet.active; rendezvousKnown = [bool]$url; rendezvousHost = $h }
}
function Get-MwReducedVdisplay($vd) {
    if (-not $vd -or $vd.error) { return [pscustomobject]@{ error = 'no answer' } }
    $state = $null; if ($vd.job) { $state = $vd.job.state }
    [pscustomobject]@{ supported = $vd.supported; installed = $vd.installed; enabled = $vd.enabled
                       active = $vd.active; job = [pscustomobject]@{ state = $state } }
}
"""


def _reads_ps():
    return _mw_local_ps() + "\n" + _READS_PS


# ── the two values no record holds: a PIN, the rendezvous address ───────────
# Each has its own call, made when a pass is about to use it (a PIN only when a
# client is about to pair: one live PIN at a time). The machine prints it on one
# line, MWPIN or MWRDV, that comes back through the ssh channel itself (direct:
# no file on the machine) into this process's memory, and nowhere else: never
# printed, logged, written to a record, or put in an exception's text.
_SECRET_PY = r'''
import sys as _sys
_port, _what = int(_sys.argv[1]), _sys.argv[2]
if _what == "pin":
    _pin = ""
    _found = instance(_port)
    if _found and knows_local_request(_found[0]):
        try:
            _st, _doc = local_json("POST /api/admin/pin/generate", exe=_found[0], dev=_found[1])
            _pin = (_doc or {}).get("pin", "") if _st == 200 else ""
        except LocalRequestError:
            _pin = ""
    else:
        # An older build: its loopback still mints one, with the run's admin key,
        # which stays in this variable.
        try:
            with _urlreq.urlopen("http://127.0.0.1:%d/api/admin/token" % _port, timeout=20) as _r:
                _tok = _json.loads(_r.read().decode("utf-8")).get("token", "")
            _q = _urlreq.Request("http://127.0.0.1:%d/api/admin/pin/generate" % _port, data=b"{}", method="POST",
                                 headers={"X-MW-Admin-Key": _tok, "Content-Type": "application/json"})
            with _urlreq.urlopen(_q, timeout=20) as _r:
                _pin = _json.loads(_r.read().decode("utf-8")).get("pin", "")
        except Exception:
            _pin = ""
    print("MWPIN " + _pin)
elif _what == "rdv":
    print("MWRDV " + rendezvous_of(read("/api/internet/status", _port)))
'''

_SECRET_PS = r"""
$ErrorActionPreference = 'SilentlyContinue'
$port = __PORT__
$what = '__WHAT__'
__READS_PS__
if ($what -eq 'pin') {
    $pin = ''
    $found = Get-MwInstance $port
    if ($found -and (Test-MwLocalRequest $found.Exe)) {
        try {
            $r = Invoke-MwLocalRequest 'POST /api/admin/pin/generate' -Exe $found.Exe -Dev:$found.Dev
            if ($r.Status -eq 200 -and $r.Json) { $pin = [string]$r.Json.pin }
        } catch { $pin = '' }
    } else {
        # An older build: its loopback still mints one, with the run's admin key,
        # which stays in this variable.
        try {
            $tok = (Invoke-RestMethod -Uri "http://127.0.0.1:$port/api/admin/token" -TimeoutSec 20).token
            $pin = [string](Invoke-RestMethod -Uri "http://127.0.0.1:$port/api/admin/pin/generate" -Method Post `
                    -ContentType 'application/json' -Body '{}' -Headers @{ 'X-MW-Admin-Key' = $tok } -TimeoutSec 20).pin
        } catch { $pin = '' }
        $tok = $null
    }
    'MWPIN ' + $pin
} elseif ($what -eq 'rdv') {
    'MWRDV ' + (Get-MwRendezvous (Read-MwObject '/api/internet/status' $port))
}
"""

_READ_ONE_PS = r"""
$ErrorActionPreference = 'SilentlyContinue'
__READS_PS__
'MWREAD ' + (Read-MwJson '__PATH__' __PORT__)
"""

_READ_ONE_PY = r'''
import json as _j, sys as _s
print("MWREAD " + _j.dumps(read(_s.argv[2], int(_s.argv[1]))))
'''

# What a remote read may ask: the reads whose whole answer a record may hold.
_PLAIN_READS = ("/api/native/status",)


def remote_read(mid, path, port):
    """One GET of @p mid's instance on @p port, through its local channel when its
    exe has the command (the loopback address otherwise), as a JSON object."""
    if path not in _PLAIN_READS:
        raise ValueError("not a read a record may hold: %s" % path)
    if MACHINES[mid]["os"] == "windows":
        script = (_READ_ONE_PS.replace("__PATH__", path).replace("__PORT__", str(int(port)))
                  .replace("__READS_PS__", _reads_ps()))
    else:
        body = _mw_local_py() + "\n" + _READS_PY + "\n" + _READ_ONE_PY
        script = "python3 - %d %s <<'MWPY'\n%s\nMWPY\n" % (int(port), path, body)
    _rc, out, _err = run_script(mid, script, timeout=120)
    for line in (out or "").splitlines():
        if line.strip().startswith("MWREAD "):
            return json.loads(line.strip()[len("MWREAD "):])
    return {"error": "no answer"}


_SECRETS = {}


def redacted(text):
    """@p text without its MWPIN, MWRDV and MWPROBE lines: what a remote output may
    show of itself, in a diagnosis, a record or an exception."""
    return "\n".join(line for line in (text or "").splitlines()
                     if not line.strip().startswith(("MWPIN", "MWRDV", "MWPROBE")))


def _secret(mid, what, port):
    """The value of one MWPIN or MWRDV line of @p mid, or "". Nothing of the run's
    output is ever shown: a failure says only that no value came."""
    if not port:
        return ""
    m = MACHINES[mid]
    if m["os"] == "windows":
        script = (_SECRET_PS.replace("__PORT__", str(int(port))).replace("__WHAT__", what)
                  .replace("__READS_PS__", _reads_ps()))
    else:
        body = _mw_local_py() + "\n" + _READS_PY + "\n" + _SECRET_PY
        script = "python3 - %d %s <<'MWPY'\n%s\nMWPY\n" % (int(port), what, body)
    try:
        _rc, out, _err = run_script(mid, script, timeout=120, direct=True)
    except Exception:
        return ""
    tag = "MWPIN " if what == "pin" else "MWRDV "
    value = ""
    for line in (out or "").splitlines():
        if line.strip().startswith(tag):
            value = line.strip()[len(tag):].strip()
    out = _err = None
    return value


def mint_pin(mid, probe_result):
    """A fresh PIN of @p mid's instance, for a client about to pair: in memory only.
    "" when none came (the caller says so, without the value)."""
    pin = _secret(mid, "pin", (probe_result or {}).get("httpPort"))
    return pin if pin.isdigit() and 4 <= len(pin) <= 8 else ""


def _probe_body_py():
    """The Unix probe: mw_local.py, the reads, then the probe itself."""
    return _mw_local_py() + "\n" + _READS_PY + "\n" + _PROBE_PY


def _probe_py_for(mid):
    """The same probe everywhere; only the OS-specific extras differ."""
    return _probe_body_py()


def probe(mid):
    """Version in service, autostart state, native status, whether a rendezvous address
    is known. Never a PIN (mint_pin) nor the rendezvous address (rendezvous_url)."""
    m = MACHINES[mid]
    pyfile = "mwprobe-%s.py" % uuid.uuid4().hex[:8]

    if m["os"] == "windows":
        remote_py = "C:/Windows/Temp/" + pyfile
        # Windows has no pgrep and may have no python: the port discovery is
        # redone in PowerShell, and the HTTP calls with Invoke-WebRequest.
        script = r"""
$ErrorActionPreference = 'SilentlyContinue'
$app = 'MoonlightWebDev'
$out = @{ appName = $app }

# The HTTPS listener presents the machine's own certificate, which nothing here
# has a reason to trust. Probing whether a port answers is not a security
# decision, so the check is turned off for this script only.
[System.Net.ServicePointManager]::ServerCertificateValidationCallback = { $true }
[System.Net.ServicePointManager]::SecurityProtocol = [System.Net.SecurityProtocolType]::Tls12

$procs = Get-CimInstance Win32_Process -Filter "Name='$app.exe'"
$ids = @($procs | ForEach-Object { $_.ProcessId })
$ports = @()
if ($ids.Count -gt 0) {
    $ports = @(Get-NetTCPConnection -State Listen |
               Where-Object { $ids -contains $_.OwningProcess } |
               ForEach-Object { $_.LocalPort } | Sort-Object -Unique)
}
foreach ($g in @(48080, 48443, 18080, 18443, 80, 443)) { if ($ports -notcontains $g) { $ports += $g } }
# A build under test (MoonlightWeb.exe --dev) beside the installed edition:
# MW_BENCH_LOCAL_PORTS="18080,18443" puts its ports ahead of the edition's.
if ($env:MW_BENCH_LOCAL_PORTS) {
    $ports = @($env:MW_BENCH_LOCAL_PORTS -split ',' | ForEach-Object { [int]$_ }) + $ports
}
$out.listening = $ports | Select-Object -First 12

# Invoke-WebRequest's -TimeoutSec does not bound the TCP connect phase: against
# a port that is filtered rather than closed it can sit for minutes, and six
# candidate ports times two schemes turned a probe into a hang. A 300 ms socket
# test first means only a port that is actually open is ever asked twice.
function Test-Open($port) {
    $c = New-Object System.Net.Sockets.TcpClient
    try {
        if (-not $c.BeginConnect('127.0.0.1', $port, $null, $null).AsyncWaitHandle.WaitOne(300)) {
            return $false
        }
        return $c.Connected
    } catch { return $false } finally { $c.Close() }
}
function Try-Port($port, $scheme) {
    if (-not (Test-Open $port)) { return $null }
    try {
        $r = Invoke-WebRequest -UseBasicParsing -TimeoutSec 6 `
             -Uri "${scheme}://127.0.0.1:$port/api/native/status"
        if ($r.Content -match '^\s*\{') { return $r.Content }
    } catch { }
    return $null
}
$httpPort = 0; $httpsPort = 0
foreach ($p in $ports) {
    if ($httpPort  -eq 0 -and (Try-Port $p 'http'))  { $httpPort  = $p }
    if ($httpsPort -eq 0 -and (Try-Port $p 'https')) { $httpsPort = $p }
    if ($httpPort -ne 0 -and $httpsPort -ne 0) { break }
}
$out.httpPort = $httpPort; $out.httpsPort = $httpsPort

__READS_PS__
if ($httpPort -ne 0) {
    $found = Get-MwInstance $httpPort
    $out.localChannel = [bool]($found -and (Test-MwLocalRequest $found.Exe))
    $out.native   = Read-MwJson '/api/native/status' $httpPort
    $out.internet = (Get-MwReducedInternet (Read-MwObject '/api/internet/status' $httpPort)) | ConvertTo-Json -Compress -Depth 6
    $out.vdisplay = (Get-MwReducedVdisplay (Read-MwObject '/api/native/virtual-display' $httpPort)) | ConvertTo-Json -Compress -Depth 6
} else {
    $out.native = '{"error":"no port answered"}'
}

$task = Get-ScheduledTask -TaskName $app -ErrorAction SilentlyContinue
$out.autostart = if ($task) { $task.State.ToString() } else { 'absent' }
$out.running = if ($procs) { 'yes' } else { 'no' }

$log = Join-Path $env:APPDATA "MoonlightWeb\$app\logs\moonlightweb.log"
if (Test-Path $log) {
    # The last "Version:" line is the one the process running now printed at
    # start; the version shown in the UI comes from a CMake cache.
    $line = Select-String -Path $log -Pattern 'Version:' | Select-Object -Last 1
    $out.versionLine = if ($line) { $line.Line.Trim() } else { '' }
    $out.logPath = $log
} else { $out.versionLine = ''; $out.logPath = '' }

$exe = Join-Path $env:ProgramFiles "$app\$app.exe"
if (Test-Path $exe) {
    $out.exeSha = (Get-FileHash $exe -Algorithm SHA256).Hash.Substring(0, 16)
    $out.exePath = $exe
} else { $out.exeSha = ''; $out.exePath = '' }

'MWPROBE ' + ($out | ConvertTo-Json -Compress -Depth 4)
"""
        script = script.replace("__READS_PS__", _reads_ps())
        rc, out, err = run_script(mid, _for_edition(script, m["os"]), timeout=300)

    else:
        extra = ""
        if m["os"] == "macos":
            extra = r"""
p = subprocess.run(["launchctl", "print",
                    "gui/%d/com.moonlightweb.agent.dev" % os.getuid()],
                   capture_output=True, text=True)
out["agentLoaded"] = p.returncode == 0
# The app never calls `launchctl bootstrap` on itself — that would start a
# second copy beside the one already running. It writes the plist and launchd
# reads it at the NEXT login. So the thing that has to exist after an install
# is the file; "not loaded right now" is the normal state, not a failure.
plist = os.path.expanduser("~/Library/LaunchAgents/com.moonlightweb.agent.dev.plist")
out["agentPlist"] = os.path.exists(plist)
out["autostart"] = ("loaded" if out["agentLoaded"]
                    else ("login item" if out["agentPlist"] else "none"))
exe = "/Applications/MoonlightWebDev.app/Contents/MacOS/MoonlightWebDev"
logs = [os.path.expanduser("~/Library/Application Support/MoonlightWeb/MoonlightWebDev/logs/moonlightweb.log"),
        os.path.expanduser("~/Library/Logs/MoonlightWebDev/moonlightweb.log")]
"""
        else:
            extra = r"""
p = subprocess.run(["systemctl", "is-active", "moonlightweb-dev.service"],
                   capture_output=True, text=True)
out["service"] = (p.stdout or p.stderr).strip() or "absent"
d = subprocess.run(["systemctl", "get-default"], capture_output=True, text=True)
out["defaultTarget"] = d.stdout.strip()
# On a desktop distribution the packaging deliberately leaves the system unit
# disabled and starts the app inside the user's session instead
# (make-packages.sh: "postinst enables it only when the machine has no
# desktop"). What has to survive a reboot there is the XDG autostart entry, so
# THAT is the file to judge, not the unit.
desktop = os.path.expanduser("~/.config/autostart/moonlightweb-dev.desktop")
out["autostartDesktop"] = os.path.exists(desktop)
if out["defaultTarget"].startswith("graphical"):
    out["autostart"] = "session autostart" if out["autostartDesktop"] else "none"
else:
    out["autostart"] = out["service"]
exe = ""
for cand in ("/opt/moonlightweb-dev/bin/MoonlightWebDev",
             "/opt/moonlightweb-dev/MoonlightWebDev",
             "/opt/moonlightweb-dev/moonlightweb-dev"):
    if os.path.exists(cand):
        exe = cand
        break
logs = [os.path.expanduser("~/.local/share/MoonlightWeb/MoonlightWebDev/logs/moonlightweb.log"),
        "/var/log/moonlightweb-dev/moonlightweb.log"]
"""
        tail = r"""
q = subprocess.run(["pgrep", "-f", APP], capture_output=True, text=True)
out["running"] = "yes" if q.stdout.strip() else "no"
out["exePath"] = exe if exe and os.path.exists(exe) else ""
if out["exePath"]:
    h = hashlib.sha256()
    with open(exe, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    out["exeSha"] = h.hexdigest()[:16].upper()
else:
    out["exeSha"] = ""
line, found = "", ""
for log in logs:
    if os.path.exists(log):
        found = log
        # The log is appended to, never rotated per run: the LAST "Version:"
        # line is the one the process running right now printed at start. The
        # version string shown in the UI comes from a CMake cache and proves
        # nothing about which binary is actually up.
        with open(log, errors="replace") as f:
            for raw in f:
                if "Version:" in raw:
                    line = raw.strip()
        break
out["versionLine"] = line
out["logPath"] = found
print("MWPROBE " + json.dumps(out))
"""
        body = _probe_body_py().replace('print("MWPROBE " + json.dumps(out))', extra + tail)
        # caffeinate first: this Mac falls asleep in the middle of an ssh session.
        prefix = "caffeinate -d -i -t 300 >/dev/null 2>&1 </dev/null &\n" if m["os"] == "macos" else ""
        script = prefix + "python3 - MoonlightWebDev <<'MWPY'\n" + body + "\nMWPY\n"
        rc, out, err = run_script(mid, _for_edition(script, m["os"]), timeout=300)

    parsed = {"_rc": rc, "_stderr": redacted(err).strip()[:400], "_mid": mid}
    for chunk in (out or "").splitlines():
        chunk = chunk.strip()
        if chunk.startswith("MWPROBE "):
            try:
                parsed.update(json.loads(chunk[len("MWPROBE "):]))
            except ValueError:
                parsed["_raw"] = chunk[:400]
    for key in ("native", "internet", "vdisplay"):
        val = parsed.get(key)
        if isinstance(val, str):
            try:
                parsed[key] = json.loads(val)
            except ValueError:
                parsed[key] = {"error": val[:200]}
    return parsed


_SETTINGS_PS = r"""
$ErrorActionPreference = 'SilentlyContinue'
$port = __PORT__
$body = @'
__BODY__
'@
function Call($method, $path, $admin, $data) {
    $headers = @{}
    if ($admin) { $headers['X-MW-Admin-Key'] = $admin }
    $uri = "http://127.0.0.1:$port$path"
    try {
        if ($data) {
            $r = Invoke-WebRequest -UseBasicParsing -Method $method -TimeoutSec 20 -Headers $headers `
                 -ContentType 'application/json' -Body ([System.Text.Encoding]::UTF8.GetBytes($data)) -Uri $uri
        } else {
            $r = Invoke-WebRequest -UseBasicParsing -Method $method -TimeoutSec 20 -Headers $headers -Uri $uri
        }
        return $r.Content
    } catch { return '{"error":"' + ($_.Exception.Message -replace '"', '') + '"}' }
}
__READS_PS__
$found = Get-MwInstance $port
if ($found -and (Test-MwLocalRequest $found.Exe)) {
    # Through the instance's local channel: the address alone writes nothing.
    $b = Invoke-MwLocalRequest 'GET /api/settings/streaming' -Exe $found.Exe -Dev:$found.Dev
    $a = Invoke-MwLocalRequest 'POST /api/settings/streaming' -Exe $found.Exe -Dev:$found.Dev -Body $body
    $before = if ($b.Status -eq 200) { $b.Text } else { '{"error":"HTTP ' + $b.Status + '"}' }
    $after = if ($a.Status -eq 200) { $a.Text } else { '{"error":"HTTP ' + $a.Status + '"}' }
    'MWSETTINGS ' + (@{ before = $before; after = $after; admin = $true } | ConvertTo-Json -Compress -Depth 4)
} else {
    $admin = ''
    try { $admin = (Call GET '/api/admin/token' $null $null | ConvertFrom-Json).token } catch { }
    $before = Call GET '/api/settings/streaming' $admin $null
    $after = Call POST '/api/settings/streaming' $admin $body
    'MWSETTINGS ' + (@{ before = $before; after = $after; admin = [bool]$admin } | ConvertTo-Json -Compress -Depth 4)
}
"""

_SETTINGS_PY = r'''
import json, sys, urllib.request
port, body = int(sys.argv[1]), sys.argv[2]


def call(method, path, admin="", data=None):
    req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), method=method,
                                 data=data.encode("utf-8") if data is not None else None)
    if admin:
        req.add_header("X-MW-Admin-Key", admin)
    if data is not None:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=20) as r:
            return r.read().decode("utf-8", "replace")
    except Exception as e:
        return json.dumps({"error": str(e)[:200]})


found = instance(port)
if found and knows_local_request(found[0]):
    # Through the instance's local channel: the address alone writes nothing.
    try:
        st_b, raw_b = local_request("GET /api/settings/streaming", exe=found[0], dev=found[1])
        st_a, raw_a = local_request("POST /api/settings/streaming", exe=found[0], dev=found[1],
                                    body=body.encode("utf-8"))
    except LocalRequestError:
        st_b = st_a = 0
        raw_b = raw_a = b""
    before = raw_b.decode("utf-8", "replace") if st_b == 200 else json.dumps({"error": "HTTP %d" % st_b})
    after = raw_a.decode("utf-8", "replace") if st_a == 200 else json.dumps({"error": "HTTP %d" % st_a})
    print("MWSETTINGS " + json.dumps({"before": before, "after": after, "admin": True}))
else:
    admin = ""
    try:
        admin = json.loads(call("GET", "/api/admin/token")).get("token", "")
    except Exception:
        pass
    before = call("GET", "/api/settings/streaming", admin)
    after = call("POST", "/api/settings/streaming", admin, body)
    print("MWSETTINGS " + json.dumps({"before": before, "after": after, "admin": bool(admin)}))
'''


def host_settings(mid, http_port, settings):
    """Write streaming settings on the HOST, through its own loopback API.

    Some settings live on the host and not in the browser: the native engine's
    video pipeline (native_video_pipeline) is one, the latency flag another.
    drive.apply_settings() only reaches the client's localStorage, so a pass
    that changes one of these declares it as `hostSettings`, and run.py puts
    the previous values back after the pass with a second call to this.

    Returns the values the given keys had before — only those the host
    reported, so a key it did not know is not "restored" to something it never
    was. Raises RuntimeError when the host refused the change: a pass run with
    a setting silently not applied would measure the wrong thing.
    """
    if not settings:
        return {}
    if not http_port:
        raise RuntimeError("no HTTP port known for %s: cannot reach its settings" % mid)
    body = json.dumps(settings)
    m = MACHINES[mid]
    if m["os"] == "windows":
        script = _SETTINGS_PS.replace("__PORT__", str(int(http_port))).replace(
            "__BODY__", body).replace("__READS_PS__", _reads_ps())
    else:
        script = "python3 - %d %s <<'MWPY'\n%s\nMWPY\n" % (
            int(http_port), shlex.quote(body), _mw_local_py() + "\n" + _SETTINGS_PY)
    rc, out, err = run_script(mid, script, timeout=120)
    result = None
    for line in (out or "").splitlines():
        line = line.strip()
        if line.startswith("MWSETTINGS "):
            try:
                result = json.loads(line[len("MWSETTINGS "):])
            except ValueError:
                pass
    if not result:
        raise RuntimeError("no answer from %s's settings API (rc %s): %s" % (
            mid, rc, (err or out or "").strip()[:200]))
    try:
        after = json.loads(result.get("after") or "{}")
    except ValueError:
        after = {"error": (result.get("after") or "")[:200]}
    if isinstance(after, dict) and after.get("error"):
        raise RuntimeError("%s refused the host settings %s: %s" % (mid, body, after["error"]))
    try:
        before = json.loads(result.get("before") or "{}")
    except ValueError:
        before = {}
    return {k: before[k] for k in settings if isinstance(before, dict) and k in before}


def https_port_from_here(address, candidates, timeout=4):
    """Which port serves the web UI over TLS, asked from the measuring station.

    Asked from HERE and not from the host, on purpose: this is the port the
    bench client will dial, so the thing worth testing is whether this machine
    can reach it — a host-side answer would still leave a closed firewall
    undiscovered. The certificate is the machine's own and is not checked; the
    question is which port answers, not whether to trust it.
    """
    import http.client
    import ssl
    ctx = ssl.create_default_context()
    ctx.check_hostname = False
    ctx.verify_mode = ssl.CERT_NONE
    for port in candidates:
        try:
            conn = http.client.HTTPSConnection(address, port, timeout=timeout, context=ctx)
            conn.request("GET", "/api/health")
            resp = conn.getresponse()
            resp.read(64)
            conn.close()
            # Any HTTP answer proves a web server is there; 404 counts.
            if resp.status:
                return port
        except Exception:
            continue
    return 0


def lan_url(mid, probe_result):
    """The address this machine answers on, with the port it actually listens on.

    48443 is only the default. A settings.json decides, a silent uninstall
    keeps that settings.json, and a fresh install inherits it — the dev
    instance on this desk answers on 18443.
    """
    probe_result = probe_result or {}
    address = MACHINES[mid]["address"]
    candidates = [p for p in (probe_result.get("listening") or []) if p != probe_result.get("httpPort")]
    for guess in (DEV_HTTPS, 18443, 443):
        if guess not in candidates:
            candidates.append(guess)
    port = probe_result.get("httpsPort") or https_port_from_here(address, candidates) or DEV_HTTPS
    return "https://%s:%d/" % (address, port)


def rendezvous_url(probe_result):
    """The address an owner reconnects on, asked of the machine the probe came from
    (_secret): in this process's memory only, never in a record. "" when the probe
    saw no rendezvous address."""
    probe_result = probe_result or {}
    mid = probe_result.get("_mid")
    if not mid or not (probe_result.get("internet") or {}).get("rendezvousKnown"):
        return ""
    key = (mid, probe_result.get("httpPort"))
    if not _SECRETS.get(key):
        _SECRETS[key] = _secret(mid, "rdv", probe_result.get("httpPort"))
    return _SECRETS[key]


def rendezvous_host(probe_result):
    """The rendezvous server's host name, which a record may hold."""
    return ((probe_result or {}).get("internet") or {}).get("rendezvousHost") or ""


def wait_up(mid, timeout=180):
    """Poll the machine's own loopback until the server answers, or give up."""
    end = time.time() + timeout
    while time.time() < end:
        got = probe(mid)
        if isinstance(got.get("native"), dict) and "error" not in got["native"]:
            return got
        time.sleep(10)
    return probe(mid)
