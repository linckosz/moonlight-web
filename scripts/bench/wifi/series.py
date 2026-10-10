"""Series of click-to-flag passes against DualRTX's native host, one client at a
time (plan « Wi-Fi : la vidéo qui attend dans SCTP », born of T7 of the radios
plan, 03/10/2026).

    python series.py mac --contents clk,g80 --rounds 2 --prefix w0 [--udp]
    python series.py n95 --contents clk --tuning sctpcc=3 --prefix w2a
    python report.py w0                      # the table of what the passes gave

One phase per client: its Chrome up and its DevTools port tunnelled here; the
pairing a bench Chrome kept for this host on another port cleared; then, for
each round and each content (alternated: clk, g80, clk, g80…), one
`content-age/local_matrix.py` run, which kills and relaunches the `--dev`
instance; the client down at the end. A physical screen of DualRTX leaving the
desktop stops the phase (memory dualrtx-vdd-display-tests).

Each pass gives the content's age (the band of scroll.html, read by the probe),
then, the probe stopped, `--clicks` click → flag samples split by the host's
answer to their stamp (way up, injection, rest), then the way up alone
(`--uplink`). `--udp` runs a bare UDP ping to the client beside every pass
(udp_ref.py; the echo is started on the Mac by this script).

The outputs go to bench-out/content-age (the passes) and bench-out/wifi (this
series' log, each matrix's output, the UDP pings).
"""
import argparse
import datetime
import json
import os
import shutil
import subprocess
import sys
import threading
import time
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
CA = os.path.join(REPO, "scripts", "bench", "content-age")
OUT = os.path.join(REPO, "bench-out", "wifi")
LOG = os.path.join(OUT, "series.log")
sys.path.insert(0, HERE)
import udp_ref  # noqa: E402
import remote_host  # noqa: E402

TUNING = ""
# --host: a native host on another machine (remote_host.py), its passes run by
# pass.py --host instead of local_matrix.py.
HOST = None
CA_OUT = os.path.join(REPO, "bench-out", "content-age")
EXE = os.path.join(REPO, "build", "MoonlightWeb.exe")
HOST_IP = "192.168.1.66"
# The --dev instance's own ports (8080/8443 since 03/10/2026, 18080/18443 before).
DEV_PORTS = "8080,8443"
HOST_URL = "https://%s:8443/" % HOST_IP
GPUS = {"arc": "Intel(R) Arc(TM) A380 Graphics",
        "rtx": "NVIDIA GeForce RTX 5060 Ti",
        "amd": "AMD Radeon(TM) Graphics"}
# Each content: clicks split by the host's stamp, then the way up alone.
CONTENTS = {
    # the page at the display's rate (240 on the virtual display), Auto with detection
    "clk": ["--clicks", "60", "--uplink", "50:20"],
    # the page at a game's rate
    "g80": ["--game-fps", "75-83", "--clicks", "60", "--uplink", "50:20"],
    # a still desktop: almost no video coming down
    "still": ["--hold", "20", "--clicks", "40", "--uplink", "50:20"],
    # the page streamed at 60 fps
    "f60": ["--fps", "60", "--clicks", "40", "--uplink", "50:20"],
}
SSH = shutil.which("ssh") or "ssh"
NOWIN = getattr(subprocess, "CREATE_NO_WINDOW", 0)
WSL_SSH = ["wsl.exe", "-u", "root", "--", "sshpass", "-p", "123456", "ssh",
           "-o", "StrictHostKeyChecking=no", "-o", "UserKnownHostsFile=/dev/null",
           "-o", "LogLevel=ERROR", "-o", "ConnectTimeout=8"]
TUN_OPTS = ["-N", "-o", "ExitOnForwardFailure=yes", "-o", "ServerAliveInterval=30"]
UDP_PORT = 47998
SINK_PORT = 47999


def log(*a):
    line = datetime.datetime.now().strftime("%H:%M:%S ") + " ".join(str(x) for x in a)
    print(line, flush=True)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line + "\n")


def run(cmd, timeout=120, stdin_text=None):
    # Bytes, not text: a text-mode pipe on Windows turns "\n" into "\r\n",
    # and the remote bash then reads a broken script.
    try:
        kw = {"input": stdin_text.encode("utf-8")} if stdin_text is not None else {
            "stdin": subprocess.DEVNULL}
        r = subprocess.run(cmd, capture_output=True, timeout=timeout, creationflags=NOWIN, **kw)
        out = (r.stdout or b"").decode("utf-8", "replace") + (r.stderr or b"").decode(
            "utf-8", "replace")
    except subprocess.TimeoutExpired:
        out = "(timed out after %d s)" % timeout
    return "\n".join(l for l in out.splitlines()
                     if not any(k in l for k in ("post-quantum", "store now", "upgraded",
                                                 "openssh.com/pq", "#< CLIXML", "<Objs")))


def cdp_up(port, timeout=30):
    end = time.time() + timeout
    while time.time() < end:
        try:
            with urllib.request.urlopen("http://127.0.0.1:%d/json/version" % port, timeout=2) as r:
                return json.load(r).get("Browser")
        except Exception:
            time.sleep(1)
    return None


def monitors():
    out = subprocess.run(["powershell", "-NoProfile", "-File",
                          os.path.join(REPO, "scripts", "bench", "monitors.ps1")],
                         capture_output=True, text=True, creationflags=NOWIN).stdout
    return [l.strip() for l in out.splitlines() if l.strip()]


def screen_names(lines):
    """The physical screens: the virtual displays come and go with each pass
    and have numbers above 99 (\\\\.\\DISPLAY335 and on)."""
    names = set()
    for l in lines:
        n = l.split()[0]
        digits = n.rsplit("DISPLAY", 1)[-1]
        if digits.isdigit() and int(digits) < 100:
            names.add(n)
    return names


class Client:
    name = ""
    port = 0
    secs = 20
    ip = ""
    # One decoded frame in four read by the probe: its copy, at every frame,
    # weighs most on the fastest modes it compares (02/10/2026). The frames it
    # reads wait ~12 ms more before their draw (U0.2 cross-check, 03/10).
    every = 4

    def __init__(self):
        self.tun = None

    def tunnel_cmd(self):
        raise NotImplementedError

    def chrome_up(self):
        raise NotImplementedError

    def chrome_down(self):
        return ""

    def udp_echo(self, on):
        """The UDP echo udp_ref.py pings, started and stopped on the client."""
        return "no UDP echo on this client"

    def udp_sink(self, on, rcvbuf_kb=4096):
        """The UDP sink udp_ref.py's bursts count on, started and stopped."""
        return "no UDP sink on this client"

    def udp_drops(self):
        """Datagrams the client's kernel dropped for a full socket buffer since
        it booted (W1 bis), or None where it cannot be read."""
        return None

    def start(self):
        log(self.name, "chrome up:", self.chrome_up()[-300:].replace("\n", " | "))
        self.open_tunnel()
        browser = cdp_up(self.port)
        log(self.name, "CDP on", self.port, ":", browser)
        return browser is not None

    def open_tunnel(self):
        self.close_tunnel()
        self.tun = subprocess.Popen(self.tunnel_cmd(), stdin=subprocess.DEVNULL,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                    creationflags=NOWIN)
        time.sleep(3)

    def close_tunnel(self):
        if self.tun and self.tun.poll() is None:
            self.tun.terminate()
            try:
                self.tun.wait(5)
            except Exception:
                self.tun.kill()
        self.tun = None
        # One left by an earlier run of this script, on the same local port.
        subprocess.run(["powershell", "-NoProfile", "-Command",
                        "Get-CimInstance Win32_Process -Filter \"Name='ssh.exe'\" | Where-Object "
                        "{ $_.CommandLine -like '*-L*%d:127.0.0.1:*' } | ForEach-Object "
                        "{ Stop-Process -Id $_.ProcessId -Force }" % self.port],
                       capture_output=True, creationflags=NOWIN)

    def ensure(self):
        if cdp_up(self.port, timeout=5):
            return True
        log(self.name, "CDP gone: tunnel reopened")
        self.open_tunnel()
        if cdp_up(self.port, timeout=10):
            return True
        log(self.name, "still gone: its Chrome restarted")
        return self.start()

    def stop(self):
        log(self.name, "chrome down:", self.chrome_down()[-200:].replace("\n", " | "))
        self.close_tunnel()

    def client_args(self):
        return ["--client-port", str(self.port), "--client-url", HOST_URL]

    def env(self):
        return {}


class Mac(Client):
    name, port, ip = "mac", 9422, "192.168.1.34"

    def tunnel_cmd(self):
        return [SSH] + TUN_OPTS + ["-L", "9422:127.0.0.1:9222", "mw-mac"]

    # The scripts are not executable on the Mac: through bash.
    def chrome_up(self):
        return run([SSH, "mw-mac", "bash ~/mw-c925/mac-chrome.sh"], timeout=90)

    def chrome_down(self):
        return run([SSH, "mw-mac", "bash ~/mw-c925/mac-chrome.sh stop"], timeout=60)

    def udp_echo(self, on):
        if not on:
            return run([SSH, "mw-mac", "pkill -f 'udp_ref.py echo'; echo echo stopped"], timeout=30)
        with open(os.path.join(HERE, "udp_ref.py"), "rb") as f:
            src = f.read().decode("utf-8")
        return run([SSH, "mw-mac", "cat > ~/mw-c925/udp_ref.py; pkill -f 'udp_ref.py echo'; "
                    "nohup python3 ~/mw-c925/udp_ref.py echo %d > ~/mw-c925/udp_echo.log 2>&1 & "
                    "sleep 1; cat ~/mw-c925/udp_echo.log" % UDP_PORT], timeout=30, stdin_text=src)

    def udp_sink(self, on, rcvbuf_kb=4096):
        if not on:
            return run([SSH, "mw-mac", "pkill -f 'udp_ref.py sink'; echo sink stopped"], timeout=30)
        with open(os.path.join(HERE, "udp_ref.py"), "rb") as f:
            src = f.read().decode("utf-8")
        return run([SSH, "mw-mac", "cat > ~/mw-c925/udp_ref.py; pkill -f 'udp_ref.py sink'; "
                    "nohup python3 ~/mw-c925/udp_ref.py sink %d %d > ~/mw-c925/udp_sink.log 2>&1 & "
                    "sleep 1; cat ~/mw-c925/udp_sink.log" % (SINK_PORT, rcvbuf_kb)], timeout=30,
                   stdin_text=src)

    def udp_drops(self):
        out = run([SSH, "mw-mac", "netstat -s -p udp | grep 'full socket buffers'"], timeout=30)
        digits = out.strip().split()
        return int(digits[0]) if digits and digits[0].isdigit() else None


class N95(Client):
    name, port, secs, every, ip = "n95", 9423, 30, 10, ""
    PS = r"powershell -NoProfile -File C:\Users\Public\mw-run\ua-n95-chrome.ps1"

    def tunnel_cmd(self):
        return [SSH] + TUN_OPTS + ["-L", "9423:127.0.0.1:9232", "mw-intel"]

    def chrome_up(self):
        return run([SSH, "mw-intel", self.PS + " < NUL"], timeout=120)

    def chrome_down(self):
        return run([SSH, "mw-intel", self.PS + " -Stop < NUL"], timeout=60)


class UmWin(Client):
    name, port = "um", 9424
    # MW_UM_IP: the UM790Pro reached at another address than the alias' cable one
    # (192.168.1.9), its Wi-Fi when the cable is off (10/10/2026). The host key
    # stays the Windows one: the cable's IP shares known_hosts with the Ubuntu.
    ALT = os.environ.get("MW_UM_IP", "")
    SSHU = [SSH] + (["-o", "HostName=" + ALT, "-o", "HostKeyAlias=um790-windows"]
                    if ALT else []) + ["mw-um790win"]

    def tunnel_cmd(self):
        return self.SSHU[:1] + TUN_OPTS + ["-L", "9424:127.0.0.1:9222"] + self.SSHU[1:]

    def chrome_up(self):
        return run(self.SSHU + [r"& 'C:\Users\minis\um790-chrome.ps1'"], timeout=120)

    def chrome_down(self):
        return run(self.SSHU + [r"& 'C:\Users\minis\um790-chrome.ps1' -Stop"], timeout=60)

    def udp_drops(self):
        # Windows has no "full socket buffers" line: the closest is the UDP
        # "Receive Errors" of both families (the pair is often IPv6). The N95
        # never counted one (W1 bis); kept to see whether this card does.
        out = run(self.SSHU + ["netstat -s -p udp; netstat -s -p udpv6"], timeout=30)
        counts = [int(l.split("=")[-1].strip()) for l in out.splitlines()
                  if "Receive Errors" in l and l.split("=")[-1].strip().isdigit()]
        return sum(counts) if counts else None


LX_UP = r"""export XDG_RUNTIME_DIR=/run/user/1000 WAYLAND_DISPLAY=wayland-0 DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/1000/bus
P=/home/bruno/.mw-inet-chrome
pkill -f "user-data-dir=$P" 2>/dev/null
sleep 2
mkdir -p /home/bruno/mw-inet
setsid -f google-chrome-stable --user-data-dir=$P --no-first-run --no-default-browser-check \
    --ozone-platform=wayland --ignore-certificate-errors --remote-debugging-port=9222 \
    --start-maximized --autoplay-policy=no-user-gesture-required --disable-infobars \
    --disable-search-engine-choice-screen --disable-sync --password-store=basic \
    --disable-features=CalculateNativeWinOcclusion,LocalNetworkAccessChecks,LocalNetworkAccessChecksWebSockets,LocalNetworkAccessChecksWebRTC \
    --disable-backgrounding-occluded-windows --disable-renderer-backgrounding \
    about:blank >/home/bruno/mw-inet/chrome-ua.log 2>&1
for i in $(seq 1 40); do curl -s --max-time 1 http://127.0.0.1:9222/json/version >/dev/null && break; sleep 0.5; done
curl -s --max-time 2 http://127.0.0.1:9222/json/version | grep '"Browser"' || echo "no CDP on 9222"
"""
LX_DOWN = r"""pkill -f "user-data-dir=/home/bruno/.mw-inet-chrome" 2>/dev/null; sleep 1; echo "bench Chrome stopped"
"""


class UmLinux(Client):
    name, port = "lx", 9425

    def tunnel_cmd(self):
        return WSL_SSH + TUN_OPTS + ["-L", "127.0.0.1:9425:127.0.0.1:9222", "bruno@192.168.1.9"]

    def chrome_up(self):
        return run(WSL_SSH + ["bruno@192.168.1.9", "bash -s"], timeout=90, stdin_text=LX_UP)

    def chrome_down(self):
        return run(WSL_SSH + ["bruno@192.168.1.9", "bash -s"], timeout=60, stdin_text=LX_DOWN)

    def close_tunnel(self):
        super().close_tunnel()
        run(["wsl.exe", "-u", "root", "--", "pkill", "-f", "127.0.0.1:9425:127.0.0.1:9222"],
            timeout=20)


class Local(Client):
    """Chrome on this machine, on the AMD's screen (pass.py's own kiosk)."""
    name = "loc"

    def __init__(self):
        super().__init__()
        self.pos = ""

    def start(self):
        for l in monitors():
            if l.startswith("\\\\.\\DISPLAY9 "):
                self.pos = l.split()[1]
        log("loc: client on DISPLAY9 at", self.pos or "?")
        return bool(self.pos)

    def ensure(self):
        return True

    def stop(self):
        pass

    def client_args(self):
        return []

    def env(self):
        # The TV session's Chrome holds 9333, the kiosk's usual port.
        return {"MW_BENCH_CLIENT_POS": self.pos, "MW_BENCH_DEBUG_PORT": "9353"}


def summarize(series, prefix):
    out = subprocess.run([sys.executable, os.path.join(HERE, "report.py"), series, "--passes",
                          "--only", prefix],
                         capture_output=True, text=True, creationflags=NOWIN)
    for l in (out.stdout or out.stderr).splitlines():
        log("   |", l)


def matrix(client, series, prefix, gpu, cadences, extra, udp):
    cmd = [sys.executable, os.path.join(CA, "local_matrix.py"), "--prefix", prefix,
           "--rates", "0", "--cadences", cadences, "--repeat", "1", "--settle", "14",
           "--secs", str(client.secs), "--every", str(client.every),
           "--vdd-gpu", GPUS[gpu], "--exe", EXE] + (["--tuning", TUNING] if TUNING else []) + \
        client.client_args() + extra + (
            ["--bitrate", str(client.bitrate)] if getattr(client, "bitrate", 0) > 0 else [])
    env = dict(os.environ)
    env["MW_BENCH_LOCAL_PORTS"] = DEV_PORTS
    env["PYTHONIOENCODING"] = "utf-8"
    env.update(client.env())
    t0 = time.time()
    log("run", prefix, "|", cadences, " ".join(extra), "|", TUNING or "no host key")
    drops = client.udp_drops()
    stop = threading.Event()
    pinger = None
    if udp and client.ip:
        # 20 a second for the whole matrix: each sample keeps its wall-clock
        # time, to be set against the pass's clicks afterwards.
        pinger = threading.Thread(target=udp_ref.ping, kwargs=dict(
            host=client.ip, port=UDP_PORT, hz=20, secs=3 * 3600,
            out=os.path.join(OUT, prefix + ".udp.json"), stop=stop.is_set), daemon=True)
        pinger.start()
    path = os.path.join(OUT, prefix + ".out")
    with open(path, "w", encoding="utf-8") as f:
        try:
            p = subprocess.run(cmd, cwd=CA, env=env, stdout=f, stderr=subprocess.STDOUT,
                               creationflags=NOWIN, timeout=60 * 60)
            rc = p.returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
    if pinger:
        stop.set()
        pinger.join(10)
    after = client.udp_drops()
    if drops is not None and after is not None:
        # W1 bis: the datagrams the client's kernel threw away for a full
        # socket buffer during the pass — the browser's, mostly.
        log(client.name, "kernel drops for a full socket buffer during", prefix, ":", after - drops)
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    saved = text.count("\n   saved ")
    alerts = [l.strip() for l in text.splitlines()
              if "!!" in l or "Traceback" in l or "Error" in l or "SystemExit" in l]
    log("done", prefix, "rc", rc, "| %d passes saved" % saved,
        "| %.1f min" % ((time.time() - t0) / 60))
    for a in alerts[:6]:
        log("   !", a[:200])
    summarize(series, prefix)


RTX_SCREEN = "\\\\.\\DISPLAY5"


def bursts(client, prefix, spec, when):
    """The radio alone, no stream: video-shaped UDP bursts to the client, lost
    or overtaken (udp_ref.py burst; plan W1). @p spec is MBPS:FPS:SECS[:RCVBUF_KB][,…]: a
    receive buffer named for one burst restarts the sink with it (W1 bis)."""
    sink_buf = 4096
    log(client.name, "udp sink:", client.udp_sink(True)[-80:].replace("\n", " | "))
    try:
        for k, part in enumerate(spec.split(",")):
            fields = part.split(":")
            mbps, fps, secs = (float(x) for x in fields[:3])
            buf = int(fields[3]) if len(fields) > 3 else 4096
            if buf != sink_buf:
                sink_buf = buf
                log(client.name, "udp sink (%d KB):" % buf,
                    client.udp_sink(True, buf)[-60:].replace("\n", " | "))
            drops = client.udp_drops()
            out = os.path.join(OUT, "%s-%s-burst-%s-%d.json" % (prefix, client.name, when, k))
            rep = udp_ref.burst(client.ip, SINK_PORT, mbps, fps, secs, out)
            after = client.udp_drops()
            log(client.name, "burst %s %s Mbit/s at %s fps, sink buffer %d KB: %s sent, %s lost, "
                "%s late (max %s datagrams, %s ms); kernel drops for a full buffer %s" % (
                    when, mbps, fps, buf, rep.get("sent"), rep.get("lost"), rep.get("late"),
                    rep.get("lateByMax"), rep.get("lateMsMax"),
                    (after - drops) if drops is not None and after is not None else "?"))
    finally:
        log(client.name, "udp sink:", client.udp_sink(False)[-60:].replace("\n", " | "))


def remote_matrix(client, series, prefix, extra, udp):
    """One pass against a host on another machine (--host): its DEV edition's
    native_tuning set, pass.py run against it, its log and the relay's frame
    log fetched back under the pass's name — what local_matrix.py does for
    the --dev on this machine (plan Wi-Fi, B and W2.5 on Linux and macOS)."""
    tag = "%s-v0-detect-r0" % prefix
    log("run", prefix, "| host", HOST.mid, "|", TUNING or "no host key")
    log(HOST.tag, HOST.set_tuning(TUNING)[-200:])
    offset = HOST.log_size()
    since = time.time()
    args = client.client_args()
    if "--client-url" in args:
        i = args.index("--client-url")
        args = args[:i] + args[i + 2:]
    cmd = [sys.executable, os.path.join(CA, "pass.py"), "--host", HOST.mid, "--tag", tag,
           "--autostep", "--settle", "14", "--secs", str(client.secs), "--every",
           str(client.every)] + args + extra
    env = dict(os.environ)
    env["PYTHONIOENCODING"] = "utf-8"
    env.update(client.env())
    drops = client.udp_drops()
    stop = threading.Event()
    pinger = None
    if udp and client.ip:
        pinger = threading.Thread(target=udp_ref.ping, kwargs=dict(
            host=client.ip, port=UDP_PORT, hz=20, secs=3 * 3600,
            out=os.path.join(OUT, prefix + ".udp.json"), stop=stop.is_set), daemon=True)
        pinger.start()
    path = os.path.join(OUT, prefix + ".out")
    t0 = time.time()
    with open(path, "w", encoding="utf-8") as f:
        try:
            p = subprocess.run(cmd, cwd=CA, env=env, stdout=f, stderr=subprocess.STDOUT,
                               creationflags=NOWIN, timeout=30 * 60)
            rc = p.returncode
        except subprocess.TimeoutExpired:
            rc = "timeout"
    if pinger:
        stop.set()
        pinger.join(10)
    after = client.udp_drops()
    if drops is not None and after is not None:
        log(client.name, "kernel drops for a full socket buffer during", prefix, ":", after - drops)
    # The host's side, named as local_matrix.py names it: report.py and
    # flagpath.py read <tag>.server.log and <tag>.relay.csv.
    n = HOST.fetch_log(offset, os.path.join(CA_OUT, tag + ".server.log"))
    relay = HOST.fetch_relay_csv(since, os.path.join(CA_OUT, tag + ".relay.csv"))
    log(HOST.tag, "log %d bytes, relay log %s" % (n, "fetched" if relay else "none"))
    if HOST.fetch_click_trace(since, os.path.join(CA_OUT, tag + ".click-trace.csv")):
        log(HOST.tag, "click trace fetched")
    with open(path, encoding="utf-8", errors="replace") as f:
        text = f.read()
    alerts = [l.strip() for l in text.splitlines()
              if "!!" in l or "Traceback" in l or "Error" in l or "SystemExit" in l]
    log("done", prefix, "rc", rc, "| %.1f min" % ((time.time() - t0) / 60))
    for a in alerts[:6]:
        log("   !", a[:200])
    summarize(series, prefix)


def phase(client, prefix, hosts, contents, rounds, cadences, udp, burst=""):
    start_screens = screen_names(monitors())
    noted = set()
    log("== phase", client.name, "hosts", hosts, "contents", contents, "x%d" % rounds,
        "| screens", sorted(start_screens))
    if not client.start():
        log("client", client.name, "did not come up: phase skipped")
        client.stop()
        return 2
    # A pairing left for this host on another port: its cookie rides to the
    # new port too and the relay refuses it (pairing verification failed, and
    # the stream falls back to the WebSocket). Cleared before the first pass.
    if client.port:
        for origin in ("https://%s:8443" % HOST_IP, "https://%s:18443" % HOST_IP):
            log(client.name, "clear", origin, run(
                ["node", os.path.join(HERE, "cdpcall.mjs"), str(client.port),
                 "Storage.clearDataForOrigin",
                 json.dumps({"origin": origin, "storageTypes": "all"})])[-80:])
        log(client.name, "cookies", run(["node", os.path.join(HERE, "cdpcall.mjs"),
                                         str(client.port), "Network.clearBrowserCookies",
                                         "{}"])[-80:])
    if udp:
        log(client.name, "udp echo:", client.udp_echo(True)[-120:].replace("\n", " | "))
    if burst and client.ip:
        bursts(client, prefix, burst, "before")
    try:
        for r in range(1, rounds + 1):
            for g in hosts:
                for c in contents:
                    if not client.ensure():
                        log("client", client.name, "lost: phase stopped")
                        return 3
                    name = "%s-%s-%s-%s-%d" % (prefix, client.name, g, c, r)
                    if HOST:
                        remote_matrix(client, prefix, name, CONTENTS[c], udp)
                    else:
                        matrix(client, prefix, name, g, cadences, CONTENTS[c], udp)
                    now = monitors()
                    gone = start_screens - screen_names(now)
                    # The RTX's screen has left the desktop on these switches
                    # before (01/10 15:44): said, and the series goes on.
                    # Bruno's two other screens leaving stops it.
                    if RTX_SCREEN in gone and RTX_SCREEN not in noted:
                        log("!! the RTX's screen (DISPLAY5) left the desktop | now:", now,
                            "| the series goes on")
                        noted.add(RTX_SCREEN)
                    gone.discard(RTX_SCREEN)
                    if gone:
                        log("!! physical screen(s) gone from the desktop:", sorted(gone),
                            "| now:", now, "| phase stopped")
                        return 4
    finally:
        if udp:
            log(client.name, "udp echo:", client.udp_echo(False)[-80:].replace("\n", " | "))
        client.stop()
    log("== phase", client.name, "finished")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("phase", choices=["lx", "mac", "um", "n95", "loc"])
    ap.add_argument("--prefix", default="wifi", help="the passes' names: <prefix>-<client>-…")
    ap.add_argument("--hosts", default="arc", help="the GPUs rendering the virtual display")
    ap.add_argument("--contents", default="clk,g80", help=",".join(CONTENTS))
    ap.add_argument("--rounds", type=int, default=1,
                    help="the contents run that many times, alternated")
    ap.add_argument("--cadences", default="detect")
    ap.add_argument("--name", default="", help="the client's name in the tags (default: the phase)")
    ap.add_argument("--bitrate", type=int, default=0, help="kbps; 0 = the automatic one")
    ap.add_argument("--tuning", default="", help="host keys for every pass (local_matrix --tuning)")
    ap.add_argument("--codec", default="", help="h264 | hevc | av1 for every pass (pass.py --codec); "
                    "the video's road comes from MW_RTP_VIDEO in this script's environment")
    ap.add_argument("--local-storage", action="append", default=[], metavar="KEY=VALUE",
                    help="set in the client page before every pass (pass.py --local-storage)")
    ap.add_argument("--host", default="",
                    help="a native host on another machine (um790pro, mw-mac): its DEV "
                         "edition's native_tuning set to --tuning, pass.py --host against it")
    ap.add_argument("--exe", default="", help="the build under test (default build/)")
    ap.add_argument("--udp", action="store_true",
                    help="a bare UDP ping to the client beside each pass (the Mac only)")
    ap.add_argument("--burst", default="",
                    help="MBPS:FPS:SECS[,...] video-shaped UDP bursts to the client before the "
                         "passes, no stream: lost or overtaken (the Mac only)")
    a = ap.parse_args()
    os.makedirs(OUT, exist_ok=True)
    client = {"lx": UmLinux, "mac": Mac, "um": UmWin, "n95": N95, "loc": Local}[a.phase]()
    if a.name:
        client.name = a.name
    client.bitrate = a.bitrate
    global TUNING, EXE, HOST
    TUNING = a.tuning
    hosts = a.hosts.split(",")
    if a.host:
        HOST = remote_host.for_machine(a.host)
        hosts = [HOST.tag]
    if a.exe:
        EXE = os.path.abspath(a.exe)
    contents = [c for c in a.contents.split(",") if c in CONTENTS]
    if a.codec:
        for c in contents:
            CONTENTS[c] = CONTENTS[c] + ["--codec", a.codec]
    for kv in a.local_storage:
        for c in contents:
            CONTENTS[c] = CONTENTS[c] + ["--local-storage", kv]
    return phase(client, a.prefix, hosts, contents, a.rounds, a.cadences, a.udp, a.burst)


if __name__ == "__main__":
    sys.exit(main())
