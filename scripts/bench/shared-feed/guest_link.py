"""A share's guest link and PIN: from the owner's page to a guest's kiosk, and nowhere else.

The benches of this folder open a row of the share board and have a guest join
it the way a person does: the link, the PIN, the Join button. The link names
the share's token and the PIN is the share's, so neither is printed, written,
put on a command line or in the environment:

- the row opens as the board's own button opens it, BackendClient.shareActivate
  in the owner's page, which signs the owner's proof; its answer stays in this
  process's memory;
- the guest's kiosk opens on a blank page in its own throwaway profile, and the
  link reaches it through a pipe to `cdp.py nav-stdin`;
- the PIN is typed into the page by Input.insertText, whose failure says only
  that it failed;
- whatever happens, the row is closed (the host revokes its link and PIN), the
  kiosk closes and its profile goes;
- at the end, `proof` counts guest links and PINs in what the run left: the
  results, its own output, the --dev's logs. Counts only, 0 expected.
"""
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, BENCH)
sys.path.insert(0, os.path.join(BENCH, "acceptance"))
import drive  # noqa: E402

CDP = os.path.join(BENCH, "cdp.py")
KIOSK = os.path.join(BENCH, "kiosk.ps1")
# A guest link names its token in its path on the LAN, in its fragment through
# the introduction server.
LINK_RE = re.compile(r"/p/[A-Za-z0-9_-]{8,}|#t=[A-Za-z0-9_-]{8,}")
# A PIN as a log or a record would carry it, by its shape, never by its value.
PIN_RE = re.compile(r"""\bpin\b["']?\s*[:=]\s*["']?\d{4,}""", re.IGNORECASE)

# Every token this run was given: the final count looks for them too. In memory only.
_tokens = []


class Share:
    """One open row: its slot and, in memory only, its token and PIN."""

    def __init__(self, slot):
        self.slot = slot
        self.token = ""
        self.pin = ""
        # What may be printed of the answer: its status and where the row leads.
        self.info = {}


def activate(owner, slot, target=None):
    """Open the row @p slot from the owner's page, as the board's button does;
    @p target adds to its body (host_uuid, app_id). Raises drive.PassFailed
    with the HTTP status and whether a link and a PIN came, nothing else."""
    body = dict(target or {})
    body["ttl_secs"] = 3600
    js = """(async () => {
        try {
            const m = await import('/js/api/BackendClient.js');
            const r = await m.BackendClient.shareActivate(%d, %s);
            return JSON.stringify({status: 200, url: (r && r.url) || '', pin: (r && r.pin) || '',
                app_id: r && r.app_id, host_uuid: r && r.host_uuid, state: r && r.state});
        } catch (e) {
            return JSON.stringify({status: (e && (e.status || e.statusCode)) || 0, url: '', pin: ''});
        }
    })()""" % (int(slot), json.dumps(body))
    try:
        raw = owner.eval(js)
    except (Exception, SystemExit):
        raise drive.PassFailed("slot %d: the owner's page did not answer" % slot) from None
    try:
        act = json.loads(raw) if isinstance(raw, str) else {}
    except ValueError:
        act = {}
    raw = None
    link = str(act.get("url") or "")
    share = Share(slot)
    m = re.search(r"/p/([^/?#]+)", link) or re.search(r"#t=([^&]+)", link)
    share.token = m.group(1) if m else ""
    share.pin = str(act.get("pin") or "")
    share.info = {k: act.get(k) for k in ("status", "app_id", "host_uuid", "state")}
    if share.token:
        _tokens.append(share.token)
    if not share.token or not share.pin:
        raise drive.PassFailed("slot %d did not open: status %s, link %s, PIN %s"
                               % (slot, act.get("status", 0), bool(link), bool(share.pin)))
    return share


def deactivate(owner, slot):
    """Close the row: the host revokes its link and PIN, and ends a guest's
    stream at once. True when the host said so."""
    js = """(async () => {
        try {
            const m = await import('/js/api/BackendClient.js');
            await m.BackendClient.shareDeactivate(%d);
            return 'closed';
        } catch (e) { return 'refused'; }
    })()""" % int(slot)
    try:
        return owner.eval(js) == "closed"
    except (Exception, SystemExit):
        return False


def open_kiosk(port, profile, rect, extra=()):
    """The guest's Chrome on a blank page, in the throwaway profile @p profile."""
    x, y, w, h = rect
    subprocess.run(["powershell", "-NoProfile", "-File", KIOSK, "-Url", "about:blank",
                    "-X", str(x), "-Y", str(y), "-W", str(w), "-H", str(h),
                    "-DebugPort", str(port), "-ChromeProfile", profile] + list(extra),
                   capture_output=True, text=True)


def go(port, base_url, share):
    """The guest's kiosk opens the link, handed over a pipe to cdp.py nav-stdin."""
    url = base_url.rstrip("/") + "/p/" + share.token
    try:
        went = subprocess.run([sys.executable, CDP, "--port", str(port), "nav-stdin"],
                              input=url + "\n", capture_output=True, text=True, timeout=90)
        ok = went.stdout.strip() == "navigated"
    except (Exception, SystemExit):
        ok = False
    url = None
    if not ok:
        raise drive.PassFailed("guest %d: the kiosk did not open the link" % share.slot)


def enter_pin(d, share):
    """The PIN into the guest's page, then its form sent. The PIN travels in one
    DevTools call whose failure is reported without it."""
    focused = d.eval("(() => { const i = document.querySelector('.player-pin-input'); "
                     "if (!i) return 0; i.value = ''; i.focus(); return 1; })()")
    if focused != 1:
        raise drive.PassFailed("guest %d: no PIN field to type into" % share.slot)
    try:
        d.c.call("Input.insertText", text=share.pin)
    except (Exception, SystemExit):
        raise drive.PassFailed("guest %d: the PIN could not be typed" % share.slot) from None
    d.eval("(() => { const i = document.querySelector('.player-pin-input'); "
           "i.form.requestSubmit(); return 1; })()")


def close_kiosk(profile):
    """Close the guest's Chrome and remove its profile (the page of the link
    keeps the token in its history). '' when done, otherwise a warning."""
    # The profile folder is the end of the path: not .chrome-guest-23 for .chrome-guest-2.
    pattern = "\\\\" + re.escape(profile) + "(\\s|\"|$)"
    subprocess.run(["powershell", "-NoProfile", "-Command",
                    "Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" | "
                    "Where-Object { $_.CommandLine -match '%s' } | "
                    "ForEach-Object { Stop-Process -Id $_.ProcessId -Force }" % pattern.replace("'", "''")],
                   capture_output=True, text=True)
    path = os.path.join(BENCH, profile)
    for _ in range(5):
        if not os.path.exists(path):
            return ""
        shutil.rmtree(path, ignore_errors=True)
        if not os.path.exists(path):
            return ""
        time.sleep(1)
    return "%s is still there and holds a guest link in its history: remove it once Chrome is closed" % profile


class Tee:
    """This run's output, kept in memory as well, for the final count."""

    def __init__(self, stream):
        self.stream = stream
        self.kept = []

    def write(self, s):
        self.kept.append(s)
        return self.stream.write(s)

    def flush(self):
        self.stream.flush()

    def __getattr__(self, name):
        return getattr(self.stream, name)


def keep_output():
    """From now on, what this process prints, warnings and tracebacks
    included, is also kept for `proof`."""
    if not isinstance(sys.stdout, Tee):
        sys.stdout = Tee(sys.stdout)
    if not isinstance(sys.stderr, Tee):
        sys.stderr = Tee(sys.stderr)
    return sys.stdout


def _count(text):
    links = len(LINK_RE.findall(text))
    # This run's tokens, also where no /p/ or #t= stands before them (each counted once).
    for t in _tokens:
        if t:
            links += text.count(t) - text.count("/p/" + t) - text.count("#t=" + t)
    return links, len(PIN_RE.findall(text))


def proof(results_dir, since, dev_logs=True):
    """How many guest links and PINs the run left in its results, its output
    and the --dev's logs written since @p since (a time.time()). Printed as
    counts only; returns (links, pins)."""
    links = pins = 0
    files = [p for p in glob.glob(os.path.join(results_dir, "**", "*"), recursive=True)
             if os.path.isfile(p) and os.path.getsize(p) < 64 * 1024 * 1024]
    if dev_logs:
        logs = os.path.join(os.environ.get("APPDATA", ""), "MoonlightWeb", "MoonlightWeb-dev", "logs")
        files += [p for p in glob.glob(os.path.join(logs, "*.log")) if os.path.getmtime(p) >= since]
    for p in files:
        try:
            with open(p, encoding="latin-1") as f:
                a, b = _count(f.read())
        except OSError:
            continue
        links += a
        pins += b
    for out in (sys.stdout, sys.stderr):
        if isinstance(out, Tee):
            a, b = _count("".join(out.kept))
            links += a
            pins += b
    print("guest links and PINs left by the run: links %d, PINs %d (%d files%s)"
          % (links, pins, len(files), ", the --dev's logs" if dev_logs else ""), flush=True)
    return links, pins
