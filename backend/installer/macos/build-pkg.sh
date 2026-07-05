#!/bin/bash
# ===========================================================================
#  MoonlightWeb — build the macOS interactive .pkg (mirrors installer/deploy.bat).
#
#  Compiles the Installer.app plugin, packs MoonlightWeb.app into a component
#  package (with the postinstall script), then wraps it in a productbuild archive
#  that shows the Introduction / License / Sunshine (plugin) / Install / Summary
#  flow.
#
#  Usage (run on macOS):
#    APP=/path/to/MoonlightWeb.app VERSION=1.2.3 OUT=moonlightweb-macos-arm64.pkg \
#      bash backend/installer/macos/build-pkg.sh
#
#  Defaults: APP=./MoonlightWeb.app, VERSION=0.1.0, OUT=moonlightweb-macos-<arch>.pkg
# ===========================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ARCH="$(uname -m)"; [ "$ARCH" = "arm64" ] || ARCH="x86_64"
APP="${APP:-./MoonlightWeb.app}"
VERSION="${VERSION:-0.1.0}"
OUT="${OUT:-moonlightweb-macos-${ARCH}.pkg}"
IDENT="com.moonlightweb.server"

[ -d "$APP" ] || { echo "error: app bundle not found: $APP" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== 1/4 build InstallerPlugin bundle =="
BUNDLE="$WORK/plugins/MoonlightWebInstaller.bundle"
mkdir -p "$BUNDLE/Contents/MacOS"
cp "$HERE/plugin/Info.plist" "$BUNDLE/Contents/Info.plist"
# InstallerPlugins.framework is a system framework; -F covers both possible homes.
clang -bundle -fobjc-arc -mmacosx-version-min=12.0 \
    -F /System/Library/Frameworks -F /System/Library/PrivateFrameworks \
    -framework InstallerPlugins -framework Cocoa -framework Foundation \
    -I "$HERE/plugin" \
    -o "$BUNDLE/Contents/MacOS/MoonlightWebInstaller" \
    "$HERE/plugin/MWSunshinePane.m"
codesign --force --timestamp=none -s - "$BUNDLE" || true
# productbuild --plugins expects the sections list beside the bundle(s).
cp "$HERE/plugins/InstallerSections.plist" "$WORK/plugins/InstallerSections.plist"

echo "== 2/4 stage app payload =="
STAGE="$WORK/stage"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/MoonlightWeb.app"

echo "== 3/4 pkgbuild component =="
COMPONENTS="$WORK/components"
mkdir -p "$COMPONENTS"
# --scripts dir must contain an executable file literally named `postinstall`.
SCRIPTS="$WORK/scripts"
mkdir -p "$SCRIPTS"
cp "$HERE/scripts/postinstall" "$SCRIPTS/postinstall"
chmod +x "$SCRIPTS/postinstall"
pkgbuild \
    --root "$STAGE" \
    --identifier "$IDENT" \
    --version "$VERSION" \
    --scripts "$SCRIPTS" \
    --install-location /Applications \
    "$COMPONENTS/MoonlightWeb-component.pkg"

echo "== 4/4 productbuild archive =="
productbuild \
    --distribution "$HERE/distribution.xml" \
    --resources "$HERE/resources" \
    --plugins "$WORK/plugins" \
    --package-path "$COMPONENTS" \
    "$OUT"

echo "built $OUT"
