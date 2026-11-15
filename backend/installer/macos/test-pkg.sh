#!/bin/bash
# ===========================================================================
#  MoonlightWeb — install the macOS .pkg on this Mac and check what its
#  postinstall leaves.
#
#  For a CI runner, where the logged-in user can run sudo without a password:
#  the package is installed five times (sudo installer), each time with what
#  the installer pane would have left in /tmp, and every check that does not
#  hold fails the run.
#
#    1. A hand-off file the user wrote, 0600, with Start at login off and an
#       agreement text holding a quote, a backslash, a tab, a line break, a
#       control character and a non-ASCII letter. A bundle with the app's
#       identifier and executable name is left in ~/Applications, running.
#    2. The same hand-off file, writable by everyone (0666).
#    3. A hand-off file the user wrote, 0600, with Start at login on and a
#       5000-character agreement text.
#    4. No hand-off file, and the log file replaced by a link.
#    5. No hand-off file, and the log directory replaced by a link.
#
#  No hand-off asks for Internet Access. The app the postinstall starts gets
#  MW_LAN_ONLY=1 from the user's session, and its own log has to say so after
#  every run, with no [UPNP] line; otherwise the app is stopped and the run
#  ends there.
#
#  Usage: bash backend/installer/macos/test-pkg.sh <package.pkg> <prod|dev>
# ===========================================================================
set -uo pipefail

PKG="${1:?usage: test-pkg.sh <package.pkg> <prod|dev>}"
case "${2:-}" in
    prod) NAME=MoonlightWeb; IDENT=com.moonlightweb.server; LABEL=com.moonlightweb.agent ;;
    dev)  NAME=MoonlightWebDev; IDENT=com.moonlightweb.server.dev; LABEL=com.moonlightweb.agent.dev ;;
    *) echo "usage: test-pkg.sh <package.pkg> <prod|dev>"; exit 2 ;;
esac
[ -f "$PKG" ] || { echo "no package at $PKG"; exit 2; }

ME="$(id -un)"
MYUID="$(id -u)"
HANDOFF=/tmp/moonlightweb-provisioning.plist
LOG_DIR=/Library/Logs/MoonlightWeb
LOG="$LOG_DIR/postinstall.log"
APP="/Applications/$NAME.app"
EXE="$APP/Contents/MacOS/$NAME"
DATA="$HOME/Library/Application Support/MoonlightWeb"
APPDATA="$DATA/$NAME"
APPLOG="$APPDATA/logs/moonlightweb.log"
AGENT="$HOME/Library/LaunchAgents/$LABEL.plist"
OLD_APP="$HOME/Applications/$NAME.app"
OLD_EXE="$OLD_APP/Contents/MacOS/$NAME"
WORK="$(mktemp -d)"
FAILS=0
CHECKS=0

pass() {
    echo "ok    $*"
    CHECKS=$((CHECKS + 1))
}
fail() {
    echo "FAIL  $*"
    echo "::error title=macOS .pkg::$*"
    CHECKS=$((CHECKS + 1))
    FAILS=$((FAILS + 1))
}
check() { # <what> <command...>
    local what="$1"
    shift
    if "$@"; then pass "$what"; else fail "$what"; fi
}
has_line() { grep -qF -- "$2" <<<"$1"; } # <text> <line>
lacks_line() { ! grep -qF -- "$2" <<<"$1"; }
gone() { ! kill -0 "$1" 2>/dev/null; }
stop_app() { pkill -u "$MYUID" -f "^$EXE( |\$)"; }

# The postinstall prepares the home of the console user; that has to be us.
CONSOLE="$(stat -f %Su /dev/console)"
if [ "$CONSOLE" != "$ME" ]; then
    echo "::error title=macOS .pkg::the console user is '$CONSOLE', not $ME: nothing to check"
    exit 1
fi
if [ -e "$APP" ] || [ -e "$DATA" ] || [ -e "$LOG_DIR" ]; then
    echo "::error title=macOS .pkg::this Mac already has $APP, $DATA or $LOG_DIR: it needs a fresh one"
    exit 1
fi
launchctl setenv MW_LAN_ONLY 1

# What the pane leaves: internet, consent, autostart. Binary when asked, so
# that the text can carry a control character.
write_handoff() { # <mode> <autostart> <consent file> [binary]
    rm -f "$HANDOFF"
    if ! python3 - "$HANDOFF" "$2" "$3" "${4:-xml}" <<'PY'; then
import plistlib, sys
path, autostart, consent, fmt = sys.argv[1], sys.argv[2] == "true", sys.argv[3], sys.argv[4]
text = open(consent, encoding="utf-8").read()
with open(path, "wb") as f:
    plistlib.dump({"internet": False, "consent": text, "autostart": autostart}, f,
                  fmt=plistlib.FMT_BINARY if fmt == "binary" else plistlib.FMT_XML)
PY
        fail "the hand-off file could not be written"
        rm -f "$HANDOFF"
        return 1
    fi
    chmod "$1" "$HANDOFF"
}

# provisioning.json is taken by the app as it starts: a copy is kept of each
# one the postinstall writes. APPLOG_FROM is where the app's log stood.
install_pkg() { # <run>
    rm -f "$WORK/provisioning.json" "$WORK/stop"
    APPLOG_FROM=0
    [ -f "$APPLOG" ] && APPLOG_FROM="$(wc -l <"$APPLOG" | tr -d ' ')"
    (
        while [ ! -e "$WORK/stop" ]; do
            [ -f "$APPDATA/provisioning.json" ] && cp "$APPDATA/provisioning.json" "$WORK/provisioning.json" 2>/dev/null
            sleep 0.02
        done
    ) &
    local watcher=$! rc
    sudo installer -pkg "$PKG" -target / >"$WORK/installer-$1.txt" 2>&1
    rc=$?
    sleep 3
    touch "$WORK/stop"
    wait "$watcher" 2>/dev/null
    if [ "$rc" -eq 0 ]; then
        pass "run $1: installer(8) succeeded"
    else
        fail "run $1: installer(8) failed ($rc)"
        cat "$WORK/installer-$1.txt"
    fi
}

this_run() { # the log from the last "== postinstall <date> ==" line on
    awk '/^== postinstall / && !/^== postinstall done ==$/ { buf = "" } { buf = buf $0 "\n" }
         END { printf "%s", buf }' "$LOG" 2>/dev/null
}

# provisioning.json as the postinstall wrote it: JSON, LAN only, and the
# agreement text as the postinstall keeps it (trailing line breaks dropped,
# 4096 characters at most, control characters other than tab and line
# break dropped).
provisioning_is() { # <consent file, or "" for none>
    python3 - "$WORK/provisioning.json" "$1" <<'PY'
import json, re, sys
path, consent = sys.argv[1], sys.argv[2]
try:
    got = json.load(open(path, encoding="utf-8"))
except Exception as e:
    print("      provisioning.json: %s" % e)
    sys.exit(1)
want = ""
if consent:
    want = open(consent, encoding="utf-8").read().rstrip("\n")[:4096]
    want = re.sub(r"[\x00-\x08\x0b-\x1f\x7f]", "", want)
ok = got.get("internet_access_authorized") is False and got.get("consent_message") == want
if not ok:
    print("      provisioning.json: %r" % got)
    print("      agreement text wanted: %r" % want)
sys.exit(0 if ok else 1)
PY
}

owned_by_me() { # nothing under the app data and the agent belongs to anyone else
    local others
    others="$(find "$DATA" "$AGENT" ! -user "$ME" -print 2>/dev/null)"
    [ -z "$others" ] || echo "      not $ME's: $others"
    [ -z "$others" ]
}

# The app the postinstall started, from its own log since this install: the
# LAN-only line, and no [UPNP] line. Then its settings.json, written by then:
# Internet Access is not on.
app_log_since() { tail -n +"$((APPLOG_FROM + 1))" "$APPLOG" 2>/dev/null; }
started_lan_only() {
    local i
    for i in $(seq 1 45); do
        app_log_since | grep -qF "[LAN-only] MW_LAN_ONLY is set" && break
        sleep 1
    done
    sleep 5 # what it does in its first seconds is in the log too
    if ! app_log_since | grep -qF "[LAN-only] MW_LAN_ONLY is set"; then
        echo "      no LAN-only line in $APPLOG since this install"
        return 1
    fi
    if app_log_since | grep -qF "[UPNP]"; then
        app_log_since | grep -F "[UPNP]" | head -5 | sed 's/^/      /'
        return 1
    fi
    [ -f "$APPDATA/settings.json" ] && ! grep -q '"internet_access_enabled": *true' "$APPDATA/settings.json"
}
must_be_lan_only() { # <run>
    if started_lan_only; then
        pass "run $1: the app started LAN only, with no [UPNP] line, Internet Access off"
        return 0
    fi
    fail "run $1: the app the postinstall started is not shown LAN only: stopped, and the run ends here"
    stop_app
    launchctl unsetenv MW_LAN_ONLY
    exit 1
}

# ── 1. A hand-off file the user wrote, 0600; an old copy running ───────────
printf 'He said "yes" \\ then\ta tab\nand a second line \001 with \303\251\n' >"$WORK/consent-1.txt"
write_handoff 600 false "$WORK/consent-1.txt" binary
mkdir -p "$OLD_APP/Contents/MacOS"
python3 - "$OLD_APP/Contents/Info.plist" "$IDENT" "$NAME" <<'PY'
import plistlib, sys
plistlib.dump({"CFBundleIdentifier": sys.argv[2], "CFBundleExecutable": sys.argv[3],
               "CFBundlePackageType": "APPL"}, open(sys.argv[1], "wb"))
PY
printf '#include <unistd.h>\nint main(void) { for (;;) pause(); }\n' | xcrun clang -x c -o "$OLD_EXE" -
# Started from a subshell, so that launchd and not this script is its parent.
("$OLD_EXE" >/dev/null 2>&1 & echo $! >"$WORK/old.pid")
OLD_PID="$(cat "$WORK/old.pid")"
check "run 1: the old copy is running" kill -0 "$OLD_PID"
install_pkg 1
must_be_lan_only 1
RUN="$(this_run)"
check "run 1: $LOG_DIR is root:wheel 755, a directory" \
    [ "$(stat -f '%Su:%Sg %Lp %HT' "$LOG_DIR")" = "root:wheel 755 Directory" ]
check "run 1: $LOG is root's, 644, a regular file" \
    [ "$(stat -f '%Su %Lp %HT' "$LOG")" = "root 644 Regular File" ]
check "run 1: the log has this run" has_line "$RUN" "== postinstall done =="
check "run 1: the hand-off file is read" has_line "$RUN" "handoff: internet=false autostart=false"
check "run 1: provisioning.json written" has_line "$RUN" "wrote $APPDATA/provisioning.json (internet=false)"
check "run 1: provisioning.json is JSON, with the agreement text" provisioning_is "$WORK/consent-1.txt"
check "run 1: Start at login off, no LaunchAgent" [ ! -e "$AGENT" ]
check "run 1: the old copy is stopped" has_line "$RUN" "stopped the old copy running from $OLD_APP"
check "run 1: the old copy is removed" has_line "$RUN" "removed the old copy $OLD_APP ($IDENT)"
check "run 1: nothing left of the old copy" [ ! -e "$OLD_APP" ]
check "run 1: its process has ended" gone "$OLD_PID"
check "run 1: everything under the app data is $ME's" owned_by_me
check "run 1: the app bundle is $ME's" [ "$(stat -f %Su "$APP")" = "$ME" ]
check "run 1: the hand-off file is gone" [ ! -e "$HANDOFF" ]

# ── 2. The same hand-off file, writable by everyone ──────────────────────
write_handoff 666 false "$WORK/consent-1.txt" binary
install_pkg 2
must_be_lan_only 2
RUN="$(this_run)"
check "run 2: the hand-off file is not read" \
    has_line "$RUN" "handoff: $HANDOFF is not the console user's own file (uid=$MYUID mode=666) — not read, defaults apply"
check "run 2: the defaults apply" has_line "$RUN" "handoff: internet=false autostart=true"
check "run 2: provisioning.json has no agreement text" provisioning_is ""
check "run 2: the LaunchAgent is written" has_line "$RUN" "wrote LaunchAgent"
check "run 2: the LaunchAgent starts the app in /Applications" \
    grep -qF "<string>$EXE</string>" "$AGENT"
check "run 2: everything under the app data is $ME's" owned_by_me
check "run 2: the hand-off file is gone" [ ! -e "$HANDOFF" ]

# ── 3. A hand-off file the user wrote, 0600, Start at login on ───────────
printf '%5000s' '' | tr ' ' x >"$WORK/consent-3.txt"
write_handoff 600 true "$WORK/consent-3.txt"
rm -f "$AGENT"
install_pkg 3
must_be_lan_only 3
RUN="$(this_run)"
check "run 3: the hand-off file is read" lacks_line "$RUN" "not the console user's own file"
check "run 3: Start at login is on" has_line "$RUN" "handoff: internet=false autostart=true"
check "run 3: the LaunchAgent is written again" has_line "$RUN" "wrote LaunchAgent"
check "run 3: the LaunchAgent is there, $ME's" [ "$(stat -f %Su "$AGENT" 2>/dev/null)" = "$ME" ]
check "run 3: the agreement text is kept to 4096 characters" provisioning_is "$WORK/consent-3.txt"
echo "---- $LOG after runs 1 to 3"
cat "$LOG"
echo "----"

# ── 4. No hand-off file; the log file is a link ──────────────────────────
rm -f "$HANDOFF"
echo "kept" >"$WORK/elsewhere.log"
sudo rm -f "$LOG"
sudo ln -s "$WORK/elsewhere.log" "$LOG"
install_pkg 4
must_be_lan_only 4
check "run 4: provisioning.json is written, LAN only" provisioning_is ""
check "run 4: nothing written through the link" [ "$(cat "$WORK/elsewhere.log")" = "kept" ]
sudo rm -f "$LOG"

# ── 5. No hand-off file; the log directory is a link ─────────────────────
mkdir "$WORK/elsewhere"
sudo rmdir "$LOG_DIR"
sudo ln -s "$WORK/elsewhere" "$LOG_DIR"
install_pkg 5
must_be_lan_only 5
check "run 5: provisioning.json is written, LAN only" provisioning_is ""
check "run 5: nothing written through the link" [ -z "$(ls -A "$WORK/elsewhere")" ]
check "run 5: everything under the app data is $ME's" owned_by_me
sudo rm -f "$LOG_DIR"

stop_app
launchctl unsetenv MW_LAN_ONLY
echo "$CHECKS checks, $FAILS failed"
[ "$FAILS" -eq 0 ]
