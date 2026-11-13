"""A bench kiosk with the owner's own session, opened by the owner's link.

From the build that makes this machine's address no reason for privilege, a page
on https://127.0.0.1 holds no session of its own: a bench page that needs the
host's session gets it as its owner does, by the link `--print-link` makes.

That link is the key to the host. It goes one way only: the instance's exe
writes it on its standard output, which is joined straight to the standard
input of `cdp.py nav-stdin`, which hands it to the kiosk's page. It is never in
this process's memory, on a command line, in the environment, in a file, in an
exception or in what is printed. The page drops it from its address once it has
used it (checked: key_gone). Never the production instance of this machine.

    python owner_kiosk.py open   --api-port 8080 --cdp-port 9333 [--origin https://127.0.0.1:8443] [--lan-only]
    python owner_kiosk.py logout --cdp-port 9333       (before the kiosk is closed)
    python owner_kiosk.py clean                        (once it is: its profile goes)

The kiosk itself is started by the caller (kiosk.ps1 -Url about:blank
-ChromeProfile .chrome-owner -DebugPort N), and closed by it between `logout`
and `clean`, in a finally whatever happened. Prints yes/no only.
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mw_local  # noqa: E402

CDP = os.path.join(HERE, "cdp.py")
NO_WINDOW = 0x08000000 if sys.platform == "win32" else 0
# This machine's production instance: its link is its owner's real key.
PROD_PORTS = (48080, 48443)
PROD_DIRS = tuple(os.path.normcase(os.path.join(os.environ.get(v, ""), "MoonlightWeb"))
                  for v in ("ProgramFiles", "ProgramFiles(x86)") if os.environ.get(v))
PROFILE = os.path.join(HERE, ".chrome-owner")


class Refused(RuntimeError):
    """Not an instance this may make a link for."""


def is_prod(exe, port):
    """Whether @p exe on @p port is this machine's production instance: its port,
    or an exe installed under Program Files\\MoonlightWeb."""
    if int(port) in PROD_PORTS:
        return True
    return os.path.normcase(os.path.dirname(os.path.abspath(exe or ""))) in PROD_DIRS


# The DEV edition's exe names: a bench instance without --dev is one of these.
DEV_NAMES = ("moonlightwebdev.exe", "moonlightwebdev", "moonlightweb-dev")


def is_bench(exe, dev):
    """The positive rule: an instance started with --dev, or the DEV edition's exe.
    A MoonlightWeb.exe without --dev is a production instance wherever it lies,
    whatever its ports."""
    return bool(dev) or os.path.basename(exe or "").lower() in DEV_NAMES


def _check(exe, dev, port):
    if is_prod(exe, port) or not is_bench(exe, dev):
        raise Refused("not a bench instance (production, or no --dev): no link is made for it")


def bench_instance(port):
    """(exe, dev) of the bench instance on @p port: found as mw_local finds it, its
    name checked, the command in its bytes; never a production one."""
    found = mw_local.instance(port)
    if not found:
        raise Refused("nothing of this machine listens on port %d" % int(port))
    exe, dev = found
    _check(exe, dev, port)
    if not mw_local.is_server_exe(exe) or not mw_local.knows(exe, b"print-link"):
        raise Refused("the exe on port %d is not one that makes a link" % int(port))
    return exe, dev


def _cdp(cdp_port, *args, stdin=None, timeout=60, cdp=CDP):
    r = subprocess.run([sys.executable, cdp, "--port", str(int(cdp_port))] + list(args), stdin=stdin,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=timeout,
                       creationflags=NO_WINDOW)
    return r.stdout.decode("utf-8", "replace").strip()


def open_owner(api_port, cdp_port, origin="", lan_only=False, cdp=CDP, _instance_for_tests=None, timeout=90):
    """Send the kiosk on @p cdp_port to the owner's link of the instance on
    @p api_port. True when the page went there. (_instance_for_tests: an (exe,
    dev) given instead of the one found, for the tests alone; its name is checked
    all the same.)"""
    if _instance_for_tests is not None:
        exe, dev = _instance_for_tests
        if not mw_local.is_server_exe(exe):
            raise Refused("not the server's exe")
    else:
        exe, dev = bench_instance(api_port)
    _check(exe, dev, api_port)
    env = {k: v for k, v in os.environ.items() if k not in ("MW_LAN_ONLY", "MW_DOMAIN", "MW_RENDEZVOUS_URL")}
    if lan_only:
        env["MW_LAN_ONLY"] = "1"
    maker = subprocess.Popen([exe] + (["--dev"] if dev else []) + ["--print-link"], stdin=subprocess.DEVNULL,
                             stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, env=env,
                             cwd=os.path.dirname(exe) or None, creationflags=NO_WINDOW)
    # The maker's output goes to the navigator's input, and through nothing else.
    nav = subprocess.Popen([sys.executable, cdp, "--port", str(int(cdp_port)), "nav-stdin"] + ([origin] if origin else []),
                           stdin=maker.stdout, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                           creationflags=NO_WINDOW)
    maker.stdout.close()
    try:
        out, _ = nav.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        nav.kill()
        maker.kill()
        return False
    try:
        maker.wait(timeout=10)
    except subprocess.TimeoutExpired:
        maker.kill()
    return out.decode("ascii", "replace").strip() == "navigated"


def key_gone(cdp_port, cdp=CDP, wait=15.0):
    """True when the page's address no longer names a key: the page drops it once
    used. Only the boolean comes back.

    Asked again for up to @p wait seconds: navigating returns once the page is
    committed, and the page takes the key off its address only when its script
    has loaded (linkKey.js), or the page in between answers nothing at all."""
    import time
    deadline = time.monotonic() + wait
    while True:
        if _cdp(cdp_port, "eval", "!/[#&?]k=/.test(location.href)", cdp=cdp) == "True":
            return True
        if time.monotonic() >= deadline:
            return False
        time.sleep(0.5)


LOGOUT = ("(async () => { try { const m = await import('/js/api/BackendClient.js'); "
          "await m.BackendClient.logout(); return 'done'; } catch (e) { return 'failed'; } })()")


def logout(cdp_port, cdp=CDP):
    """The page ends its own session: the host revokes it. Its status only."""
    return _cdp(cdp_port, "eval", LOGOUT, cdp=cdp) == "done"


def chromes_on_profile():
    """How many chrome.exe have the owner kiosk's profile on their command line,
    or None when that cannot be told (no answer, an error, too long)."""
    try:
        if sys.platform == "win32":
            r = subprocess.run(["powershell", "-NoProfile", "-NonInteractive", "-Command",
                                "@(Get-CimInstance Win32_Process -Filter \"Name='chrome.exe'\" -ErrorAction Stop | "
                                "Where-Object { $_.CommandLine -like '*\\.chrome-owner*' }).Count"],
                               stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                               timeout=60, creationflags=NO_WINDOW)
        else:
            r = subprocess.run(["pgrep", "-c", "-f", r"\.chrome-owner"], stdin=subprocess.DEVNULL,
                               stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        return None
    out = r.stdout.decode("ascii", "replace").strip()
    return int(out) if out.isdigit() else None


def remove_profile(path=PROFILE):
    """Remove the owner kiosk's profile: this fixed folder only, and only once it
    is known that no Chrome runs on it. (removed, why)."""
    path = os.path.abspath(path)
    if os.path.normcase(path) != os.path.normcase(PROFILE):
        return False, "not the owner kiosk's profile"
    if not os.path.isdir(path):
        return True, "absent"
    running = chromes_on_profile()
    if running is None:
        return False, "cannot tell whether a Chrome runs on it"
    if running:
        return False, "a Chrome still runs on it"
    import shutil
    shutil.rmtree(path, ignore_errors=True)
    return not os.path.exists(path), "removed" if not os.path.exists(path) else "left"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["open", "logout", "clean"])
    ap.add_argument("--api-port", type=int, default=8080)
    ap.add_argument("--cdp-port", type=int, default=0)
    ap.add_argument("--origin", default="")
    ap.add_argument("--lan-only", action="store_true")
    a = ap.parse_args()
    if a.what in ("open", "logout") and not a.cdp_port:
        ap.error("--cdp-port is needed")
    if a.what == "open":
        try:
            went = open_owner(a.api_port, a.cdp_port, a.origin, a.lan_only)
        except Refused as e:
            print("refused: %s" % e)
            return 2
        gone = key_gone(a.cdp_port) if went else False
        print("owner page opened: %s; key gone from its address: %s" % (went, gone))
        return 0 if went and gone else 1
    if a.what == "logout":
        out = logout(a.cdp_port)
        print("session ended by the page: %s" % out)
        return 0 if out else 1
    removed, why = remove_profile()
    print("owner kiosk profile removed: %s (%s)" % (removed, why))
    return 0 if removed else 1


if __name__ == "__main__":
    sys.exit(main())
