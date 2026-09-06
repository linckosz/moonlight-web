#!/bin/bash
# ===========================================================================
#  MoonlightWeb — run mw-native-tests on a Mac WITH Screen Recording.
#
#  Two things stand between a freshly built test binary and a captured frame:
#
#   1. The console session. A shell arrived over SSH is another audit session;
#      ScreenCaptureKit sees no display from it. The tests are therefore run
#      through launchd, in the user's GUI session (gui/<uid>), and their output
#      collected from a file.
#
#   2. TCC. Screen Recording is granted per (bundle id, code requirement) — for
#      a bundle — or per PATH for a bare executable. A bare mw-native-tests
#      would need a grant of its own, ticked by hand in System Settings on every
#      machine and lost on every rebuild (ad hoc) or path change. Wrapped in a
#      minimal .app that carries the APP's bundle id (com.moonlightweb.server)
#      and signed with the same certificate, it satisfies the app's existing
#      grant: same identifier, same designated requirement, same row in TCC.db.
#      Measured 06/09/2026: session tests capturing at once, no new TCC row.
#
#  Usage:
#    scripts/mac-native-tests.sh <path/to/mw-native-tests> [identity] [keychain]
#      identity  the code-signing identity the APP is signed with (default:
#                "MoonlightWeb Dev", the bench identity)
#      keychain  the keychain holding it (default: ~/Library/Keychains/mw-dev.keychain-db);
#                unlocked with $MW_KEYCHAIN_PASSWORD when that is set — a locked
#                keychain makes codesign fail with errSecInternalComponent
#
#  Exit status is the test binary's (0 = every check passed).
# ===========================================================================
set -u

BIN="${1:?path to mw-native-tests}"
IDENTITY="${2:-MoonlightWeb Dev}"
KEYCHAIN="${3:-$HOME/Library/Keychains/mw-dev.keychain-db}"
if [ -f "$KEYCHAIN" ] && [ -n "${MW_KEYCHAIN_PASSWORD:-}" ]; then
    security unlock-keychain -p "$MW_KEYCHAIN_PASSWORD" "$KEYCHAIN" || exit 2
fi
WORK="${MW_TEST_WORK:-$HOME/.moonlightweb-native-tests}"
APP="$WORK/MoonlightWebTests.app"
LOG="$WORK/test.log"
LABEL=com.moonlightweb.native-test
PLIST="$HOME/Library/LaunchAgents/$LABEL.plist"
UID_=$(id -u)

[ -x "$BIN" ] || { echo "not executable: $BIN" >&2; exit 2; }
mkdir -p "$WORK"

# ── The wrapper bundle ──────────────────────────────────────────────────────
rm -rf "$APP"; mkdir -p "$APP/Contents/MacOS"
cp "$BIN" "$APP/Contents/MacOS/mw-native-tests"
cat > "$APP/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleExecutable</key><string>mw-native-tests</string>
  <key>CFBundleIdentifier</key><string>com.moonlightweb.server</string>
  <key>CFBundleName</key><string>MoonlightWebTests</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>LSUIElement</key><true/>
  <key>NSScreenCaptureUsageDescription</key><string>MoonlightWeb native tests capture the screen.</string>
</dict></plist>
PL
# (bash 3.2, the system's: an empty array under `set -u` is an error, hence
# the guarded expansion.)
KC=()
[ -f "$KEYCHAIN" ] && KC=(--keychain "$KEYCHAIN")
codesign --force ${KC[@]+"${KC[@]}"} -s "$IDENTITY" --identifier com.moonlightweb.server \
    "$APP/Contents/MacOS/mw-native-tests" || { echo "signing failed" >&2; exit 2; }
codesign --force ${KC[@]+"${KC[@]}"} -s "$IDENTITY" "$APP" || { echo "signing failed" >&2; exit 2; }
codesign -d -r- "$APP" 2>&1 | grep designated

# ── Run it in the GUI session ───────────────────────────────────────────────
launchctl bootout "gui/$UID_/$LABEL" 2>/dev/null || true
rm -f "$LOG"
cat > "$PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>Label</key><string>$LABEL</string>
  <key>ProgramArguments</key><array><string>$APP/Contents/MacOS/mw-native-tests</string></array>
  <key>RunAtLoad</key><true/>
  <key>ProcessType</key><string>Interactive</string>
  <key>StandardOutPath</key><string>$LOG</string>
  <key>StandardErrorPath</key><string>$LOG</string>
</dict></plist>
EOF
# A sleeping panel presents nothing to capture.
caffeinate -u -t 3
launchctl bootstrap "gui/$UID_" "$PLIST" || { echo "launchctl bootstrap failed" >&2; exit 2; }
for _ in $(seq 1 90); do
    sleep 1
    [ -s "$LOG" ] && grep -q 'checks passed' "$LOG" && break
done
launchctl bootout "gui/$UID_/$LABEL" 2>/dev/null || true
rm -f "$PLIST"

cat "$LOG"
grep -q 'checks passed, 0 failed' "$LOG"
