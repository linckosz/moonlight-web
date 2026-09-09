"""Turn a campaign's raw results into one self-contained HTML report.

    python report.py [--results results] [--out ../../bench-out/report.html]

Reads whatever the campaign produced — results/inventory.json, matrix.json,
native-bench.csv, probe-results.jsonl, passes.jsonl — and writes a single file
with no external resource of any kind: the charts are inline SVG computed here,
so the report opens on a machine with no network, which is what a bench report
has to do.

The report is LOCAL — bench-out/ is in .gitignore — but "local" is a policy, not
a property: a report is a single self-contained file, which is exactly the shape
of thing that gets attached to an issue, dropped in a chat or pasted into a
release note. So it is also SCRUBBED, by default and on the way out (see the
Redactor below): no address, no machine name, no account name, no home
directory, no rendezvous id, no token. What is left is what a bench report is
actually about — a codec, an encoder, a resolution and a number of milliseconds.

  --no-redact  writes the raw thing instead, for reading alone at one's desk.

Its point is not the numbers, it is the last section. Every anomaly the rules
below can recognise is printed with the evidence that triggered it and with a
proposed mitigation, so the report reads as a plan of action rather than as a
table somebody still has to interpret.
"""
import argparse
import csv
import html
import json
import os
import re
import socket
import statistics
from datetime import datetime

# ── Scrubbing ───────────────────────────────────────────────────────────────
#
# Every string on its way into the HTML goes through Redactor.text(), and the
# only escaper this module exposes (esc) calls it. That is deliberate: a rule
# that has to be remembered at each of forty interpolation sites is a rule that
# will be forgotten at the forty-first, and the one that leaks is always the one
# nobody thought carried an address — a driver's error message, the path in a
# provenance card, a note written by discover.ps1.
#
# Order matters below. A MAC address is read before an IPv6 one (they overlap),
# and home directories are rewritten before the account name is hunted on its
# own, so that C:\Users\someone\... does not become C:\Users\<user>\... twice.
_LOOPBACK = {"127.0.0.1", "0.0.0.0", "255.255.255.255", "::1"}

_RULES = [
    # Home directories, both spellings — the path is the point, the account is not.
    (re.compile(r"(?i)([A-Z]:[\\/]Users[\\/])[^\\/\s\"'<>|]+"), r"\1<user>"),
    (re.compile(r"(/(?:home|Users)/)[^/\s\"'<>|]+"), r"\1<user>"),
    # Hardware and network addresses. MAC first: it also matches as IPv6.
    (re.compile(r"\b(?:[0-9a-fA-F]{2}[:-]){5}[0-9a-fA-F]{2}\b"), "<mac>"),
    # Two IPv6 shapes. The compressed one is matched on its "::", which no clock
    # or version string carries; the long one needs four colon-separated groups,
    # which keeps 12:34:56 a timestamp.
    (re.compile(r"(?<![\w:.])(?:[0-9a-fA-F]{1,4}:)*[0-9a-fA-F]{0,4}::"
                r"(?:[0-9a-fA-F]{1,4}:?)*[0-9a-fA-F]{0,4}(?![\w:.])"), "<ipv6>"),
    (re.compile(r"(?<![\w:.])(?:[0-9a-fA-F]{1,4}:){3,7}[0-9a-fA-F]{1,4}(?![\w:.])"), "<ipv6>"),
    # Rendezvous: the host in the URL and the Crockford-32 id of 26 characters.
    (re.compile(r"\b[\w-]+\.moonlightweb\.top\b"), "<rendezvous-host>"),
    (re.compile(r"\b[0-9A-HJKMNP-TV-Z]{26}\b"), "<rendezvous-id>"),
    # Anything long enough to be a key, a token or a pairing secret. The cut is
    # at 32 so the 16-character binary digest of the provenance card survives:
    # that one identifies a build, which is the opposite of confidential.
    (re.compile(r"\b[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-"
                r"[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\b"), "<uuid>"),
    (re.compile(r"\b[0-9a-fA-F]{32,}\b"), "<token>"),
]

_IPV4 = re.compile(r"\b(?:\d{1,3}\.){3}\d{1,3}(?:/\d{1,2})?\b")


def _ipv4_sub(m):
    """Loopback and the wildcard say nothing about anybody's network, and
    blanking them would make half the diagnostic advice unreadable."""
    text = m.group(0)
    return text if text.split("/")[0] in _LOOPBACK else "<ip>"


class Redactor:
    """Rewrites the identifying parts of a string. Built once from the
    inventory, because the machine names are only knowable from it."""

    def __init__(self, enabled=True, inventory=None):
        self.enabled = enabled
        self.aliases = {}
        self._names = []

        # The fleet is renamed by position, which keeps the report readable
        # without naming anything: the order is the order of hosts.json, so the
        # person who owns the fleet can still map it and nobody else can.
        machines = (inventory or {}).get("machines") or []
        for i, m in enumerate(machines, 1):
            alias = "this machine" if m.get("kind") == "local" else f"host-{i}"
            self.aliases[str(m.get("id"))] = alias
            for field in ("id", "label", "sshAlias"):
                value = m.get(field)
                if isinstance(value, str) and len(value) >= 3:
                    # A label is a sentence: only its head is the name.
                    head = re.split(r"\s+[—-]\s+", value)[0].strip()
                    for candidate in {value, head}:
                        if len(candidate) >= 3:
                            self._names.append((candidate, alias))

        # This machine's own name, and the account running the campaign.
        for name in (socket.gethostname(), socket.gethostname().split(".")[0]):
            if name:
                self._names.append((name, "<host>"))
        for var in ("USERNAME", "USER", "LOGNAME"):
            who = os.environ.get(var)
            if who and len(who) >= 3:
                self._names.append((who, "<user>"))

        # Longest first, so "bench-mac-bench" is not half-replaced by "bench-mac".
        self._names.sort(key=lambda pair: -len(pair[0]))
        self._names = [(re.compile(re.escape(n), re.IGNORECASE), a)
                       for n, a in self._names]

    def alias(self, machine_id):
        return self.aliases.get(str(machine_id), str(machine_id)) if self.enabled \
            else str(machine_id)

    def text(self, value):
        s = str(value)
        if not self.enabled or not s:
            return s
        for pattern, replacement in _RULES:
            s = pattern.sub(replacement, s)
        s = _IPV4.sub(_ipv4_sub, s)
        for pattern, alias in self._names:
            s = pattern.sub(alias, s)
        return s


# Set by render(); the module-level escaper reads it. Nothing reaches the HTML
# without passing here.
REDACTOR = Redactor(enabled=False)


def esc(value):
    return html.escape(REDACTOR.text(value))

# ── Thresholds ──────────────────────────────────────────────────────────────
#
# For real hardware on a LAN at 1080p60. They are deliberately not applied to a
# VM, a machine with no GPU or a software encoder: a bench that fails those for
# being slow is reporting the obvious and burying the real findings.
THRESHOLDS = {
    "photon_median_ms": 45,
    "photon_p90_ms": 80,
    "discarded_share": 0.20,
    "reference_drift": 0.15,
}

GREEN, YELLOW, RED, GREY, BLACK = "green", "yellow", "red", "grey", "black"
FLAG_LABEL = {
    GREEN: "within threshold",
    YELLOW: "works, but off the mark",
    RED: "does not work",
    GREY: "not applicable",
    BLACK: "not run",
}

# Symptom -> what to do about it. Keyed by the rule that fired; the text is the
# same advice the playbook in docs/bench-campaign.md carries, kept here so the
# report stands on its own when it is read away from the repository.
MITIGATIONS = {
    "codec-fallback":
        "Check the log line `[Session] Per-request streaming settings:` against the `videoCodec` "
        "of the /start reply. Three filters silently change a codec: MediaTrack transports carry "
        "H.264 only (filterTransportsByCodec), the host may not advertise it "
        "(serverCodecModeSupport), and a fallback to H.264 also forces hdr_enabled and "
        "chroma_444_enabled off. Decide which one fired before treating this as a host bug.",
    "chroma-fallback":
        "4:4:4 is decided per codec, not per GPU: NVENC does it for H.264 and HEVC but never for "
        "AV1, and AMF and oneVPL not at all. The browser also refuses the profile it cannot "
        "decode (chroma444ClientCapability). Confirm which side declined before filing anything.",
    "hdr-fallback":
        "HDR needs three things at once: the OS switched to HDR for that display "
        "(scripts\\Set-DisplayHdr.ps1), a 10-bit path on the host, and a client that can decode "
        "and present it. A washed-out picture rather than a refusal usually means the signal "
        "information was not carried; a client on an SDR screen tone-maps it itself and proves "
        "nothing about the host.",
    "photon-slow":
        "Before blaming the pipeline, re-read the series: a bimodal distribution is the capture "
        "cadence, not the encoder — the flag falls either side of the next deadline. Then split "
        "the chain with `cdp.py perf`: host acquire/convert/encode against client "
        "handoff/decode/queue/render. Only a leg that moved between two passes is a finding.",
    "photon-discarded":
        "Read `saw` and `via` on the discarded samples: a flag absent from the picture, the wrong "
        "surface sampled and a surface that was never drawn all read as `timeout` and have "
        "nothing to do with each other. The classic cause is the session streaming a display the "
        "flag is not on — but there is one flag per monitor since 07/09/2026, so suspect a hidden "
        "page (frozen rAF) first.",
    "probe-unavailable":
        "Click-to-photon needs the flag on the host: Windows, macOS with Input Monitoring "
        "granted, or Linux in an X11 session. Where it is missing the pass is still worth "
        "running — the host stages and the client legs are measured all the same — but the "
        "end-to-end figure has to stay empty rather than be guessed at.",
    "reference-drift":
        "The reference replayed at the head and the tail of the matrix disagree: the machine was "
        "not in a steady state, so no comparison inside this matrix means anything. Find what "
        "moved (thermal throttling, another session, a virtual display becoming primary) and "
        "replay the whole matrix.",
    "pass-failed":
        "A pass that produced no frame is a real failure and the log is the only evidence. Keep "
        "the .err next to the CSV, and check first whether the captured display still exists — a "
        "virtual display adapter that became primary swallows the windows and the bench then "
        "captures a desktop that never changes.",
    "cadence-short":
        "The capture delivered fewer frames a second than the pass asked for. Split the two "
        "causes before calling it a limit: the display may simply not produce that many presents "
        "(a still desktop produces almost none, which is not a fault), or the encoder may not be "
        "keeping up — the encode p99 against the frame budget says which. Note that the declared "
        "refresh rate is not evidence: /api/native/status has been seen reporting 60 Hz for a "
        "display Windows drives at 164 Hz.",
    "encoder-refused":
        "The engine advertised the codec and then refused to initialise it. That gap between the "
        "capability table and the encoder is a real defect and the .err file next to the CSV has "
        "the driver's own words. Check whether it is the codec alone or the codec at that "
        "resolution, by re-running the same spec at the display's native size.",
    "no-native":
        "No MoonlightWeb answered on loopback: start one with --dev before a campaign. The dev "
        "instance starts empty, so each host has to be paired again in it, and its ports are "
        "whatever settings.json inherited — read the log, never assume 48080/48443.",
}


def read_json(path, default=None):
    try:
        with open(path, encoding="utf-8-sig") as f:
            return json.load(f)
    except Exception:
        return default


def read_jsonl(path):
    rows = []
    try:
        with open(path, encoding="utf-8-sig") as f:
            for line in f:
                line = line.strip()
                if line:
                    try:
                        rows.append(json.loads(line))
                    except json.JSONDecodeError:
                        pass
    except FileNotFoundError:
        pass
    return rows


def read_csv(path):
    try:
        with open(path, encoding="utf-8-sig", newline="") as f:
            return list(csv.DictReader(f))
    except FileNotFoundError:
        return []


def num(v):
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


# ── Charts: inline SVG, computed here ───────────────────────────────────────

def bar_chart(series, unit="ms", width=760, bar_h=26, threshold=None):
    """series: [(label, value, hi_or_None, flag)] — hi draws a p90 whisker."""
    series = [s for s in series if s[1] is not None]
    if not series:
        return '<p class="empty">no data</p>'
    peak = max(max(s[1], s[2] or 0) for s in series) or 1
    left, right = 190, 70
    plot = width - left - right
    height = len(series) * (bar_h + 8) + 24
    out = [f'<svg viewBox="0 0 {width} {height}" role="img" class="chart">']

    if threshold:
        x = left + plot * min(1.0, threshold / peak)
        out.append(f'<line x1="{x:.1f}" y1="8" x2="{x:.1f}" y2="{height - 16}" '
                   f'class="threshold"/>')
        out.append(f'<text x="{x + 4:.1f}" y="16" class="tick">{threshold} {unit}</text>')

    for i, (label, value, hi, flag) in enumerate(series):
        y = 24 + i * (bar_h + 8)
        w = plot * (value / peak)
        out.append(f'<text x="{left - 10}" y="{y + bar_h * 0.7:.0f}" class="lbl" '
                   f'text-anchor="end">{esc(label)}</text>')
        out.append(f'<rect x="{left}" y="{y}" width="{w:.1f}" height="{bar_h}" '
                   f'rx="3" class="bar bar--{flag}"/>')
        if hi:
            hx = left + plot * (hi / peak)
            out.append(f'<line x1="{hx:.1f}" y1="{y + 3}" x2="{hx:.1f}" y2="{y + bar_h - 3}" '
                       f'class="whisker"/>')
            out.append(f'<line x1="{left + w:.1f}" y1="{y + bar_h / 2:.0f}" x2="{hx:.1f}" '
                       f'y2="{y + bar_h / 2:.0f}" class="whisker"/>')
        text = f'{value:.1f}' + (f' / {hi:.0f}' if hi else '')
        out.append(f'<text x="{left + max(w, 2) + 8:.1f}" y="{y + bar_h * 0.7:.0f}" '
                   f'class="val">{text}</text>')
    out.append('</svg>')
    return "".join(out)


def histogram(samples, width=760, height=190, bins=18):
    """The shape of a click-to-photon series. It is normally BIMODAL — the flag
    falls either side of the next capture deadline — and a report that shows
    only a median hides the single most useful fact about the distribution."""
    samples = [s for s in samples if isinstance(s, (int, float))]
    if len(samples) < 4:
        return '<p class="empty">not enough samples to show a distribution</p>'
    lo, hi = min(samples), max(samples)
    if hi <= lo:
        hi = lo + 1
    step = (hi - lo) / bins
    counts = [0] * bins
    for s in samples:
        counts[min(bins - 1, int((s - lo) / step))] += 1
    peak = max(counts) or 1
    pad, base = 34, height - 26
    bw = (width - pad * 2) / bins
    out = [f'<svg viewBox="0 0 {width} {height}" role="img" class="chart">']
    for i, c in enumerate(counts):
        h = (base - 10) * (c / peak)
        x = pad + i * bw
        out.append(f'<rect x="{x:.1f}" y="{base - h:.1f}" width="{bw - 2:.1f}" '
                   f'height="{h:.1f}" rx="2" class="bar bar--green"/>')
    med = statistics.median(samples)
    mx = pad + (med - lo) / (hi - lo) * (width - pad * 2)
    out.append(f'<line x1="{mx:.1f}" y1="6" x2="{mx:.1f}" y2="{base}" class="threshold"/>')
    out.append(f'<text x="{mx + 4:.1f}" y="16" class="tick">median {med:.0f} ms</text>')
    out.append(f'<line x1="{pad}" y1="{base}" x2="{width - pad}" y2="{base}" class="axis"/>')
    out.append(f'<text x="{pad}" y="{height - 8}" class="tick">{lo:.0f} ms</text>')
    out.append(f'<text x="{width - pad}" y="{height - 8}" class="tick" '
               f'text-anchor="end">{hi:.0f} ms</text>')
    out.append('</svg>')
    return "".join(out)


# ── Analysis ────────────────────────────────────────────────────────────────

def analyse(results_dir):
    inventory = read_json(os.path.join(results_dir, "inventory.json"), {}) or {}
    matrix = read_json(os.path.join(results_dir, "matrix.json"), {}) or {}
    provenance = read_json(os.path.join(results_dir, "provenance.json"), {}) or {}
    bench = read_csv(os.path.join(results_dir, "native-bench.csv"))
    probes = read_jsonl(os.path.join(results_dir, "probe-results.jsonl"))
    browser = read_jsonl(os.path.join(results_dir, "passes.jsonl"))

    probe_by_label = {}
    for p in probes:
        probe_by_label.setdefault(p.get("label"), []).append(p)
    browser_by_label = {b.get("label"): b for b in browser}

    anomalies = []

    def flag_anomaly(kind, title, evidence, owner="Opus"):
        anomalies.append({
            "kind": kind, "title": title, "evidence": evidence,
            "mitigation": MITIGATIONS.get(kind, ""), "owner": owner,
        })

    perf_meaningful = True
    for m in inventory.get("machines", []):
        if m.get("kind") == "local":
            perf_meaningful = m.get("perfMeaningful", True)
            if not m.get("native"):
                flag_anomaly("no-native", "No MoonlightWeb answered on loopback",
                             "discover.ps1 found no /api/native/status", owner="Fable")
            elif not m["native"].get("latencyFlagSupported"):
                flag_anomaly("probe-unavailable", "No click-to-photon on this machine",
                             m["native"].get("latencyFlagReason") or "reason not reported",
                             owner="Fable")
            elif (m["native"].get("latencyFlagEnabled")
                  and not m["native"].get("latencyFlagActive")):
                flag_anomaly(
                    "probe-not-armed", "The flag is asked for but not armed",
                    "settings.json says latency_flag_enabled, yet no overlay is running: "
                    "the file was edited without restarting the server. Every click will "
                    "time out and read like a broken pipeline. Restart it.",
                    owner="Bruno")
            elif not m["native"].get("latencyFlagEnabled"):
                flag_anomaly(
                    "probe-off", "Click-to-photon is switched off",
                    "This machine can raise the flag but latency_flag_enabled is false in "
                    "settings.json. It is a file-only setting: set it and restart.",
                    owner="Bruno")

    # Pair every planned pass with whatever was measured for it.
    passes = []
    bench_by_index = {i: row for i, row in enumerate(bench)}
    runnable = [p for p in matrix.get("passes", []) if not p.get("skip")]
    for i, p in enumerate(matrix.get("passes", [])):
        entry = {
            "id": p.get("id"), "factor": p.get("factor"),
            "settings": p.get("settings", {}), "skip": p.get("skip") or "",
            "flag": GREY if p.get("skip") else BLACK,
            "bench": None, "probe": None, "browser": browser_by_label.get(p.get("id")),
            "notes": [],
        }
        if not p.get("skip") and p in runnable:
            entry["bench"] = bench_by_index.get(runnable.index(p))
        series = probe_by_label.get(p.get("id"), [])
        if series:
            entry["probe"] = series[-1]
        passes.append(entry)

    for e in passes:
        if e["skip"]:
            continue
        b, pr = e["bench"], e["probe"]
        flag = GREEN

        if b and b.get("error"):
            flag = RED
            e["notes"].append(f"pass failed: {b['error']}")
            # "advertised the codec, then refused to encode it" is a different
            # defect from "the pass produced nothing", and deserves its own
            # advice: the first is a gap between the capability table and the
            # driver, the second is usually the captured display having moved.
            kind = ("encoder-refused" if "could not initialize" in str(b["error"]).lower()
                    else "pass-failed")
            flag_anomaly(kind, f"{e['id']}: the encoder pass produced no frame", b["error"])
        elif b is None and pr is None:
            flag = BLACK
            e["notes"].append("not run")

        # Requested against negotiated — the whole reason this report exists.
        negotiated = (b or {}).get("negotiated", "") or ""
        want = e["settings"]
        if negotiated:
            low = negotiated.lower()
            wanted_codec = str(want.get("video_codec", "")).lower()
            if wanted_codec and wanted_codec not in low.replace(".", ""):
                flag = YELLOW if flag == GREEN else flag
                e["notes"].append(f"asked {wanted_codec}, engine reported: {negotiated}")
                flag_anomaly("codec-fallback",
                             f"{e['id']}: asked {wanted_codec}, got something else",
                             negotiated)
            if want.get("chroma_444_enabled") and "4:4:4" not in low:
                flag = YELLOW if flag == GREEN else flag
                e["notes"].append("4:4:4 requested, 4:2:0 delivered")
                flag_anomaly("chroma-fallback", f"{e['id']}: 4:4:4 fell back to 4:2:0", negotiated)
            if want.get("hdr_enabled") and "hdr" not in low:
                flag = YELLOW if flag == GREEN else flag
                e["notes"].append("HDR requested, SDR delivered")
                flag_anomaly("hdr-fallback", f"{e['id']}: HDR fell back to SDR", negotiated)

        # Asked-for cadence against the cadence the capture actually delivered.
        # This is the only place the resolution/fps sweep says anything: a pass
        # that ran to completion at a third of the requested rate looks perfect
        # in every other column.
        asked_fps = num(want.get("stream_fps"))
        got_fps = num((b or {}).get("captureFps"))
        if asked_fps and got_fps and got_fps < asked_fps * 0.9:
            flag = YELLOW if flag == GREEN else flag
            e["notes"].append(f"asked {asked_fps:.0f} fps, captured {got_fps:.0f}")
            flag_anomaly("cadence-short",
                         f"{e['id']}: asked {asked_fps:.0f} fps, the capture delivered {got_fps:.0f}",
                         f"encode p99 {(b or {}).get('encodeP99')} ms, "
                         f"{(b or {}).get('frames')} frames")

        if pr:
            n, of = pr.get("n") or 0, pr.get("of") or 0
            if of and (of - n) / of > THRESHOLDS["discarded_share"]:
                flag = YELLOW if flag == GREEN else flag
                e["notes"].append(f"{of - n} of {of} samples discarded")
                flag_anomaly("photon-discarded",
                             f"{e['id']}: {of - n} of {of} click-to-photon samples discarded",
                             ", ".join(str(x) for x in (pr.get("all") or [])[:8]))
            median = num(pr.get("median"))
            if perf_meaningful and median and median > THRESHOLDS["photon_median_ms"]:
                flag = YELLOW if flag == GREEN else flag
                e["notes"].append(f"median {median:.0f} ms over the {THRESHOLDS['photon_median_ms']} ms mark")
                flag_anomaly("photon-slow",
                             f"{e['id']}: click-to-photon median {median:.0f} ms",
                             f"p90 {pr.get('p90')} ms, min {pr.get('min')}, max {pr.get('max')}")
        e["flag"] = flag

    # Reproducibility: the reference replayed head and tail.
    head = next((e for e in passes if e["id"] == "ref-head"), None)
    tail = next((e for e in passes if e["id"] == "ref-tail"), None)
    drift = None
    if head and tail and head["bench"] and tail["bench"]:
        a, b = num(head["bench"].get("encodeMean")), num(tail["bench"].get("encodeMean"))
        if a and b:
            drift = abs(b - a) / a
            if drift > THRESHOLDS["reference_drift"]:
                flag_anomaly("reference-drift",
                             f"The reference drifted {drift * 100:.0f}% across the matrix",
                             f"head {a:.2f} ms, tail {b:.2f} ms encode mean")

    return inventory, matrix, passes, anomalies, drift, perf_meaningful, provenance


# ── Rendering ───────────────────────────────────────────────────────────────

CSS = """
:root { color-scheme: light dark;
  --bg:#fbfbfa; --card:#fff; --ink:#1a1a19; --ink2:#5c5c57; --line:#e4e2dd;
  --green:#2f8f4e; --yellow:#c98a12; --red:#c0392b; --grey:#9a9a94; --black:#4a4a46;
  --accent:#2a6fb5; }
@media (prefers-color-scheme: dark) { :root {
  --bg:#16171a; --card:#1e2024; --ink:#e9e9e6; --ink2:#a3a39d; --line:#2e3138;
  --green:#5fbf80; --yellow:#e0ab48; --red:#e07a6f; --grey:#7c7c76; --black:#6a6a64;
  --accent:#68a8e8; } }
* { box-sizing:border-box }
body { margin:0; background:var(--bg); color:var(--ink);
  font:15px/1.6 -apple-system,BlinkMacSystemFont,"Segoe UI",system-ui,sans-serif; }
.wrap { max-width:1080px; margin:0 auto; padding:40px 24px 80px }
h1 { font-size:1.75rem; margin:0 0 6px; letter-spacing:-.02em }
h2 { font-size:1.15rem; margin:44px 0 14px; padding-bottom:8px; border-bottom:1px solid var(--line) }
h3 { font-size:.95rem; margin:22px 0 8px; color:var(--ink2) }
.sub { color:var(--ink2); margin:0 0 26px }
.card { background:var(--card); border:1px solid var(--line); border-radius:10px;
  padding:18px 20px; margin:14px 0 }
.meta { display:grid; grid-template-columns:repeat(auto-fit,minmax(210px,1fr)); gap:12px 26px }
.meta div { font-size:.86rem } .meta b { display:block; color:var(--ink2); font-weight:500 }
table { width:100%; border-collapse:collapse; font-size:.85rem }
th,td { text-align:left; padding:7px 10px; border-bottom:1px solid var(--line); vertical-align:top }
th { color:var(--ink2); font-weight:600 }
.scroll { overflow-x:auto }
.dot { display:inline-block; width:9px; height:9px; border-radius:50%; margin-right:7px }
.dot--green{background:var(--green)} .dot--yellow{background:var(--yellow)}
.dot--red{background:var(--red)} .dot--grey{background:var(--grey)} .dot--black{background:var(--black)}
.legend { display:flex; flex-wrap:wrap; gap:16px; font-size:.82rem; color:var(--ink2); margin:10px 0 0 }
.chart { width:100%; height:auto; display:block; margin:6px 0 }
.chart .lbl { font-size:11px; fill:var(--ink2) }
.chart .val { font-size:11px; fill:var(--ink) }
.chart .tick { font-size:10px; fill:var(--ink2) }
.chart .axis { stroke:var(--line) }
.chart .threshold { stroke:var(--accent); stroke-width:1; stroke-dasharray:3 3 }
.chart .whisker { stroke:var(--ink2); stroke-width:1.5 }
.bar--green{fill:var(--green)} .bar--yellow{fill:var(--yellow)} .bar--red{fill:var(--red)}
.bar--grey{fill:var(--grey)} .bar--black{fill:var(--black)}
.anom { border-left:3px solid var(--yellow) }
.anom h3 { margin-top:0; color:var(--ink) }
.anom .ev { font-family:ui-monospace,SFMono-Regular,Menlo,monospace; font-size:.78rem;
  color:var(--ink2); background:var(--bg); padding:8px 10px; border-radius:6px; margin:8px 0;
  overflow-x:auto; white-space:pre-wrap }
.anom .own { font-size:.78rem; color:var(--ink2) }
.privacy { font-size:.84rem; color:var(--ink2); border-left:3px solid var(--green) }
.privacy--raw { border-left-color:var(--red) }
.empty { color:var(--ink2); font-style:italic; font-size:.86rem }
.note { color:var(--ink2); font-size:.8rem }
code { font-family:ui-monospace,SFMono-Regular,Menlo,monospace; font-size:.85em }
"""


def render(inventory, matrix, passes, anomalies, drift, perf_meaningful, provenance,
           out_path, redact=True):
    global REDACTOR
    REDACTOR = Redactor(enabled=redact, inventory=inventory)
    disp = matrix.get("display") or {}
    parts = [f"<style>{CSS}</style>", '<div class="wrap">']

    parts.append("<h1>MoonlightWeb — bench campaign</h1>")
    parts.append(f'<p class="sub">{esc(datetime.now().strftime("%d/%m/%Y %H:%M"))} · '
                 f'commit <code>{esc(str(inventory.get("commit") or "?"))}</code></p>')

    # Said up front, because the reader has to know which of the two files this
    # is before deciding what may be done with it.
    if redact:
        parts.append('<div class="card privacy"><b>Scrubbed for sharing.</b> Addresses, '
                     'machine and account names, home directories, rendezvous ids and '
                     'tokens are removed. The fleet is numbered in the order of '
                     '<code>hosts.json</code>, which is decipherable by whoever owns the '
                     'fleet and by nobody else.</div>')
    else:
        parts.append('<div class="card privacy privacy--raw"><b>Raw report — do not share.</b> '
                     'It carries addresses, machine names and paths. Re-run '
                     '<code>report.py</code> without <code>--no-redact</code> to get a '
                     'version fit to attach to anything.</div>')

    # Which binary produced these numbers, said before anything else. A campaign
    # of record runs on the CI artifact installed the way a user installs it; a
    # local build is for diagnosis. Both are legitimate, they are NOT comparable,
    # and a report that does not say which is a report nobody can trust twice.
    tier = provenance.get("tier")
    if tier:
        label = {
            "artifact": ("CI artifact — as a user gets it", GREEN),
            "installed": ("installed build — as a user runs it", GREEN),
            "local-build": ("local build — diagnosis, not a campaign of record", YELLOW),
        }.get(tier, (tier, GREY))
        parts.append(f'<div class="card"><span class="dot dot--{label[1]}"></span>'
                     f'<b>{esc(label[0])}</b><div class="meta" style="margin-top:10px">'
                     f'<div><b>Binary</b><code>{esc(str(provenance.get("exe") or "?"))}</code></div>'
                     f'<div><b>Digest</b><code>{esc(str(provenance.get("sha256") or "?"))}</code></div>'
                     f'<div><b>Version</b>{esc(str(provenance.get("fileVersion") or "?"))}</div>'
                     f'<div><b>Clip</b><code>{esc(str(provenance.get("clipSha256") or "none"))}</code></div>'
                     f'</div></div>')

    # ── The fleet, as it actually was ──
    parts.append('<div class="card"><div class="meta">')
    parts.append(f'<div><b>Captured display</b>{disp.get("width")}x{disp.get("height")} · '
                 f'{esc(str(disp.get("encoder_name") or "?"))}</div>')
    parts.append(f'<div><b>Codecs</b>{esc(", ".join(disp.get("codecs") or []) or "?")}</div>')
    parts.append(f'<div><b>HDR live</b>{"yes" if disp.get("hdr_active") else "no"}</div>')
    ref = matrix.get("reference", {})
    parts.append(f'<div><b>Reference</b>{ref.get("stream_height")}p{ref.get("stream_fps")} · '
                 f'{esc(str(ref.get("video_codec")))} · enhancer {esc(str(ref.get("video_enhancement")))}</div>')
    parts.append("</div></div>")

    parts.append("<h2>The fleet today</h2>")
    parts.append('<div class="scroll"><table><tr><th>Machine</th><th>Reachable</th><th>OS</th>'
                 "<th>Backends</th><th>Notes</th></tr>")
    for m in inventory.get("machines", []):
        flag = GREEN if m.get("reachable") else BLACK
        parts.append(
            f'<tr><td><span class="dot dot--{flag}"></span>'
            f'{esc(REDACTOR.alias(m.get("id")))}</td>'
            f'<td>{"yes" if m.get("reachable") else "no"}</td>'
            f'<td>{esc(str(m.get("os")))}</td>'
            f'<td>{esc(", ".join(m.get("backends") or []) or "—")}</td>'
            f'<td class="note">{esc("; ".join(m.get("notes") or []))}</td></tr>')
    parts.append("</table></div>")

    # ── Dashboard ──
    parts.append("<h2>Every pass at a glance</h2>")
    parts.append('<div class="scroll"><table><tr><th>Pass</th><th>Factor</th><th>Status</th>'
                 "<th>What was seen</th></tr>")
    for e in passes:
        detail = e["skip"] or "; ".join(e["notes"]) or FLAG_LABEL[e["flag"]]
        parts.append(f'<tr><td><span class="dot dot--{e["flag"]}"></span>'
                     f'<code>{esc(e["id"])}</code></td><td>{esc(e["factor"])}</td>'
                     f'<td>{FLAG_LABEL[e["flag"]]}</td>'
                     f'<td class="note">{esc(detail)}</td></tr>')
    parts.append("</table>")
    parts.append('<div class="legend">' + "".join(
        f'<span><span class="dot dot--{k}"></span>{v}</span>' for k, v in FLAG_LABEL.items()
    ) + "</div></div>")

    # ── Curves ──
    parts.append("<h2>Click to photon</h2>")
    photon = [(e["id"], num((e["probe"] or {}).get("median")),
               num((e["probe"] or {}).get("p90")), e["flag"])
              for e in passes if e["probe"]]
    if photon:
        parts.append('<div class="card">')
        parts.append(bar_chart(photon, "ms",
                               threshold=THRESHOLDS["photon_median_ms"] if perf_meaningful else None))
        parts.append('<p class="note">Bar = median, whisker = p90. The dashed line is the '
                     f'{THRESHOLDS["photon_median_ms"]} ms mark for real hardware on a LAN.</p></div>')
        ref_probe = next((e["probe"] for e in passes if e["id"] == "ref-head" and e["probe"]), None)
        if ref_probe and ref_probe.get("samples"):
            parts.append("<h3>Distribution of the reference series</h3>")
            parts.append('<div class="card">' + histogram(ref_probe["samples"]) +
                         '<p class="note">Two humps are normal: they are the capture cadence, the '
                         'flag falling either side of the next deadline — not the presenter.</p></div>')
    else:
        parts.append('<div class="card"><p class="empty">No click-to-photon series in this '
                     'campaign. Where the host cannot raise the flag that is expected, not a '
                     'failure — the reason is in the fleet table above.</p></div>')

    parts.append("<h2>Encoder</h2>")
    enc = [(e["id"], num((e["bench"] or {}).get("encodeMean")),
            num((e["bench"] or {}).get("encodeP99")), e["flag"])
           for e in passes if e["bench"]]
    parts.append('<div class="card"><h3>Encode time (mean, p99 whisker)</h3>' +
                 bar_chart(enc, "ms") + "</div>")
    size = [(e["id"], num((e["bench"] or {}).get("deltaKB")),
             num((e["bench"] or {}).get("deltaKBp95")), e["flag"])
            for e in passes if e["bench"]]
    parts.append('<div class="card"><h3>Delta frame size (mean KB, p95 whisker)</h3>' +
                 bar_chart(size, "KB") + "</div>")
    qp = [(e["id"], num((e["bench"] or {}).get("avgQp")), None, e["flag"])
          for e in passes if e["bench"] and num((e["bench"] or {}).get("avgQp"))]
    if qp:
        parts.append('<div class="card"><h3>Average QP (lower is sharper)</h3>' +
                     bar_chart(qp, "") +
                     '<p class="note">AV1 reports a q-index on 0–255 and is not comparable with '
                     'the others on this chart.</p></div>')

    # ── Per pass ──
    parts.append("<h2>Pass by pass</h2>")
    parts.append('<div class="scroll"><table><tr><th>Pass</th><th>Asked</th><th>Engine reported</th>'
                 "<th>encode ms</th><th>KB</th><th>QP</th><th>fps</th>"
                 "<th>photon med / p90</th></tr>")
    for e in passes:
        s, b, pr = e["settings"], e["bench"] or {}, e["probe"] or {}
        asked = (f'{s.get("stream_height")}p{s.get("stream_fps")} {s.get("video_codec")}'
                 f'{" 4:4:4" if s.get("chroma_444_enabled") else ""}'
                 f'{" HDR" if s.get("hdr_enabled") else ""}'
                 f'{" enh" if s.get("video_enhancement") == "on" else ""}'
                 f'{"" if s.get("mute_host_audio") else " unmuted"}')
        photon_txt = (f'{pr.get("median")} / {pr.get("p90")}' if pr else "—")
        parts.append(
            f'<tr><td><span class="dot dot--{e["flag"]}"></span><code>{esc(e["id"])}</code></td>'
            f'<td>{esc(asked)}</td><td class="note">{esc(str(b.get("negotiated") or e["skip"] or "—"))}</td>'
            f'<td>{esc(str(b.get("encodeMean") or "—"))}</td>'
            f'<td>{esc(str(b.get("deltaKB") or "—"))}</td>'
            f'<td>{esc(str(b.get("avgQp") or "—"))}</td>'
            f'<td>{esc(str(b.get("captureFps") or "—"))}</td>'
            f"<td>{esc(photon_txt)}</td></tr>")
    parts.append("</table></div>")

    if drift is not None:
        colour = RED if drift > THRESHOLDS["reference_drift"] else GREEN
        parts.append(f'<p class="note"><span class="dot dot--{colour}"></span>'
                     f'Reference drift across the matrix: {drift * 100:.1f}% '
                     f'(threshold {THRESHOLDS["reference_drift"] * 100:.0f}%).</p>')

    # ── The point of the whole thing ──
    parts.append("<h2>What to do about it</h2>")
    if not anomalies:
        parts.append('<div class="card"><p class="empty">Nothing tripped a rule. That is a '
                     'result, not an absence of one — but read the pass table anyway: the rules '
                     'only know what they were taught.</p></div>')
    for a in anomalies:
        parts.append('<div class="card anom">')
        parts.append(f'<h3>{esc(a["title"])}</h3>')
        if a["evidence"]:
            parts.append(f'<div class="ev">{esc(str(a["evidence"]))}</div>')
        parts.append(f'<p>{esc(a["mitigation"])}</p>')
        parts.append(f'<p class="own">Suggested owner: <b>{esc(a["owner"])}</b> · '
                     f'rule <code>{esc(a["kind"])}</code></p>')
        parts.append("</div>")

    parts.append('<p class="note" style="margin-top:40px">Bench report — never committed '
                 '(<code>bench-out/</code> is ignored). It describes the state of one fleet '
                 'on one afternoon; the scrubbing above is what makes it safe to hand to '
                 'somebody, and it is worth a read before doing so.</p>')
    parts.append("</div>")

    doc = ('<!doctype html><html lang="en"><head><meta charset="utf-8">'
           '<meta name="viewport" content="width=device-width,initial-scale=1">'
           "<title>MoonlightWeb bench campaign</title></head><body>"
           + "".join(parts) + "</body></html>")
    os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
    with open(out_path, "w", encoding="utf-8") as f:
        f.write(doc)
    return out_path


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=os.path.join(here, "results"))
    ap.add_argument("--out", default=os.path.join(here, "..", "..", "bench-out", "report.html"))
    ap.add_argument("--no-redact", dest="redact", action="store_false",
                    help="keep addresses, machine names and paths (for reading alone)")
    ns = ap.parse_args()

    inventory, matrix, passes, anomalies, drift, perf, prov = analyse(ns.results)
    path = render(inventory, matrix, passes, anomalies, drift, perf, prov, ns.out,
                  redact=ns.redact)
    print(f"report written to {os.path.abspath(path)}")
    print(f"  {len(passes)} passes, {len(anomalies)} anomalies, "
          f"{'scrubbed' if ns.redact else 'RAW — do not share'}")


if __name__ == "__main__":
    main()
