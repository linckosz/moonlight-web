"""The acceptance run: install on the fleet, then stream one factor at a time.

    python run.py --plan-only                 print both matrices, run nothing
    python run.py --chapter 1 --package <dir> install and update everywhere
    python run.py --chapter 2                 DualRTX host, DualRTX client
    python run.py --chapter 3                 every other host, DualRTX client
    python run.py --only mw-mac               restrict to one machine
    python run.py --pass default              restrict to one pass
    python run.py --pass "codec-load-*"       ...or to the passes a pattern matches

Results are appended to bench-out/acceptance/results/, screenshots land under
bench-out/acceptance/screens/<machine>/<chapter>/<pass>.png, and report.py
turns the two into one HTML page. Nothing here is ever committed.
"""
import argparse
import fnmatch
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
OUT = os.path.join(REPO, "bench-out", "acceptance")
RESULTS = os.path.join(OUT, "results")
SCREENS = os.path.join(OUT, "screens")

sys.path.insert(0, HERE)
import drive  # noqa: E402
import fleet  # noqa: E402
import gpu_load  # noqa: E402
import install as installer  # noqa: E402

CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"
PROFILE = os.path.join(os.path.dirname(HERE), ".chrome-client")
DEBUG_PORT = 9333


def load_matrix():
    with open(os.path.join(HERE, "matrix.json"), encoding="utf-8") as f:
        return json.load(f)


def append(name, obj):
    os.makedirs(RESULTS, exist_ok=True)
    with open(os.path.join(RESULTS, name), "a", encoding="utf-8") as f:
        f.write(json.dumps(obj, ensure_ascii=False) + "\n")


# ── the measuring browser ───────────────────────────────────────────────────

def client_position():
    """Where the client window opens: never on the screen being captured.

    At 0,0 on a host that streams its primary screen, the client shows up in
    its own stream — a hall of mirrors whose content changes with the stream's
    own frame rate, so a slower encoder is handed a calmer picture and every
    size and cadence figure is skewed (22/09/2026). MW_BENCH_CLIENT_POS takes
    "x,y", or "secondary" for the top-left corner of the first screen that is
    not the primary one. Unset keeps 0,0 for a remote host, where it is right.
    """
    pos = os.environ.get("MW_BENCH_CLIENT_POS", "").strip()
    if not pos:
        return 0, 0
    if pos.lower() != "secondary":
        x, y = pos.split(",")
        return int(x), int(y)
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Add-Type -AssemblyName System.Windows.Forms; "
                          "[System.Windows.Forms.Screen]::AllScreens | "
                          "Where-Object { -not $_.Primary } | Select-Object -First 1 | "
                          "ForEach-Object { '{0},{1}' -f $_.Bounds.X, $_.Bounds.Y }"],
                         capture_output=True, text=True).stdout.strip()
    if not out:
        raise SystemExit("MW_BENCH_CLIENT_POS=secondary but there is only one screen")
    x, y = out.split(",")
    return int(x), int(y)


def list_gpus():
    """Each GPU mw-gpu-load sees: (name, "high,low" LUID, its first screen or None)."""
    listing = subprocess.run([gpu_load._tool(), "--list"],
                             capture_output=True, text=True, encoding="utf-8").stdout
    gpus = []
    for line in listing.splitlines():
        m = re.search(r"^\d+\t(.*?) — .*\(LUID (\d+):(\d+)\)(?: — (\S+))?", line)
        if m:
            gpus.append((m.group(1), "%s,%s" % (m.group(2), m.group(3)), m.group(4)))
    return gpus


def client_on_gpu(name):
    """Where to open the client so that it decodes on the GPU named `name`:
    ((x, y) of that GPU's screen, its LUID). A GPU with no screen is refused,
    for the reason client_luid() gives."""
    gpu = next((g for g in list_gpus() if name.lower() in g[0].lower()), None)
    if not gpu or not gpu[2]:
        raise SystemExit("the client cannot sit on the %s: %s" % (
            name, "no screen of its own" if gpu else "no such GPU"))
    out = subprocess.run(["powershell", "-NoProfile", "-Command",
                          "Add-Type -AssemblyName System.Windows.Forms; "
                          "[System.Windows.Forms.Screen]::AllScreens | "
                          "Where-Object { $_.DeviceName -eq '%s' } | "
                          "ForEach-Object { '{0},{1}' -f $_.Bounds.X, $_.Bounds.Y }" % gpu[2]],
                         capture_output=True, text=True).stdout.strip()
    if not out:
        raise SystemExit("the %s's screen %s is not on the desktop" % (gpu[0], gpu[2]))
    x, y = out.split(",")
    print("  client decodes on the %s (%s), which drives %s" % (gpu[0], gpu[1], gpu[2]),
          flush=True)
    return (int(x), int(y)), gpu[1]


def client_luid():
    """The adapter the client decodes on: the GPU that drives its screen.

    A GPU with no screen of its own presents through the one that has it, so a
    client pinned to the RTX of DualRTX (no display) read back every frame
    through the Arc — and when a pass saturated the Arc, the client froze on a
    stream that was fine (22/09/2026, the false "C"). MW_BENCH_CLIENT_LUID is
    still honoured, but a LUID whose GPU drives no screen is refused. Unset, the
    LUID is the one of the GPU behind the screen at client_position().
    """
    try:
        gpus = list_gpus()
    except gpu_load.Unavailable:
        return os.environ.get("MW_BENCH_CLIENT_LUID", "")  # cannot check: as given
    luid = os.environ.get("MW_BENCH_CLIENT_LUID", "").strip()
    if luid:
        if "," not in luid:
            luid = "0," + luid
        gpu = next((g for g in gpus if g[1] == luid), None)
        if gpu and not gpu[2]:
            raise SystemExit("MW_BENCH_CLIENT_LUID=%s is the %s, which drives no screen: the "
                             "client would present through another GPU (docs/bench-campaign.md §4)"
                             % (luid, gpu[0]))
        return luid
    x, y = client_position()
    screen = subprocess.run(["powershell", "-NoProfile", "-Command",
                             "Add-Type -AssemblyName System.Windows.Forms; "
                             "[System.Windows.Forms.Screen]::FromPoint("
                             "(New-Object System.Drawing.Point %d,%d)).DeviceName" % (x, y)],
                            capture_output=True, text=True).stdout.strip()
    gpu = next((g for g in gpus if g[2] and g[2].lower() == screen.lower()), None)
    if not gpu:
        # mw-gpu-load names one screen per GPU: a second screen on the same GPU
        # is not in its list.
        raise SystemExit("no GPU is listed for the client's screen %s; set MW_BENCH_CLIENT_LUID"
                         % (screen or "?"))
    print("  client decodes on the %s (%s), which drives %s" % (gpu[0], gpu[1], screen),
          flush=True)
    return gpu[1]


# The GPU the running client was pinned to by a pass's `clientGpu`, or None
# for the default placement.
KIOSK = {"gpu": None}


def kiosk_start(url, on_gpu=None):
    """A Chrome of its own, with a debugging port and no certificate fuss.

    Not the Chrome extension: it emulates a fixed viewport, its synthetic
    clicks carry no user activation so requestFullscreen is refused, and its
    tab stays visibilityState=hidden, which freezes rAF.

    `on_gpu` opens it on the screen of that GPU, so that it decodes there.
    """
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
                    "Where-Object { $_.CommandLine -like '*%s*' } | "
                    "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }"
                    % os.path.basename(PROFILE)],
                   capture_output=True, text=True)
    time.sleep(2)
    # The client decodes on the GPU of its own screen, never the encoder's
    # (docs/bench-campaign.md §4). Chrome wants "high,low": a bare decimal is
    # ignored without a word.
    if on_gpu:
        pos, luid = client_on_gpu(on_gpu)
    else:
        pos, luid = client_position(), client_luid()
    KIOSK["gpu"] = on_gpu
    subprocess.Popen([CHROME] + (["--use-adapter-luid=" + luid] if luid else []) + [
                      "--user-data-dir=" + PROFILE,
                      "--no-first-run", "--no-default-browser-check",
                      "--disable-infobars",
                      "--autoplay-policy=no-user-gesture-required",
                      "--remote-debugging-port=%d" % DEBUG_PORT,
                      "--ignore-certificate-errors",
                      # Covered by another window, Chrome calls the page hidden
                      # and the app ignores a click on a tile: the client on a
                      # screen someone else is using never launched (22/09/2026).
                      # Same switches as kiosk.ps1 gives the content page.
                      "--disable-features=CalculateNativeWinOcclusion",
                      "--disable-backgrounding-occluded-windows",
                      "--disable-renderer-backgrounding",
                      "--window-position=%d,%d" % pos, "--window-size=1920,1200",
                      url])
    # Wait for the debugging port to answer rather than for a fixed delay: on a
    # busy machine Chrome takes longer than eight seconds to open it, and the
    # first CDP call then fails with ECONNREFUSED before anything has run.
    import urllib.request
    for _ in range(40):
        try:
            with urllib.request.urlopen(
                    "http://localhost:%d/json/version" % DEBUG_PORT, timeout=2):
                time.sleep(2)
                return
        except Exception:
            time.sleep(1)
    time.sleep(5)


def content_start(page, probe=True):
    """Put a bench page (content/<page>) TOPMOST over the captured screen.

    What the host streams is otherwise whatever the operator has open there —
    a terminal that scrolls whenever a session prints — and no two passes see
    the same picture. The rectangle is MW_BENCH_CONTENT_RECT ("x,y,w,h",
    physical pixels), the primary screen at 2560x1440 by default.
    """
    rect = os.environ.get("MW_BENCH_CONTENT_RECT", "0,0,2560,1440").split(",")
    path, _, query = page.partition("?")
    url = "file:///" + os.path.join(os.path.dirname(HERE), "content", path).replace("\\", "/")
    if query:
        url += "?" + query
    # A kiosk Chrome sometimes stays a blank white window for good — seen on
    # 22/09/2026, a whole pass streamed a white screen at 2 fps. What is on the
    # screen is read back (Desktop Duplication, not GDI, which can lie) and the
    # launch retried until the page has painted something.
    for _attempt in range(3):
        subprocess.run(["powershell", "-NoProfile", "-File",
                        os.path.join(os.path.dirname(HERE), "kiosk.ps1"), "-Url", url,
                        "-X", rect[0], "-Y", rect[1], "-W", rect[2], "-H", rect[3]],
                       capture_output=True, text=True)
        if not probe:
            return  # a screen ddagrab may not reach: nothing to check it with
        for _ in range(8):
            time.sleep(2)
            if screen_painted():
                return
        print("      content page never painted — launching it again", flush=True)
    raise drive.PassFailed("the content page never painted on the captured screen")


def screen_painted():
    """True when output 0 is not one flat colour (a pointer on it aside)."""
    if int(os.environ.get("MW_BENCH_DISPLAY", "0") or 0):
        return True  # ddagrab reads the first adapter's outputs only: cannot tell
    p = subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi", "-i",
                        "ddagrab=output_idx=0:framerate=5,hwdownload,format=bgra,"
                        "scale=640:360,format=gray", "-frames:v", "1", "-f", "rawvideo", "-"],
                       capture_output=True)
    px = p.stdout
    if len(px) < 640 * 360:
        return True  # cannot tell: do not block the pass on the probe itself
    # Text at this scale is grey, not black: count what departs from the
    # dominant shade. A pointer alone is a few dozen pixels, text is thousands.
    ref = max(set(px[::97]), key=px[::97].count)
    return sum(1 for v in px[::7] if abs(v - ref) > 30) > 300


def content_stop():
    subprocess.run(["powershell", "-NoProfile", "-File",
                    os.path.join(os.path.dirname(HERE), "kiosk-close.ps1"), "-Content"],
                   capture_output=True, text=True)


def kiosk_stop():
    subprocess.run(["powershell", "-NoProfile", "-File",
                    os.path.join(os.path.dirname(HERE), "kiosk-close.ps1"), "-Client"],
                   capture_output=True, text=True)


# ── reading what came back ──────────────────────────────────────────────────

def parse_latency(text):
    """'28.4ms' -> 28.4. '--' and None mean the overlay had nothing to add up."""
    if not text:
        return None
    m = re.search(r"([0-9]+(?:\.[0-9]+)?)", text)
    return float(m.group(1)) if m else None


def negotiated(stats):
    """What the overlay says actually happened, as opposed to what was asked.

    A HEVC that came back H.264, a 4:4:4 that came back 4:2:0 and an HDR that
    came back SDR are silent everywhere else. This is the row that tells.
    """
    rows = stats.get("rows") or {}
    get = lambda *names: next((rows[k] for n in names for k in rows if n.lower() in k.lower()), "")
    return {
        "resolution": get("Resolution"),
        "framerate": get("Framerate", "FPS"),
        "bitrate": get("Bitrate"),
        "codec": get("Codec"),
        "enhancer": get("Enhancer"),
        "transport": get("Transport"),
        "decoder": get("Decoder"),
    }


def verdict(machine, status, stats, reason):
    """green / yellow / red / grey, with the fleet rules that override numbers.

    On a VM, a machine with no GPU or a software encoder, latency is never a
    failure criterion — those cells are grey or yellow, never red. What is
    validated there is that it works at all.
    """
    if status == "skipped":
        return "grey", reason
    if status != "ok":
        if not fleet.MACHINES[machine].get("perf", True):
            return "yellow", reason
        return "red", reason
    lat = parse_latency((stats or {}).get("latencyText"))
    if lat is None:
        return "yellow", "stream ran, the overlay never totalled a latency"
    if not fleet.MACHINES[machine].get("perf", True):
        return "green", "compatibility bench: latency is reported, never judged"
    if lat > 150:
        return "yellow", "total latency %.1f ms is off the mark" % lat
    return "green", ""


# ── one pass ────────────────────────────────────────────────────────────────

def run_pass(d, chapter, machine, spec, base, seconds, settle, access):
    rec = {
        "chapter": chapter, "machine": machine, "pass": spec["id"],
        "factor": spec.get("factor", ""),
        "target": spec.get("target", access["target"]),
        "via": spec.get("via", "lan"),
        "content": spec.get("content", ""),
        "ts": time.strftime("%Y-%m-%dT%H:%M:%S"),
    }
    settings = dict(base)
    settings.update(spec.get("settings") or {})
    rec["requested"] = settings

    shot_dir = os.path.join(SCREENS, machine, chapter)
    shot = os.path.join(shot_dir, spec["id"] + ".png")

    load = None
    shown = False
    try:
        want_url = access["rendezvous"] if rec["via"] == "rendezvous" else access["lan"]
        if not want_url:
            raise drive.PassFailed("no %s URL for this machine" % rec["via"])

        # displayGpu: stream the native display that GPU drives, so the pass
        # measures that GPU's encoder. A codec its driver does not offer is
        # grey, never red: there is nothing to measure.
        index = disp = None
        if spec.get("displayGpu"):
            try:
                index, disp = gpu_load.display_on_gpu(spec["displayGpu"])
            except gpu_load.Unavailable as e:
                raise drive.NotApplicable(str(e))
            rec["encoder"] = {"gpu": disp.get("gpu"), "name": disp.get("encoder"),
                              "display": disp.get("label"), "codecs": disp.get("codecs")}
            codec = settings["video_codec"].lower().replace(".", "")
            offered = [c.strip().lower().replace(".", "")
                       for c in (disp.get("codecs") or "").split(",")]
            if codec not in offered:
                raise drive.NotApplicable("%s on the %s offers %s: no %s encoder" % (
                    disp.get("encoder"), disp.get("gpu"), disp.get("codecs"), codec.upper()))

        # clientGpu: the client decodes on that GPU, never on the encoder's
        # (docs/bench-campaign.md §4). The browser is reopened only when the
        # placement changes, and put back where it was for a pass without one.
        if spec.get("clientGpu") != KIOSK["gpu"]:
            kiosk_start("about:blank", on_gpu=spec.get("clientGpu"))
            d.reconnect()
            access["_current"] = None
        # Reaching a host through the rendezvous is a slower thing than dialling
        # its address: the page has to be fetched from stream.dev, a tunnel has
        # to come up, and only then does the host list arrive. The LAN patience
        # applied to it reported "no host card" on machines that were simply
        # still connecting.
        patience = 45 if rec["via"] == "rendezvous" else 25
        if want_url != access.get("_current"):
            d.navigate(want_url)
            access["_current"] = want_url
            d.wait_library(access["name"], access["pin"], tries=patience)

        d.apply_settings(settings)
        if not d.wait_library(access["name"], access["pin"], tries=patience):
            # A host that has just ended a stream can take a while to serve its
            # library again, and a reload that lands in that window shows an
            # empty page. Going back to the URL once is cheap and turns a
            # spurious failure into a pass.
            d.navigate(want_url)
            if not d.wait_library(access["name"], access["pin"], tries=patience):
                raise drive.PassFailed("no host card after the reload, twice")

        card, app = d.pick_tile(rec["target"], index=index)
        rec["tile"] = {"host": card.get("name", ""), "app": app.get("name", ""),
                       "appId": app.get("appId", "")}
        # contentAfter: the captured screen only exists once the stream is up
        # (the Virtual Display), so the page goes up after the first picture.
        after = spec.get("contentAfter", False)
        if spec.get("content") and machine == "local" and not after:
            content_start(spec["content"])
            shown = True
        if spec.get("load") == "gpu":
            # The load comes first and is calibrated before the stream exists:
            # were it tuned while the encoder runs, it would hand back the GPU
            # time the pass means to take away.
            try:
                if machine == "local":
                    # loadGpu names it outright where /api/native/status cannot:
                    # a Sunshine target, whose encoder GPU is Sunshine's config.
                    gpu = (spec.get("loadGpu") or (disp or {}).get("gpu")
                           or gpu_load.encoder_gpu(rec["target"]))
                    load = gpu_load.Load(gpu, os.path.join(
                        RESULTS, "gpu-load-%s-%s-%s.jsonl" % (chapter, machine, spec["id"])),
                        level=spec.get("loadLevel"))
                elif fleet.MACHINES[machine]["os"] == "windows":
                    # Deployed beforehand with gpu_load.deploy_windows(). An iGPU
                    # whose driver reports no temperature is covered by the
                    # SoC's own thermal protection, hence loadAllowNoSensor.
                    gpu = gpu_load.remote_encoder_gpu(machine, rec["target"])
                    load = gpu_load.RemoteLoad(machine, gpu, level=spec.get("loadLevel"),
                                               allow_no_sensor=spec.get("loadAllowNoSensor", False))
                elif fleet.MACHINES[machine]["os"] == "macos":
                    # Built on the Mac itself, from HEAD's sources, when they changed.
                    gpu_load.deploy_macos(machine)
                    gpu = gpu_load.remote_encoder_gpu_macos(machine, rec["target"])
                    load = gpu_load.MacLoad(machine, gpu, level=spec.get("loadLevel"),
                                            allow_no_sensor=spec.get("loadAllowNoSensor", False))
                else:
                    raise gpu_load.Unavailable("the GPU load runs on Windows and macOS hosts for now")
                load.wait_calibrated()
            except gpu_load.Unavailable as e:
                raise drive.NotApplicable(str(e))
        d.launch(card, app)
        d.wait_picture(timeout=60)
        if spec.get("content") and machine == "local" and after:
            content_start(spec["content"], probe=False)
            shown = True
        time.sleep(settle)

        checks = spec.get("checks") or []
        if "input" in checks:
            d.poke_input()
        time.sleep(seconds)

        d.expand_latency_detail()
        stats = d.stats()
        rec["stats"] = stats
        if load:
            rec["load"] = load.snapshot(seconds)
            if not rec["load"]["running"]:
                # A pass measured after the load stopped (its 60 s ran out, or
                # the heat guard fired) measured an idle GPU.
                raise drive.NotApplicable("the GPU load stopped before the measure: %s" % (
                    (rec["load"].get("end") or {}).get("reason", "exited")))
        rec["negotiated"] = negotiated(stats)
        rec["latencyMs"] = parse_latency(stats.get("latencyText"))
        rec["legs"] = stats.get("legs") or {}
        if "audio" in checks:
            rec["audio"] = d.audio_state()
        rec["screenshot"] = d.screenshot(shot)
        rec["status"] = "ok"
        rec["reason"] = ""
    except drive.NotApplicable as e:
        rec["status"] = "skipped"
        rec["reason"] = str(e)
        try:
            rec["screenshot"] = d.screenshot(shot)
        except Exception:
            rec["screenshot"] = None
    except drive.PassFailed as e:
        rec["status"] = "failed"
        rec["reason"] = str(e)
        try:
            rec["screenshot"] = d.screenshot(shot)
        except Exception:
            rec["screenshot"] = None
    except (Exception, SystemExit) as e:  # nothing here may end the campaign
        rec["status"] = "failed"
        rec["reason"] = "%s: %s" % (type(e).__name__, e)
        rec["screenshot"] = None
    finally:
        try:
            d.stop()
        except Exception:
            pass
        if load:
            load.stop()
        if shown:
            content_stop()

    rec["verdict"], why = verdict(machine, rec["status"], rec.get("stats"), rec.get("reason", ""))
    if why and not rec.get("reason"):
        rec["reason"] = why
    return rec


def already_done(chapter, machine):
    """Pass ids already recorded for this chapter and machine, for --resume.

    Sixty passes is an hour and a half. A run that dies on pass four must be
    restartable without throwing away the three that worked.
    """
    done = set()
    path = os.path.join(RESULTS, "passes.jsonl")
    if not os.path.exists(path):
        return done
    with open(path, encoding="utf-8") as f:
        for line in f:
            try:
                rec = json.loads(line)
            except ValueError:
                continue
            if rec.get("chapter") == chapter and rec.get("machine") == machine                     and rec.get("status") == "ok":
                done.add(rec.get("pass"))
    return done


def run_chapter(key, matrix, access_by_machine, only=None, one_pass=None, resume=False):
    chapter = matrix["chapters"][key]
    base = matrix["base"]
    seconds = matrix.get("seconds", 10)
    settle = matrix.get("settleSeconds", 6)

    machines = [m for m in chapter["machines"] if not only or m in only]
    if not machines:
        return []

    out = []
    first = access_by_machine.get(machines[0], {})
    kiosk_start(first.get("lan") or "about:blank")
    d = drive.Driver(DEBUG_PORT)

    for machine in machines:
        access = dict(access_by_machine.get(machine) or {})
        access.setdefault("target", chapter.get("target", "display"))
        access["_current"] = None
        print("\n=== %s · %s ===" % (key, machine))
        # A fresh PIN per machine, minted right now. The one from chapter 1 was
        # minted an hour and several restarts ago, and a client that cannot
        # unlock just shows an empty page — which the pass then reports as "no
        # host card", blaming the host for a stale credential.
        try:
            probe = fleet.probe(machine)
            pin = (probe.get("pin") or {}).get("pin")
            if pin:
                access["pin"] = pin
            lan = fleet.lan_url(machine, probe)
            if lan:
                access["lan"] = lan
            print("  fresh pin=%s lan=%s" % (bool(pin), access.get("lan")), flush=True)
        except Exception as e:
            print("  could not re-probe (%s) — using chapter 1's access" % e, flush=True)
        if not access.get("lan"):
            print("  no address — skipped")
            continue
        done = already_done(key, machine) if resume else set()
        for spec in chapter["passes"]:
            if one_pass and not any(fnmatch.fnmatch(spec["id"], p) for p in one_pass):
                continue
            if spec["id"] in done:
                print("  %-16s already recorded — skipped" % spec["id"], flush=True)
                continue
            print("  %-16s %s" % (spec["id"], spec.get("factor", "")), flush=True)
            rec = run_pass(d, key, machine, spec, base, seconds, settle, access)
            mark = {"green": "OK", "yellow": "~~", "red": "!!", "grey": "--"}.get(rec["verdict"], "??")
            print("      %s  latency=%s  %s" % (
                mark, rec.get("latencyMs"), rec.get("reason", "")[:90]), flush=True)
            append("passes.jsonl", rec)
            out.append(rec)
    kiosk_stop()
    return out


# ── chapter 1 ───────────────────────────────────────────────────────────────

def packages_for(package_dir):
    """Map each machine to the artifact it takes, by OS and architecture."""
    found = {}
    for root, _dirs, files in os.walk(package_dir):
        for name in files:
            found[name] = os.path.join(root, name)

    def pick(*needles):
        for name, path in sorted(found.items()):
            low = name.lower()
            if all(n in low for n in needles):
                return path
        return None

    # The mini PC is a dual boot: which artifact it takes depends on the OS it
    # has booted TODAY, and that is what the fleet file says (its "os"). Wired to
    # the .deb, chapter 1 uninstalled the Windows app it had booted into and
    # then tried to install a Debian package on it (21/09/2026).
    um790pro_is_windows = fleet.MACHINES.get("um790pro", {}).get("os") == "windows"
    return {
        "local":     pick("win", "x64", ".exe"),
        "mw-intel":  pick("win", "x64", ".exe"),
        "mw-arm":    pick("win", "arm64", ".exe"),
        "mw-mac":    pick(".pkg"),
        "um790pro":  pick("win", "x64", ".exe") if um790pro_is_windows else pick(".deb"),
        "mw-debian": pick(".deb"),
    }


def run_chapter1(package_dir, only=None):
    mapping = packages_for(package_dir)
    out = []
    for mid in fleet.MACHINES:
        if only and mid not in only:
            continue
        pkg = mapping.get(mid)
        print("\n=== chapter 1 · %s ===" % mid)
        if not pkg:
            rec = {"machine": mid, "status": "skipped",
                   "reason": "no artifact for this OS/arch in %s" % package_dir}
            print("  " + rec["reason"])
            append("chapter1.jsonl", rec)
            out.append(rec)
            continue
        rec = installer.chapter1(mid, pkg, also_prod=(mid == "local"))
        probe = rec.get("afterUpdate") or {}
        native = probe.get("native") or {}
        rec["status"] = "ok" if native.get("available") else "failed"
        rec["reason"] = "" if native.get("available") else (
            native.get("reason") or native.get("error") or "the native host did not come up")
        print("  autostart=%s running=%s native=%s pin=%s" % (
            probe.get("autostart"), probe.get("running"),
            native.get("available"), bool(rec.get("pin"))))
        append("chapter1.jsonl", rec)
        out.append(rec)
    return out


def shots_chapter1(access, only=None):
    """Open each host in the bench client and photograph its home page.

    This is the evidence for chapter 1 that no shell command can give: the PIN
    really unlocks, the host list really renders, and the version badge in the
    header really reads what the log claimed. One picture per machine.
    """
    machines = [m for m in fleet.MACHINES if not only or m in only]
    if not machines:
        return []
    kiosk_start("about:blank")
    d = drive.Driver(DEBUG_PORT)
    out = []
    for mid in machines:
        acc = access.get(mid) or {}
        url = acc.get("lan")
        rec = {"machine": mid, "url": url}
        try:
            d.navigate(url)
            rec["unlocked"] = d.wait_library(acc.get("name", "bench"), acc.get("pin", ""),
                                             tries=15)
            rec["header"] = d.eval(
                "JSON.stringify((document.querySelector('.app-version, .version-badge')"
                " || {}).textContent || '')").strip('"')
            rec["shot"] = d.screenshot(os.path.join(SCREENS, mid, "01-install", "home.png"))
        except Exception as e:
            rec["unlocked"] = False
            rec["error"] = "%s: %s" % (type(e).__name__, e)
            try:
                rec["shot"] = d.screenshot(os.path.join(SCREENS, mid, "01-install", "home.png"))
            except Exception:
                rec["shot"] = None
        print("  %-10s unlocked=%s shot=%s" % (mid, rec.get("unlocked"),
                                               bool(rec.get("shot"))), flush=True)
        append("chapter1-shots.jsonl", rec)
        out.append(rec)
    kiosk_stop()
    return out


def access_map():
    """Where the client goes for each machine, and with what PIN.

    Built from chapter 1's results when they exist — that is where the PIN was
    minted — and from the machine table otherwise.
    """
    access = {}
    path = os.path.join(RESULTS, "chapter1.jsonl")
    by_machine = {}
    if os.path.exists(path):
        with open(path, encoding="utf-8") as f:
            for line in f:
                try:
                    rec = json.loads(line)
                except ValueError:
                    continue
                by_machine[rec.get("machine")] = rec
    for mid, m in fleet.MACHINES.items():
        rec = by_machine.get(mid, {})
        access[mid] = {
            "lan": rec.get("lanUrl") or fleet.lan_url(mid, {}),
            "rendezvous": rec.get("rendezvousUrl", ""),
            "pin": rec.get("pin", ""),
            "name": "bench",
        }
    return access


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--chapter", action="append", default=[],
                    help="1, 2 or 3; repeatable. Default: all three")
    ap.add_argument("--package", default="", help="directory holding the CI artifacts")
    ap.add_argument("--only", action="append", default=[], help="restrict to a machine")
    ap.add_argument("--pass", dest="one_pass", action="append", default=[],
                    help="restrict to a pass id, or a pattern (codec-load-*); repeatable")
    ap.add_argument("--plan-only", action="store_true")
    ap.add_argument("--resume", action="store_true",
                    help="skip passes already recorded as ok for that machine")
    ns = ap.parse_args()

    matrix = load_matrix()
    chapters = ns.chapter or ["1", "2", "3"]
    if chapters == ["shots"]:
        chapters = []

    if ns.plan_only:
        print("base settings: %s\n" % json.dumps(matrix["base"], indent=2))
        for key, ch in matrix["chapters"].items():
            machines = [m for m in ch["machines"] if not ns.only or m in ns.only]
            print("%s — %s" % (key, ch["label"]))
            print("  machines: %s" % ", ".join(machines) or "(none)")
            for spec in ch["passes"]:
                print("    %-16s %-38s target=%s via=%s" % (
                    spec["id"], spec.get("factor", ""),
                    spec.get("target", ch.get("target")), spec.get("via", "lan")))
            print("  = %d passes\n" % (len(ch["passes"]) * len(machines)))
        return

    os.makedirs(RESULTS, exist_ok=True)
    if "1" in chapters:
        if not ns.package:
            raise SystemExit("chapter 1 needs --package <dir with the CI artifacts>")
        run_chapter1(ns.package, only=ns.only or None)

    access = access_map()
    if "1" in chapters or ns.chapter == ["shots"]:
        print("\n=== chapter 1 · home pages ===")
        shots_chapter1(access, only=ns.only or None)
    if "2" in chapters:
        run_chapter("02-dualrtx", matrix, access, only=ns.only or None,
                    one_pass=ns.one_pass, resume=ns.resume)
    if "3" in chapters:
        run_chapter("03-fleet", matrix, access, only=ns.only or None,
                    one_pass=ns.one_pass, resume=ns.resume)


if __name__ == "__main__":
    main()
