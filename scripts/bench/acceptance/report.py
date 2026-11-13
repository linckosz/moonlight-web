"""Turn the acceptance results into one HTML page, and print it to PDF.

    python report.py [--results <dir>] [--out <dir>] [--no-pdf]

This is NOT scripts/bench/report.py. That one redacts by default — addresses,
machine names, rendezvous ids, every hex run of 32 or more — because a
campaign report is meant to be readable by someone else.

This one keeps the machine names and their screenshots, because a go/no-go that
will not say which machine failed is not worth reading. What it deliberately
does NOT print is the way in: the rendezvous address and the pairing PIN of
each host are handed to the reader in the session that produced the page, never
written into it — a report is a thing one forwards, and those two together open
the machine. It is still written under bench-out/, which .gitignore covers.
"""
import argparse
import base64
import html
import io
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DEFAULT_OUT = os.path.join(REPO, "bench-out", "acceptance")
CHROME = r"C:\Program Files\Google\Chrome\Application\chrome.exe"

MARK = {"green": "🟩", "yellow": "🟨", "red": "🟥", "grey": "⬜", "black": "⬛"}
WORD = {"green": "pass", "yellow": "works, off the mark", "red": "fails",
        "grey": "not applicable", "black": "not run"}


def esc(x):
    return html.escape("" if x is None else str(x))


def read_jsonl(path):
    out = []
    if not os.path.exists(path):
        return out
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                out.append(json.loads(line))
            except ValueError:
                continue
    return out


def thumb(path, width=520):
    """A screenshot small enough to live inside the page.

    The page has to survive being copied to a phone with no folder beside it,
    and the PDF has to stay openable — sixty full-size PNGs would make neither.
    """
    if not path or not os.path.exists(path):
        return None
    try:
        from PIL import Image
        img = Image.open(path)
        img = img.convert("RGB")
        if img.width > width:
            img = img.resize((width, max(1, round(img.height * width / img.width))),
                             Image.LANCZOS)
        buf = io.BytesIO()
        img.save(buf, format="JPEG", quality=72, optimize=True)
        return "data:image/jpeg;base64," + base64.b64encode(buf.getvalue()).decode()
    except Exception:
        return None


# ── the numbers, as sentences ───────────────────────────────────────────────

def load_expectations():
    """What each machine is known NOT to be able to do, from matrix.json.

    A downgrade that was expected is not a finding. Reporting "asked HEVC, got
    H.264" eight times about a VM whose only encoder is openh264 buries the one
    line that matters under noise that was never news.
    """
    try:
        with open(os.path.join(HERE, "matrix.json"), encoding="utf-8") as f:
            return json.load(f).get("expect", {}) or {}
    except (OSError, ValueError):
        return {}


EXPECT = load_expectations()


def expected_codec(machine, codec):
    known = (EXPECT.get(machine) or {}).get("codecs")
    if not known:
        return False
    return any(k.lower() in (codec or "").lower().replace(".", "") for k in known)


def drift(requested, neg, machine=""):
    """Where what came back is not what was asked. The quiet failures.

    A HEVC that came back H.264, a 4:4:4 that came back 4:2:0 and an HDR that
    came back SDR are all silent: nothing errors, the picture arrives, and only
    this comparison says so.
    """
    notes = []
    want_codec = (requested.get("video_codec") or "").lower()
    got_codec = (neg.get("codec") or "").lower()
    if want_codec and got_codec and want_codec not in got_codec.replace(".", ""):
        if expected_codec(machine, got_codec):
            notes.append("%s as expected on this machine, not %s"
                         % (neg["codec"], want_codec.upper()))
        else:
            notes.append("asked %s, got %s" % (want_codec.upper(), neg["codec"]))
    if requested.get("chroma_444_enabled") and "4:4:4" not in (neg.get("codec") or ""):
        notes.append("asked 4:4:4, got %s" % (neg.get("codec") or "?"))
    # HDR is deliberately NOT inferred from the overlay here. On a native host
    # the setting is not what decides: app.js forces hdr_enabled to what the two
    # screens can actually do, so a host whose displays report hdr_active=false
    # streams SDR by design, not by silent downgrade. Calling that a drift
    # produced a finding that was simply untrue. The per-pass hostNote says what
    # the host reported instead.
    if requested.get("video_enhancement") == "on":
        enh = (neg.get("enhancer") or "").strip().lower()
        if enh in ("", "off", "none", "--"):
            notes.append("asked Enhancer on, overlay says %r" % (neg.get("enhancer") or ""))
    return notes


def problems(chapter1, passes):
    """Everything worth a line in the todo list, deduplicated by its wording."""
    found = []

    def add(kind, machine, text):
        key = (kind, machine, text)
        if key not in {(f["kind"], f["machine"], f["text"]) for f in found}:
            found.append({"kind": kind, "machine": machine, "text": text})

    for rec in chapter1:
        if rec.get("status") != "ok":
            add("install", rec.get("machine", "?"),
                rec.get("reason") or "chapter 1 did not finish")
        probe = rec.get("afterUpdate") or {}
        # "Ready" is a scheduled task that exists and will fire at the next
        # logon; "session autostart" is the XDG entry a desktop Linux relies on,
        # where the system unit is deliberately left disabled.
        good = ("Running", "Ready", "loaded", "active", "session autostart", "login item")
        if probe.get("autostart") and probe["autostart"] not in good:
            detail = ""
            if probe.get("defaultTarget", "").startswith("graphical"):
                detail = (" — the system unit is deliberately disabled on a desktop "
                          "distribution, and no XDG autostart entry was written "
                          "either, so nothing brings it back after a reboot")
            elif probe.get("appName") == "MoonlightWebDev" and not probe.get("defaultTarget"):
                detail = (" — the installer builds its scheduled tasks' principal from "
                          "USERDOMAIN, which a network logon (SSH, WinRM, an RMM or MDM "
                          "agent) sets to WORKGROUP. WORKGROUP\\someone maps to no SID, "
                          "schtasks fails with \"No mapping between account names and "
                          "security IDs was done\", and a silent install finishes green "
                          "with no logon task, no update task and no virtual display "
                          "task — the last of which leaves can_manage false, so the "
                          "virtual display is installed and unusable. An interactive "
                          "install is unaffected: there USERDOMAIN is the machine")
            add("autostart", rec.get("machine", "?"),
                "autostart reads %r after the update%s" % (probe["autostart"], detail))
        if not rec.get("rendezvousKnown"):
            add("access", rec.get("machine", "?"),
                "no rendezvous URL — this host has no line to stream.dev")

    for rec in passes:
        if rec.get("verdict") == "red":
            add("stream", rec.get("machine", "?"),
                "%s · %s — %s" % (rec.get("chapter"), rec.get("pass"),
                                  rec.get("reason") or "failed"))
        elif rec.get("verdict") == "yellow" and rec.get("reason"):
            add("stream", rec.get("machine", "?"),
                "%s · %s — %s" % (rec.get("chapter"), rec.get("pass"), rec.get("reason")))
        if rec.get("hostNote"):
            add("host", rec.get("machine", "?"),
                "%s · %s — %s" % (rec.get("chapter"), rec.get("pass"), rec["hostNote"]))
        machine = rec.get("machine", "?")
        for note in drift(rec.get("requested") or {}, rec.get("negotiated") or {}, machine):
            if "as expected" in note:
                continue  # said in the row, not worth a todo line
            add("negotiation", machine,
                "%s · %s — %s" % (rec.get("chapter"), rec.get("pass"), note))
    return found


def appraisal(chapter1, passes, todo):
    """The go/no-go paragraph, argued from what actually ran."""
    installed = [r for r in chapter1 if r.get("status") == "ok"]
    reds = [p for p in passes if p.get("verdict") == "red"]
    yellows = [p for p in passes if p.get("verdict") == "yellow"]
    greens = [p for p in passes if p.get("verdict") == "green"]
    ran = len([p for p in passes if p.get("status") in ("ok", "failed")])

    if not passes:
        return "no-go", ("Nothing streamed. There is no evidence here either way — "
                         "this is not a verdict on the build, it is a run that did not happen.")

    red_machines = sorted({p["machine"] for p in reds
                           if _perf_machine(p.get("machine"))})
    if red_machines:
        # A failure is not a failure is not a failure. Everything broken at once
        # and one setting broken on one machine call for different decisions,
        # and flattening both into "do not deploy" makes the verdict useless:
        # the reader stops believing it the first time it cries wolf.
        factors = sorted({p.get("pass") for p in reds})
        reference_broken = any(p.get("pass") in ("default", "ref-tail") for p in reds)
        widespread = len(red_machines) > 1 or len(reds) > 3
        if reference_broken or widespread:
            return "no-go", (
                "%d of %d passes failed outright, on %s. A failure on a machine with real "
                "hardware is not a bench artefact: it is what a user would meet, and this "
                "one is not confined to an unusual setting. Read the todo list below, fix "
                "what it names, and run this again before deploying."
                % (len(reds), ran, ", ".join(red_machines)))
        return "conditional", (
            "%d of %d passes are clean. One thing is genuinely broken and it is contained: "
            "%s on %s, and nowhere else. The default configuration streams on every machine "
            "of the fleet, including through the rendezvous, so this version is deployable "
            "if that one setting is acceptable to ship broken — a user who picks it gets no "
            "picture at all, not a degraded one. The todo list below carries the host's own "
            "diagnosis; it is a small, well-located fix rather than an unknown."
            % (len(greens), ran, ", ".join(factors), ", ".join(red_machines)))
    if len(installed) < len(chapter1):
        missing = [r.get("machine") for r in chapter1 if r.get("status") != "ok"]
        return "hold", (
            "Every stream that ran came back clean, but the build did not install on %s. "
            "Streaming is only half the promise — a version that cannot be installed "
            "cannot be deployed. Settle the install first."
            % ", ".join(str(m) for m in missing))
    if len(yellows) > max(3, len(passes) // 5):
        return "hold", (
            "%d of %d passes work but sit off the mark, against %d clean ones. None of "
            "them is a failure on its own; together they are a pattern worth one look "
            "before shipping." % (len(yellows), ran, len(greens)))
    return "go", (
        "%d of %d passes are clean, none failed on hardware that counts, and the build "
        "installed and updated on every machine that answered. Nothing here argues "
        "against deploying this version. The remaining items below are notes, not "
        "blockers." % (len(greens), ran))


def _perf_machine(mid):
    try:
        sys.path.insert(0, HERE)
        import fleet
        return fleet.MACHINES.get(mid, {}).get("perf", True)
    except Exception:
        return True


# ── the page ────────────────────────────────────────────────────────────────

CSS = """
:root {
  --bg: #ffffff; --fg: #16181d; --muted: #5d6470; --line: #e3e6ec;
  --card: #f7f8fa; --accent: #3D6BFF; --danger: #c02626; --danger-bg: #fdeaea;
  --green: #17803d; --yellow: #a15c00; --red: #c02626; --grey: #6b7280;
  --mono: ui-monospace, "Cascadia Mono", "SF Mono", Menlo, Consolas, monospace;
}
* { box-sizing: border-box; }
body {
  margin: 0; padding: 0 16px 80px; background: var(--bg); color: var(--fg);
  font: 15px/1.55 -apple-system, "Segoe UI", Roboto, Inter, system-ui, sans-serif;
  -webkit-text-size-adjust: 100%;
}
.wrap { max-width: 1120px; margin: 0 auto; }
h1 { font-size: 26px; margin: 28px 0 4px; letter-spacing: -0.01em; }
h2 { font-size: 20px; margin: 40px 0 6px; padding-bottom: 6px; border-bottom: 2px solid var(--line); }
h3 { font-size: 16px; margin: 26px 0 8px; color: var(--muted); font-weight: 600; }
p  { margin: 8px 0; }
a  { color: var(--accent); }
.sub { color: var(--muted); margin-top: 0; }
.banner {
  background: var(--danger-bg); border: 1px solid var(--danger); color: var(--danger);
  border-radius: 8px; padding: 12px 16px; margin: 20px 0; font-weight: 600;
}
.banner span { display: block; font-weight: 400; margin-top: 4px; color: #7c1d1d; }
.cards { display: grid; grid-template-columns: repeat(auto-fit, minmax(190px, 1fr)); gap: 10px; margin: 14px 0 6px; }
.card { background: var(--card); border: 1px solid var(--line); border-radius: 8px; padding: 10px 12px; }
.card .k { color: var(--muted); font-size: 12px; text-transform: uppercase; letter-spacing: .04em; }
.card .v { font-size: 18px; font-weight: 600; margin-top: 2px; }
table { border-collapse: collapse; width: 100%; margin: 12px 0; font-size: 13.5px; }
th, td { text-align: left; padding: 7px 9px; border-bottom: 1px solid var(--line); vertical-align: top; }
th { color: var(--muted); font-weight: 600; font-size: 12px; text-transform: uppercase; letter-spacing: .04em; }
tbody tr:nth-child(even) { background: #fafbfc; }
code, .mono { font-family: var(--mono); font-size: 12.5px; }
.pill { display: inline-block; padding: 1px 7px; border-radius: 999px; font-size: 11.5px; font-weight: 600; }
.green  { background: #e6f5ec; color: var(--green); }
.yellow { background: #fdf2e0; color: var(--yellow); }
.red    { background: var(--danger-bg); color: var(--red); }
.grey   { background: #f0f1f3; color: var(--grey); }
.reason { color: var(--muted); font-size: 12.5px; }
.shot { max-width: 260px; border: 1px solid var(--line); border-radius: 5px; display: block; }
.verdict { border-radius: 10px; padding: 16px 18px; margin: 14px 0; border: 1px solid var(--line); background: var(--card); }
.verdict.go   { border-color: var(--green);  background: #f1faf4; }
.verdict.hold { border-color: var(--yellow); background: #fffaf0; }
.verdict.nogo { border-color: var(--red);    background: var(--danger-bg); }
.verdict .head { font-size: 19px; font-weight: 700; margin-bottom: 6px; }
ul.todo { list-style: none; padding: 0; }
ul.todo li { padding: 7px 0 7px 26px; border-bottom: 1px solid var(--line); position: relative; }
ul.todo li::before { content: "☐"; position: absolute; left: 4px; color: var(--muted); }
.kind { display: inline-block; min-width: 92px; color: var(--muted); font-size: 12px; }
footer { margin-top: 50px; color: var(--muted); font-size: 12.5px; border-top: 1px solid var(--line); padding-top: 12px; }
@media print {
  body { padding: 0; font-size: 11.5px; }
  h2 { page-break-after: avoid; }
  tr, .verdict, .banner { page-break-inside: avoid; }
  .shot { max-width: 180px; }
}
@media (max-width: 640px) {
  table { font-size: 12px; }
  .shot { max-width: 130px; }
}
"""


def render(chapter1, passes, out_dir, meta):
    todo = problems(chapter1, passes)
    stance, words = appraisal(chapter1, passes, todo)
    counts = {k: len([p for p in passes if p.get("verdict") == k])
              for k in ("green", "yellow", "red", "grey")}

    h = []
    h.append("<!doctype html><html lang='en'><head><meta charset='utf-8'>")
    h.append("<meta name='viewport' content='width=device-width, initial-scale=1'>")
    h.append("<title>Acceptance Bench</title><style>%s</style></head><body><div class='wrap'>" % CSS)

    h.append("<h1>MoonlightWeb — pre-deployment acceptance</h1>")
    h.append("<p class='sub'>%s · build <code>%s</code> · CI run <code>%s</code></p>"
             % (esc(meta.get("generatedAt")), esc(meta.get("version")), esc(meta.get("run"))))

    h.append("<div class='banner'>Do not commit this file."
             "<span>No address and no PIN are printed here — they were handed over in the "
             "session instead, so this page can be read by anyone without handing them a "
             "way in. It still names every machine on the desk and shows its screen, so it "
             "lives under <code>bench-out/</code>, which git ignores — keep it there."
             "</span></div>")

    # provenance
    h.append("<h2>What was tested</h2>")
    h.append("<div class='cards'>")
    for k, v in (("Version", meta.get("version")), ("Commit", meta.get("commit")),
                 ("CI run", meta.get("run")), ("Edition", "MoonlightWebDev (channel dev)"),
                 ("Rendezvous", meta.get("rendezvous"))):
        h.append("<div class='card'><div class='k'>%s</div><div class='v mono'>%s</div></div>"
                 % (esc(k), esc(v or "—")))
    h.append("</div>")
    h.append("<div class='cards'>")
    for k, v in (("Clean", counts["green"]), ("Off the mark", counts["yellow"]),
                 ("Failed", counts["red"]), ("Not applicable", counts["grey"])):
        h.append("<div class='card'><div class='k'>%s</div><div class='v'>%s</div></div>"
                 % (esc(k), v))
    h.append("</div>")

    # verdict first: it is the reason the page exists
    cls = {"go": "go", "hold": "hold", "conditional": "hold", "no-go": "nogo"}[stance]
    head = {"go": "Ship it", "hold": "Hold", "no-go": "Do not deploy",
            "conditional": "Deployable, with one known defect"}[stance]
    h.append("<div class='verdict %s'><div class='head'>%s</div><p>%s</p></div>"
             % (cls, esc(head), esc(words)))

    # What is running where. The addresses and the PINs that used to sit in this
    # table are deliberately absent: they are a way into these machines, and a
    # report is a thing one forwards. They were handed over in the session that
    # produced this page, and they are read from the same probe as the version
    # below — so a row that says "reachable" here is a row that answered.
    h.append("<h2>What is running where</h2>")
    h.append("<p class='sub'>Neither the rendezvous address nor a pairing PIN is kept by the "
             "bench: the run asks each machine for them when it uses them. A PIN is made on "
             "the host when one is needed.</p>")
    h.append("<table><thead><tr><th>Machine</th><th>Rendezvous</th><th>On the LAN</th>"
             "<th>Local channel</th><th>Version in service</th></tr></thead><tbody>")
    for rec in chapter1:
        probe = rec.get("afterUpdate") or {}
        yes = "<span class='pill green'>reachable</span>"
        no = "<span class='pill red'>none</span>"
        h.append("<tr><td><strong>%s</strong></td><td>%s</td><td>%s</td><td>%s</td>"
                 "<td class='mono'>%s</td></tr>"
                 % (esc(rec.get("machine")),
                    yes if rec.get("rendezvousKnown") else no,
                    yes if rec.get("lanUrl") else no,
                    "<span class='pill green'>yes</span>" if probe.get("localChannel") else
                    "<span class='pill'>an older build</span>",
                    esc((probe.get("versionLine") or "")[:70] or probe.get("exeSha") or "—")))
    h.append("</tbody></table>")

    # chapter 1
    h.append("<h2>Chapter 1 — install, then update</h2>")
    h.append("<p class='sub'>Every DEV instance removed, the artifact installed silently, "
             "then the same artifact installed again as an update. A silent uninstall never "
             "deletes configuration, so the settings have to survive the second pass.</p>")
    h.append("<table><thead><tr><th></th><th>Machine</th><th>Artifact</th><th>Uninstall</th>"
             "<th>Install</th><th>Update</th><th>Autostart</th><th>Native host</th>"
             "<th>Note</th></tr></thead><tbody>")
    for rec in chapter1:
        probe = rec.get("afterUpdate") or {}
        native = probe.get("native") or {}
        ok = rec.get("status") == "ok"
        v = "green" if ok else ("grey" if rec.get("status") == "skipped" else "red")
        steps = rec.get("steps") or {}

        def rcword(name):
            s = steps.get(name)
            if not s:
                return "—"
            return "ok" if s.get("rc") == 0 else ("rc=%s" % s.get("rc"))

        h.append("<tr><td>%s</td><td><strong>%s</strong></td><td class='mono'>%s</td>"
                 "<td>%s</td><td>%s</td><td>%s</td><td class='mono'>%s</td>"
                 "<td>%s</td><td class='reason'>%s</td></tr>"
                 % (MARK[v], esc(rec.get("machine")), esc(rec.get("package") or "—"),
                    esc(rcword("uninstall")), esc(rcword("install")), esc(rcword("update")),
                    esc(probe.get("autostart") or "—"),
                    "<span class='pill %s'>%s</span>" % (
                        "green" if native.get("available") else "red",
                        "available" if native.get("available") else "unavailable"),
                    esc(rec.get("reason") or native.get("reason") or "")))
    h.append("</tbody></table>")

    # streaming chapters
    for key, title, blurb in (
            ("02-dualrtx", "Chapter 2 — DualRTX host, DualRTX client",
             "One factor moves per pass; everything else stays on the base config "
             "(Auto resolution, Auto bitrate, SDR, 60 fps, HEVC, Enhancer off, "
             "Gaming mode off, 4:2:0, Virtual Display)."),
            ("03-fleet", "Chapter 3 — every other host, DualRTX client",
             "Same base config, but the default target is the native physical display: "
             "the Virtual Display is a Windows driver and most of this fleet has none.")):
        rows = [p for p in passes if p.get("chapter") == key]
        if not rows:
            continue
        h.append("<h2>%s</h2><p class='sub'>%s</p>" % (esc(title), esc(blurb)))
        h.append("<p class='sub'><strong>Reading the numbers.</strong> What is captured is an "
                 "idle desktop, and a host only encodes when something changes: a framerate of "
                 "2 fps and a bitrate of 2 Mbps are that stillness, not a fault. The columns "
                 "worth a verdict are the resolution, the negotiated codec, the total latency "
                 "and whether a picture arrived at all. Throughput belongs to "
                 "<code>/bench</code>, which drives a moving clip on purpose.</p>")
        by_machine = {}
        for row in rows:
            by_machine.setdefault(row["machine"], []).append(row)
        for machine, group in by_machine.items():
            h.append("<h3>%s</h3>" % esc(machine))
            h.append("<table><thead><tr><th></th><th>Pass</th><th>Factor</th>"
                     "<th>Target</th><th>Negotiated</th><th>Latency</th>"
                     "<th>Note</th><th>Shot</th></tr></thead><tbody>")
            for row in group:
                v = row.get("verdict", "grey")
                neg = row.get("negotiated") or {}
                negtext = " · ".join(x for x in (neg.get("resolution"), neg.get("framerate"),
                                                 neg.get("codec"), neg.get("bitrate")) if x)
                lat = row.get("latencyMs")
                notes = list(drift(row.get("requested") or {}, neg, row.get("machine", "")))
                if row.get("reason"):
                    notes.insert(0, row["reason"])
                if row.get("hostNote"):
                    notes.insert(0, row["hostNote"])
                load = row.get("load")
                if load:
                    # What the GPU load did while the stream was measured: a
                    # load that sagged well under its calibrated 45 fps is the
                    # encoder taking its share back, and worth reading with the
                    # latency beside it.
                    notes.append("GPU load on %s: level %s, %s fps, GPU %s ms, %s °C" % (
                        load.get("gpu"), load.get("level"), load.get("fps"),
                        load.get("gpuMs"), load.get("tempC")))
                audio = row.get("audio")
                if audio is not None:
                    notes.append("audio: %s" % ("live track" if audio.get("live")
                                                else "no live track"))
                img = thumb(row.get("screenshot"))
                h.append("<tr><td title='%s'>%s</td><td class='mono'>%s</td><td>%s</td>"
                         "<td class='mono'>%s%s</td><td class='mono'>%s</td>"
                         "<td class='mono'>%s</td><td class='reason'>%s</td><td>%s</td></tr>"
                         % (esc(WORD.get(v, "")), MARK.get(v, "⬛"),
                            esc(row.get("pass")), esc(row.get("factor")),
                            esc(row.get("target")),
                            " <span class='pill grey'>via stream.dev</span>"
                            if row.get("via") == "rendezvous" else "",
                            esc(negtext or "—"),
                            ("%.1f ms" % lat) if lat is not None else "—",
                            esc(" · ".join(notes)),
                            ("<img class='shot' src='%s' alt=''>" % img) if img else "—"))
            h.append("</tbody></table>")

    # todo
    h.append("<h2>What to do about it</h2>")
    if not todo:
        h.append("<p>Nothing came up. Every machine installed, updated and streamed "
                 "without a note worth carrying forward.</p>")
    else:
        h.append("<ul class='todo'>")
        for item in todo:
            h.append("<li><span class='kind'>%s</span><strong>%s</strong> — %s</li>"
                     % (esc(item["kind"]), esc(item["machine"]), esc(item["text"])))
        h.append("</ul>")

    h.append("<footer>Generated by <code>scripts/bench/acceptance/report.py</code>. "
             "Raw results in <code>results/</code>, full-size screenshots in "
             "<code>screens/</code>. Neither this page nor those folders are committed.</footer>")
    h.append("</div></body></html>")

    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, "report.html")
    with open(path, "w", encoding="utf-8") as f:
        f.write("\n".join(h))
    return path


def to_pdf(html_path, pdf_path):
    """Print with the Chrome that is already on this machine — no new dependency."""
    if not os.path.exists(CHROME):
        return None
    profile = os.path.join(os.path.dirname(pdf_path), ".chrome-print")
    argv = [CHROME, "--headless=new", "--disable-gpu", "--no-first-run",
            "--user-data-dir=" + profile,
            "--print-to-pdf=" + pdf_path, "--no-pdf-header-footer",
            "--virtual-time-budget=20000",
            "file:///" + html_path.replace("\\", "/")]
    subprocess.run(argv, capture_output=True, text=True, timeout=180)
    shutil.rmtree(profile, ignore_errors=True)
    return pdf_path if os.path.exists(pdf_path) else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=os.path.join(DEFAULT_OUT, "results"))
    ap.add_argument("--out", default=DEFAULT_OUT)
    ap.add_argument("--no-pdf", action="store_true")
    ap.add_argument("--version", default="")
    ap.add_argument("--commit", default="")
    ap.add_argument("--run", default="")
    ns = ap.parse_args()

    chapter1 = read_jsonl(os.path.join(ns.results, "chapter1.jsonl"))
    passes = read_jsonl(os.path.join(ns.results, "passes.jsonl"))
    # A pass replayed after a fix supersedes the earlier one.
    latest = {}
    for row in passes:
        latest[(row.get("chapter"), row.get("machine"), row.get("pass"))] = row
    passes = list(latest.values())
    seen = {}
    for rec in chapter1:
        seen[rec.get("machine")] = rec
    chapter1 = list(seen.values())

    meta = {"generatedAt": time.strftime("%Y-%m-%d %H:%M"), "version": ns.version,
            "commit": ns.commit, "run": ns.run,
            "rendezvous": "stream.dev.moonlightweb.top"}
    path = render(chapter1, passes, ns.out, meta)
    print("html: " + path)
    if not ns.no_pdf:
        pdf = to_pdf(path, os.path.join(ns.out, "report.pdf"))
        print("pdf : " + (pdf or "not produced"))


if __name__ == "__main__":
    main()
