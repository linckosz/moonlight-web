"""One stream under Auto's learned ceiling, or the climb from the bottom to the top.

    python ceiling.py rung --rung 500 --secs 600 --tag r500 --target vdisplay --screen 3840x2160
    python ceiling.py climb --tag climb --target vdisplay --screen 3840x2160 --deadline 21:45
    python ceiling.py climb --tag climb2 --rung 500 --target vdisplay --screen 3840x2160
    python ceiling.py rung --rung 250 --secs 120 --tag journal --learn log --expect journal
    python ceiling.py rung --rung 700 --secs 300 --tag ill --target vdisplay --screen 3840x2160 \
        --expect sick --load-gpu 0:12345
    python ceiling.py rung --rung 1000 --secs 360 --tag weak --target vdisplay --screen 3840x2160 \
        --expect sick --client-port 9423 --client-url https://10.0.0.2:8443/
    python ceiling.py rung --keep --secs 600 --tag mini-1 --tile "Display 1" --gdi DISPLAY1 \
        --client-port 9423 --client-url https://10.0.0.2:18443/
    python ceiling.py rung --keep --secs 400 --tag tv --expect silent --tile "Display 1" \
        --gdi DISPLAY1 --client-port 9452 --client-url https://10.0.0.2:18443/
    python ceiling.py picture --max-launches 2     # a browser in someone's hands
    python ceiling.py rung --rung 450 --secs 60 --tag guard --target vdisplay \
        --screen 3840x2160 --expect any --refuse-above 400
    python ceiling.py rung --secs 330 --tag rdv --target vdisplay --screen 3840x2160 \
        --expect any --rendezvous

A stream earns a rung by holding a moving picture for five minutes, and the rung
only serves the next launch, so going from 250 M to 1000 M takes at least five
launches. `rung` prepares the browser's key and tests one rung for as long as
asked; `climb` prepares nothing and lets the browser earn its way up. `--keep`
leaves the key as the last launch left it.

Driven like ../content-age/pass.py: MW_BENCH_LOCAL_PORTS for a --dev instance,
MW_BENCH_CLIENT_POS and MW_BENCH_CLIENT_LUID to keep the client off the encoder's
GPU (docs/bench-campaign.md §4), --client-port for a Chrome on another machine.

--screen tells the page it sits on a screen of that size. A desk with no 4K screen
cannot ask for more than 1440p120 otherwise, and a client on a virtual 4K screen
would decode on the GPU that encodes. Decoding and rendering stay real; only the
presenting stays at the true screen's size.

The app reads the page's mode at launch, so everything this writes in localStorage
is written before the click. See README.md.
"""
import argparse
import importlib
import json
import os
import subprocess
import sys
import time
import urllib.parse

HERE =os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(BENCH))
OUT = os.path.join(REPO, "bench-out", "auto-ceiling")
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
sys.path.insert(0, os.path.join(BENCH, "content-age"))
sys.path.insert(0, os.path.join(BENCH, "cadence"))
sys.path.insert(0, HERE)
import journal  # noqa: E402

# Every memory that could make this launch something other than the one asked for.
# Read before, and read again after: the enhancer's trap was a descent nobody saw.
MEMORIES = (journal.KEY, "mw_enhancer_rung", "mw_autostep_failed", "mw_autostep")

# Only what the pass needs over the product's own defaults: Auto and Auto, as a
# viewer who never opened Settings has them.
SETTINGS = {"stream_resolution": "auto", "stream_fps": 0, "stream_bitrate_auto": True,
            "mute_host_audio": True, "show_performance_stats": True}
# --enhance: the Enhancer on, its algorithm left to Auto, as a viewer who turned it on.
ENHANCE = {"video_enhancement": "on", "video_enhancement_algo": "auto"}

CAPTURE = r"""(() => {
    if (window.__mwCeilingLog) return window.__mwCeilingLog.length;
    const out = (window.__mwCeilingLog = []);
    const wanted = /^\[MW\] (Auto ceiling|Pixel budget)/;
    const inner = console.log;
    console.log = (...args) => {
        inner.apply(console, args);
        try {
            if (wanted.test(String(args[0]))) out.push(String(args[0]));
        } catch (e) { /* never break the line it was catching */ }
    };
    return 0;
})()"""

READ_MEMORIES = """JSON.stringify(Object.fromEntries(%s.map((k) => [k, localStorage.getItem(k)])))"""

# Screen.prototype, and the DPR kept: devicePixelSize() multiplies the two, and the
# stream's own canvas goes on using the real ratio.
FAKE_SCREEN = """(() => {
    const dpr = window.devicePixelRatio || 1;
    const w = %d / dpr, h = %d / dpr;
    for (const [k, v] of [['width', w], ['height', h], ['availWidth', w], ['availHeight', h]])
        Object.defineProperty(Screen.prototype, k, { configurable: true, get: () => v });
    return JSON.stringify({ width: screen.width, height: screen.height, dpr });
})()"""


# The guard says nothing of a "yes", so the pass listens at the door. With a limit
# it also answers "no" above it: the browser's refusal, on a real launch.
SPY_DECODING = r"""(() => {
    window.__mwDecodingLimit = %d;
    if (window.__mwDecodingAsked) return window.__mwDecodingAsked.length;
    const asked = (window.__mwDecodingAsked = []);
    const proto = MediaCapabilities.prototype;
    const inner = proto.decodingInfo;
    proto.decodingInfo = function (config) {
        const v = (config && config.video) || {};
        const entry = { contentType: v.contentType, width: v.width, height: v.height,
                        fps: v.framerate, answer: null };
        asked.push(entry);
        const limit = window.__mwDecodingLimit;
        if (limit > 0 && v.width * v.height * v.framerate > limit) {
            entry.answer = 'no, from the bench';
            return Promise.resolve({ supported: true, smooth: false, powerEfficient: false });
        }
        return inner.call(this, config).then((info) => {
            entry.answer = { supported: info.supported, smooth: info.smooth,
                             powerEfficient: info.powerEfficient };
            return info;
        });
    };
    return 0;
})()"""

READ_ASKED = "JSON.stringify(window.__mwDecodingAsked || [])"

# Through the rendezvous: an entry under a machine that is not this one. Whatever
# this page learns, this stays as it was put.
WITNESS_KEY = journal.key_of("bench-another-machine")

# The one rendezvous this bench may hold a line to: never the production one.
STAGING = "://stream.dev."


# What a launch can take over its --secs: the picture's wait, the page leaving, and
# the virtual display going (wait_picture 90 s, 14 s of stops, wait_screens 150 s).
LAUNCH_EXTRA_S = 254


def say(text):
    print(text, flush=True)


def nvidia_load():
    """Each NVIDIA GPU's encoder and decoder, in percent; [] when there is no tool.

    The one check that cannot be fooled by a LUID that moved on a reboot: the GPU
    that encodes and the GPU that decodes, read while they do it.
    """
    try:
        out = subprocess.run(
            ["nvidia-smi", "--query-gpu=pci.bus_id,utilization.encoder,utilization.decoder",
             "--format=csv,noheader,nounits"], capture_output=True, text=True, timeout=10).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    gpus = []
    for line in out.splitlines():
        parts = [p.strip() for p in line.split(",")]
        if len(parts) == 3 and parts[1].isdigit() and parts[2].isdigit():
            gpus.append({"bus": parts[0], "enc": int(parts[1]), "dec": int(parts[2])})
    return gpus


def shared_gpu(samples):
    """The buses seen encoding and decoding in the same sample: the bench's rule broken."""
    return sorted({g["bus"] for gpus in samples for g in gpus if g["enc"] > 0 and g["dec"] > 0})


def stats_line(rows):
    """The row Auto's ceiling adds to the card, whatever the page's language."""
    for label, value in (rows or {}).items():
        if label.startswith(("Auto", "自动")):
            return "%s %s" % (label, value)
    return None


def settings_for(a):
    """What the page is set to: Auto and Auto, and the Enhancer when the pass is about it."""
    return dict(SETTINGS, **ENHANCE) if getattr(a, "enhance", False) else dict(SETTINGS)


def enhancer_of(rows):
    """The Enhancer's row on the card, whatever the page's language."""
    for label, value in (rows or {}).items():
        if label.startswith(("Enhancer", "Amélioration", "增强器")):
            return value
    return None


def enhancer_defects(samples):
    """An Enhancer pass shows MWSR at work, or idle where the stream fills the screen
    (1:1, nothing to enlarge): never missing, never another algorithm."""
    seen = [s.get("enhancer") for s in samples if s.get("enhancer")]
    if not seen:
        return ["the Enhancer was asked for and the card never showed it"]
    if not any("MWSR" in v or "1:1" in v for v in seen):
        return ["the Enhancer was asked for and never ran MWSR: %s" % ", ".join(sorted(set(seen)))]
    return []


def pick_tile(d, a):
    """The tile to click, by its name when a pass gives one.

    A rank is not a name: the first display tile of a desk may well be the one
    another bench is measuring on.
    """
    if a.target == "vdisplay" or not a.tile:
        return d.pick_tile(a.target)
    for index in range(8):
        card, app = d.pick_tile("display", index=index)
        if app.get("name") == a.tile:
            return card, app
    raise SystemExit("no display tile named %r" % a.tile)


def read_memories(d, keys=MEMORIES):
    return d.json_eval(READ_MEMORIES % json.dumps(list(keys)))


def deadline_at(text):
    """Today at HH:MM, in seconds since the epoch; 0 when no deadline is given."""
    if not text:
        return 0
    hour, minute = text.split(":")
    today = time.localtime()
    return time.mktime((today.tm_year, today.tm_mon, today.tm_mday, int(hour), int(minute), 0, 0, 0, -1))


def save(tag, result):
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, tag + ".json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, ensure_ascii=False)


def silent_load(gpu_load, gpu, path, level):
    """gpu_load.Load with the tool's music off: nothing of a bench is heard on a desk."""
    class Silent(gpu_load.Load):
        def __init__(self):
            self.gpu, self.path = gpu, path
            os.makedirs(os.path.dirname(path), exist_ok=True)
            argv = [gpu_load._tool(), "--gpu", gpu, "--autostart", "--topmost", "--no-music", "--json", path]
            if level:
                argv += ["--level", str(level)]
            self.proc = subprocess.Popen(argv)
            self.started = time.time()
    return Silent()


class LoadCycles:
    """mw-gpu-load on the client's GPU, from --load-at on, for as long as the pass.

    The tool stops at 60 s and a sick stream takes 60 s of drops to say so: one run
    is not enough, and each next run replays the level the first one found, without
    its eight seconds of calibration. Its own heat limit stays in charge: a run that
    stops for it is not started again.
    """

    def __init__(self, a, tag):
        self.a = a
        self.tag = tag
        self.runs = []
        self.current = None
        self.level = None
        self.refused = None

    def keep_going(self, since_start):
        if self.refused or since_start < self.a.load_at:
            return
        gpu_load = importlib.import_module("gpu_load")
        if self.current:
            seen = self.current.snapshot(5)
            if seen["running"]:
                return
            self.runs.append(seen)
            # A tool that said no is said no again: never started over, every two
            # seconds, for the whole pass (05/11: a GPU it could not see).
            said_no = next((x for x in gpu_load.read(self.current.path) if x.get("event") == "refused"), None)
            if said_no:
                self.refused = "%s: %s" % (said_no.get("reason"), said_no.get("detail"))
                self.current = None
                say("  !! no load at all: %s" % self.refused)
                return
            say("  load run %d over: %s fps, %s C, %s" % (
                len(self.runs), seen["fps"], seen["tempC"],
                (seen["end"] or {}).get("reason", "?")))
            self.level = self.level or seen["level"]
            self.current = None
            if (seen["end"] or {}).get("reason") not in (None, "duration", "timeout", "time"):
                self.refused = (seen["end"] or {}).get("reason")
                say("  no more load: %s" % self.refused)
                return
        path = os.path.join(OUT, "%s.load-%d.jsonl" % (self.tag, len(self.runs) + 1))
        self.current = silent_load(gpu_load, self.a.load_gpu, path, self.level)

    def stop(self):
        if self.current:
            self.runs.append(self.current.snapshot(5))
            self.current.stop()
            self.current = None
        return {"gpu": self.a.load_gpu, "runs": self.runs, "stoppedBy": self.refused}


def is_loopback(url):
    host = (urllib.parse.urlsplit(url or "").hostname or "").lower()
    return host == "localhost" or host == "::1" or host.startswith("127.")


class Bench:
    def __init__(self, a):
        self.a = a
        self.remote = a.client_port > 0
        if os.environ.get("MW_BENCH_DEBUG_PORT"):
            self.run.DEBUG_PORT = int(os.environ["MW_BENCH_DEBUG_PORT"])
        self.access = dict(self.run.access_map().get("local") or {})
        probe = self.fleet.probe("local")
        pin = (probe.get("pin") or {}).get("pin")
        if pin:
            self.access["pin"] = pin
        self.access["lan"] = a.client_url or self.fleet.lan_url("local", probe) or self.access.get("lan")
        self.identifier = ""
        self.witness = None
        if a.rendezvous:
            url = self.fleet.rendezvous_url(probe)
            if not url or STAGING not in url:
                raise SystemExit("--rendezvous: this instance holds no line to the staging server")
            self.access["lan"] = url
            self.identifier = journal.identifier_of(url)
        if self.remote and is_loopback(self.access["lan"]):
            # The fleet knows this desk by its loopback: a Chrome on another
            # machine sent there opens itself, and the pass waits for a host
            # card that never comes (07/11/2026).
            raise SystemExit("--client-port: %s is the client's own loopback; give the host's "
                             "LAN address with --client-url" % self.access["lan"])
        self.key = journal.key_of(self.identifier)
        self.memories = (self.key,) + MEMORIES[1:]
        if self.identifier:
            self.memories += (journal.KEY, WITNESS_KEY)
        if self.remote:
            self.d = self.drive.Driver(a.client_port)
        else:
            self.run.kiosk_start(self.access["lan"])
            self.d = self.drive.Driver(self.run.DEBUG_PORT)
        # The Driver holds Auto at its base for every other bench: this one is
        # about the ceiling, and says what the switch is at each launch.
        self.d.auto_learn = a.learn
        self.shown = False
        self.first = True

    # Imported here: --plan and the tests run on a machine with no bench tools.
    run = property(lambda self: importlib.import_module("run"))
    drive = property(lambda self: importlib.import_module("drive"))
    fleet = property(lambda self: importlib.import_module("fleet"))

    def monitors(self):
        return importlib.import_module("cadence").monitors()

    def said(self, value):
        """A memory, as a log may say it: the machine's address stays out of the log."""
        text = json.dumps(value)
        return text.replace(self.identifier, "<identifier>") if self.identifier else text

    def library(self):
        return importlib.import_module("pass").library(self.d, self.access)

    def prepare_page(self):
        """The page at the host's address, with the code on disk, its cards up."""
        d = self.d
        d.navigate(self.access["lan"])
        # The service worker keeps the last version it cached: an edited module
        # would not run (see scripts/bench/content-age/pass.py).
        d.eval("(async () => { await caches.delete('mw-shell'); for (const r of await "
               "navigator.serviceWorker.getRegistrations()) await r.unregister(); return 1; })()")
        d.navigate(self.access["lan"])
        self.library()
        d.eval("localStorage.removeItem('mw-streaming-settings')")
        d.apply_settings(settings_for(self.a))
        if self.a.lang:
            d.eval("localStorage.setItem('mw-lang', %s)" % json.dumps(self.a.lang))
            d.call("Page.reload")
            time.sleep(4)
        self.library()
        if self.identifier:
            # This page's plain key must stay empty, whatever an older pass left.
            d.eval("localStorage.removeItem(%s)" % json.dumps(journal.KEY))
            self.witness = json.dumps(journal.prepared_key("another-machine", 700, 1))
            d.eval("localStorage.setItem(%s, %s)" % (json.dumps(WITNESS_KEY), json.dumps(self.witness)))

    def arm_page(self):
        """What a reload wipes: put back before each launch, after each look at the cards."""
        d = self.d
        d.eval(CAPTURE)
        d.eval(SPY_DECODING % (self.a.refuse_above * 1000000))
        if self.a.screen:
            w, h = (int(v) for v in self.a.screen.lower().split("x"))
            said = d.json_eval(FAKE_SCREEN % (w, h))
            size = (round(said["width"] * said["dpr"]), round(said["height"] * said["dpr"]))
            if size != (w, h):
                raise SystemExit("the page reads a %s screen, not %s" % (size, self.a.screen))
            say("  the page now reads a %dx%d screen (dpr %s)" % (w, h, said["dpr"]))

    def one_launch(self, tag, rung_m, keep, expect):
        a, d = self.a, self.d
        if self.first:
            self.prepare_page()
        self.first = False
        self.arm_page()
        d.eval("window.__mwCeilingLog.length = 0")
        d.eval("window.__mwDecodingAsked.length = 0")
        before = read_memories(d, self.memories)
        say("  before: " + self.said(before))
        card, app = pick_tile(d, a)
        uuid = card["uuid"]
        say("tile %s / %s" % (card.get("name"), app.get("name")))
        # Nothing carried from a pass before, unless this pass is about that.
        d.eval("localStorage.removeItem('mw_enhancer_rung')")
        d.eval("localStorage.removeItem('mw_autostep_failed')")
        d.eval("localStorage.removeItem('mw_autostep')")
        if a.autostep:
            d.eval("localStorage.setItem('mw_autostep', %s)" % json.dumps(a.autostep))
        prepared = before.get(self.key) if keep else None
        if not keep:
            d.eval("localStorage.removeItem(%s)" % json.dumps(self.key))
            if rung_m:
                prepared = json.dumps(journal.prepared_key(uuid, rung_m, int(time.time() * 1000)))
                d.eval("localStorage.setItem(%s, %s)" % (json.dumps(self.key), json.dumps(prepared)))
        d.eval("localStorage.setItem('mw_auto_learn', %s)" % json.dumps(a.learn))

        baseline = self.monitors()
        before_names = {m[0] for m in baseline}
        result = {"tag": tag, "args": vars(a), "rungAsked": rung_m, "before": before,
                  "screensBefore": baseline, "samples": [], "gpu": []}
        started = time.time()
        d.launch(card, app)
        d.wait_picture(timeout=90)
        # The guard runs before the launch, and a reload at the end would wipe this.
        asked = d.json_eval(READ_ASKED)
        result["decodingAsked"] = asked
        for one in asked:
            say("  decodingInfo %sx%s at %s, %s: %s" % (
                one.get("width"), one.get("height"), one.get("fps"),
                one.get("contentType"), json.dumps(one.get("answer"))))
        rect = None
        if a.target == "vdisplay":
            time.sleep(2)
            made = [m for m in self.monitors() if m[0] not in before_names]
            if not made:
                raise SystemExit("no virtual display appeared: %s" % self.monitors())
            x, y = made[0][1].split(",")
            w, h = made[0][2].split("x")
            rect = "%s,%s,%s,%s" % (x, y, w, h)
            result["virtualDisplay"] = made[0]
            say("  virtual display " + " ".join(made[0]))
        elif a.gdi:
            named = [m for m in baseline if m[0].endswith(a.gdi)]
            if not named:
                raise SystemExit("%s is not on the desktop: %s" % (a.gdi, baseline))
            x, y = named[0][1].split(",")
            w, h = named[0][2].split("x")
            rect = "%s,%s,%s,%s" % (x, y, w, h)
        if rect:
            os.environ["MW_BENCH_CONTENT_RECT"] = rect
        self.run.content_start(a.page, probe=False)
        self.shown = True
        if not self.remote:
            win = d.call("Browser.getWindowForTarget")
            d.call("Browser.setWindowBounds", windowId=win["windowId"],
                   bounds={"windowState": "fullscreen"})

        load = LoadCycles(a, tag) if a.load_gpu else None
        ended_early = False
        stop_on = False
        lines = []
        end = time.time() + a.secs
        last = 0
        try:
            while time.time() < end:
                lines = d.json_eval("JSON.stringify(window.__mwCeilingLog || [])")
                now = time.time()
                if load:
                    load.keep_going(now - started)
                if now - last >= a.every:
                    last = now
                    stats = d.stats()
                    if not stats.get("canvas"):
                        say("  the stream is gone at %d s" % (now - started))
                        ended_early = True
                        break
                    rows = stats.get("rows") or {}
                    sample = {"s": round(now - started), "fps": rows.get("Framerate:"),
                              "size": rows.get("Resolution:"), "auto": stats_line(rows),
                              "enhancer": enhancer_of(rows),
                              "visibility": stats.get("visibility")}
                    result["samples"].append(sample)
                    gpus = nvidia_load() if not self.remote else []
                    if gpus:
                        result["gpu"].append(gpus)
                    say("  %4d s  %-14s %-22s %s%s" % (
                        sample["s"], sample["size"], sample["fps"], sample["auto"] or "",
                        "  [%s]" % sample["enhancer"] if a.enhance and sample["enhancer"] else ""))
                    # What the pass has so far survives a browser lost at minute nine.
                    save(tag, result)
                s = journal.parse(lines)
                # A climb need not sit out its time once this launch has said its word.
                if a.until_earned and (s["earned"] or s["sick"] or
                                       (s["held"] and s["rung"] == journal.RUNGS_M[-1])):
                    stop_on = True
                    break
                # Nor a pass that has made it ill, or that is about a client
                # too weak for its rung: the verdict is written.
                if (load or expect == "sick") and s["sick"]:
                    stop_on = True
                    break
                time.sleep(min(2, a.every))
        finally:
            if load:
                result["load"] = load.stop()
        if stop_on:
            say("  this launch has said what it had to")
            time.sleep(10)

        if not ended_early:
            result["overlayAtEnd"] = d.stats()
            try:
                d.screenshot(os.path.join(OUT, tag + ".png"))
            except Exception as e:  # a picture is a bonus: the pass is the numbers
                say("  no picture: %s" % e)
            d.stop()
        time.sleep(4)
        self.run.content_stop()
        self.shown = False

        # A page reloaded under us has an empty list: what was read before stands.
        final = d.json_eval("JSON.stringify(window.__mwCeilingLog || [])")
        if len(final) >= len(lines):
            lines = final
        after = read_memories(d, self.memories)
        s = journal.parse(lines)
        result.update(lines=lines, journal=s, after=after, uuid=uuid, endedEarly=ended_early,
                      seconds=round(time.time() - started))
        bad = journal.judge(s, expect, after.get(self.key), uuid, prepared=prepared)
        bad += journal.judge_guard(s, asked, a.refuse_above)
        if self.identifier:
            bad += journal.judge_rendezvous(after, self.identifier, WITNESS_KEY, self.witness,
                                            learned=a.learn == "1")
        if ended_early and not s["sick"]:
            bad.append("the stream ended by itself, and no line says why")
        shared = shared_gpu(result["gpu"])
        if shared:
            bad.append("the same GPU encoded and decoded: %s" % shared)
        if not any(x["auto"] for x in result["samples"]) and s["rung"] and s["rung"] > journal.BASE_M \
                and a.learn == "1":
            bad.append("above the base budget and the stats card never showed its line")
        if a.enhance:
            bad += enhancer_defects(result["samples"])
        if a.target == "vdisplay":
            self.wait_screens(baseline, result)
        result["defects"] = bad
        save(tag, result)
        for line in lines:
            say("  " + line)
        say("  after: " + self.said(after))
        say("%s: %s" % (tag, "clean" if not bad else "DEFECTS\n    " + "\n    ".join(bad)))
        return s, uuid, result

    def wait_screens(self, baseline, result):
        """The virtual display goes up to a minute after the page does. A screen
        that left the desktop with it is said: it does not come back by waiting."""
        names = {m[0] for m in baseline}
        for _ in range(30):
            now = {m[0] for m in self.monitors()}
            if now <= names:
                gone = names - now
                if gone:
                    say("  !! screen(s) gone from the desktop: %s" % " ".join(sorted(gone)))
                    result["screensGone"] = sorted(gone)
                return
            time.sleep(5)
        say("  !! the virtual display is still up after 150 s")
        result["virtualDisplayStuck"] = True

    def close(self):
        if self.shown:
            self.run.content_stop()
        if not self.remote:
            self.run.kiosk_stop()


def picture(a):
    """The moving picture alone, for a client nobody can drive from here.

    A browser in someone's hands launches when it launches: each virtual display
    that turns up gets the page, for as long as it lasts. `--max-launches` of them,
    or `--secs` in all.
    """
    run = importlib.import_module("run")
    monitors = importlib.import_module("cadence").monitors
    known = {m[0] for m in monitors()}
    say("waiting for a virtual display; on the desktop now: %s" % " ".join(sorted(known)))
    shown = 0
    end = time.time() + a.secs
    up = False
    try:
        while time.time() < end and shown < a.max_launches:
            made = [m for m in monitors() if m[0] not in known]
            if made and not up:
                x, y = made[0][1].split(",")
                w, h = made[0][2].split("x")
                os.environ["MW_BENCH_CONTENT_RECT"] = "%s,%s,%s,%s" % (x, y, w, h)
                run.content_start(a.page, probe=False)
                up = True
                say("%s the page is on %s" % (time.strftime("%H:%M:%S"), " ".join(made[0])))
            elif up and not made:
                run.content_stop()
                up = False
                shown += 1
                say("%s the virtual display is gone (%d of %d)" % (
                    time.strftime("%H:%M:%S"), shown, a.max_launches))
            time.sleep(3)
    finally:
        if up:
            run.content_stop()


def plan(a):
    """What this command would do, with nothing touched."""
    if a.mode == "picture":
        say("the moving picture (%s) on each new virtual display, %d at most, %d s at most" % (
            a.page, a.max_launches, a.secs))
        return
    if a.mode == "climb":
        say("climb: key %s once, then up to %d launches, each until it earns (at most %d s)%s" % (
            "prepared at %d M" % a.rung if a.rung else "cleared", a.max_launches, a.secs,
            ", none that could end after " + a.deadline if a.deadline else ""))
    else:
        how = ("the key kept as it is" if a.keep else
               "the key prepared at %s M" % a.rung if a.rung else "no key")
        say("one launch of %d s with %s, expected: %s" % (a.secs, how, a.expect))
    say("mw_auto_learn=%s%s, target %s%s%s%s, content %s" % (
        a.learn, ", mw_autostep=" + a.autostep if a.autostep else "", a.target,
        ", tile " + a.tile if a.tile else "",
        ", the page told it has a %s screen" % a.screen if a.screen else "",
        ", the Enhancer on (algorithm Auto)" if a.enhance else "", a.page))
    say("client: %s" % ("port %d, %s" % (a.client_port, a.client_url) if a.client_port
                        else "a kiosk on this machine, MW_BENCH_CLIENT_POS=%s MW_BENCH_CLIENT_LUID=%s" % (
                            os.environ.get("MW_BENCH_CLIENT_POS", ""),
                            os.environ.get("MW_BENCH_CLIENT_LUID", ""))))
    say("host: MW_BENCH_LOCAL_PORTS=%s%s" % (os.environ.get("MW_BENCH_LOCAL_PORTS", "(default)"),
                                             ", through the staging rendezvous" if a.rendezvous else ""))
    if a.refuse_above:
        say("the browser is made to say no above %d M" % a.refuse_above)


def main():
    # The page's lines say more than a Windows console's code page can: a pass whose
    # output goes to a file stopped on its first arrow, its results saved, its end lost.
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("mode", choices=["rung", "climb", "picture"])
    ap.add_argument("--tag", default="")
    ap.add_argument("--rung", type=int, default=0, help="prepare the key at this rung, in M")
    ap.add_argument("--keep", action="store_true", help="the key as the last launch left it")
    ap.add_argument("--secs", type=int, default=0, help="600 for a rung, 420 for a climb's launch")
    ap.add_argument("--every", type=int, default=20, help="seconds between two readings")
    ap.add_argument("--expect", choices=["hold", "sick", "journal", "silent", "any"], default="hold")
    ap.add_argument("--learn", choices=["1", "log", "0"], default="1", help="mw_auto_learn")
    ap.add_argument("--autostep", choices=["0", "1"], default="", help="mw_autostep, the page's default otherwise")
    ap.add_argument("--target", choices=["display", "vdisplay"], default="display")
    ap.add_argument("--tile", default="", help="the display tile, by its name")
    ap.add_argument("--gdi", default="", help="that tile's screen (DISPLAY1), for the page's place")
    ap.add_argument("--screen", default="", help="what the page is told its screen is: 3840x2160")
    ap.add_argument("--enhance", action="store_true",
                    help="the Enhancer on, algorithm Auto: a rung held with MWSR running")
    # band=1: the text has an end and the page is blank a fifth of the time; the
    # band still changes every frame, so a loaded stream stays loaded.
    ap.add_argument("--page", default="scroll.html?px=600&band=1",
                    help="the moving picture (content/)")
    ap.add_argument("--load-gpu", default="",
                    help="mw-gpu-load on this LUID (high:low), the client's GPU: to make it ill")
    ap.add_argument("--load-at", type=int, default=30, help="when the load starts, in seconds")
    ap.add_argument("--lang", default="", help="the page's language: fr, zh (English otherwise)")
    ap.add_argument("--client-port", type=int, default=0, help="a Chrome on another machine")
    ap.add_argument("--client-url", default="", help="the address it reaches this host at, never a loopback")
    ap.add_argument("--rendezvous", action="store_true",
                    help="open the page through the staging rendezvous, at this host's identifier")
    ap.add_argument("--refuse-above", type=int, default=0,
                    help="in M: decodingInfo answers no above this, to see the guard come down")
    ap.add_argument("--max-launches", type=int, default=7)
    ap.add_argument("--deadline", default="", help="HH:MM today: no launch starts that could end after it")
    ap.add_argument("--plan", action="store_true", help="say what would run, and run nothing")
    a = ap.parse_args()
    a.until_earned = a.mode == "climb"
    if a.mode == "picture":
        a.secs = a.secs or 1800
        if a.plan:
            plan(a)
        else:
            picture(a)
        return
    if not a.tag:
        ap.error("--tag: what this pass's results are called")
    if not a.secs:
        a.secs = 420 if a.mode == "climb" else 600
    if a.mode == "rung" and not (a.rung or a.keep) and a.expect in ("hold", "sick"):
        ap.error("rung: --rung, or --keep, or an --expect that needs neither")
    if a.expect == "sick" and a.learn != "1":
        ap.error("--expect sick: only a page that learns loses a rung (--learn 1)")
    if a.rung and a.rung not in journal.RUNGS_M:
        ap.error("no rung of %d M: %s" % (a.rung, journal.RUNGS_M))
    if a.target == "display" and not a.tile:
        ap.error("--tile: a display is chosen by its name, never by its rank")
    if a.target == "display" and not (a.gdi or os.environ.get("MW_BENCH_CONTENT_RECT")):
        ap.error("--gdi: the moving picture has to go on that tile's own screen")
    if a.load_gpu and a.client_port:
        ap.error("--load-gpu: the client's GPU is on this machine only")
    if a.plan:
        plan(a)
        return

    say(journal.__doc__.split("\n")[0])
    plan(a)
    b = Bench(a)
    try:
        if a.mode == "rung":
            _, _, result = b.one_launch(a.tag, a.rung, a.keep, a.expect)
            raise SystemExit(1 if result["defects"] else 0)
        keep = False
        deadline = deadline_at(a.deadline)
        for i in range(1, a.max_launches + 1):
            if deadline and time.time() + a.secs + LAUNCH_EXTRA_S > deadline:
                say("climb: no time for launch %d before %s: it stops here" % (i, a.deadline))
                return
            s, uuid, _ = b.one_launch("%s-%d" % (a.tag, i), a.rung if i == 1 else 0, keep, "any")
            keep = True
            if s["sick"]:
                say("climb: stopped ill at %s M on launch %d: %s" % (s["rung"], i, s["sick"]["reason"]))
                raise SystemExit(1)
            if s["rung"] == journal.RUNGS_M[-1] and s["held"]:
                say("climb: the top rung held on launch %d" % i)
                return
            if not s["limited"] and not s["earned"]:
                say("climb: %s M is not what holds this screen back: it stops there" % s["rung"])
                return
        say("climb: %d launches and the top not reached" % a.max_launches)
        raise SystemExit(1)
    finally:
        b.close()


if __name__ == "__main__":
    main()
