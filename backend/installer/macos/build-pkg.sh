#!/bin/bash
# ===========================================================================
#  MoonlightWeb — build the macOS interactive .pkg (mirrors installer/deploy.bat).
#
#  Compiles the Installer.app plugin, packs MoonlightWeb.app into a component
#  package (with the postinstall script), then wraps it in a productbuild archive
#  that shows the Introduction / License / Internet (plugin) / Install / Summary
#  flow.
#
#  Usage (run on macOS):
#    APP=/path/to/MoonlightWeb.app VERSION=1.2.3 OUT=moonlightweb-macos-arm64.pkg \
#      bash backend/installer/macos/build-pkg.sh
#
#  Defaults: APP=./MoonlightWeb.app, VERSION=0.1.2, OUT=moonlightweb-macos-<arch>.pkg
#
#  EDITION=dev packages the DEV edition instead: MoonlightWebDev.app, bundle and
#  package id com.moonlightweb.server.dev, LaunchAgent com.moonlightweb.agent.dev
#  — everything a production install is known by, renamed, so the two coexist.
# ===========================================================================
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ARCH="$(uname -m)"; [ "$ARCH" = "arm64" ] || ARCH="x86_64"
EDITION="${EDITION:-prod}"
case "$EDITION" in
    prod) APP_NAME=MoonlightWeb;    IDENT=com.moonlightweb.server;     AGENT_LABEL=com.moonlightweb.agent ;;
    dev)  APP_NAME=MoonlightWebDev; IDENT=com.moonlightweb.server.dev; AGENT_LABEL=com.moonlightweb.agent.dev ;;
    *) echo "error: EDITION must be prod or dev (got '$EDITION')" >&2; exit 1 ;;
esac
APP="${APP:-./$APP_NAME.app}"
VERSION="${VERSION:-0.1.2}"
OUT="${OUT:-moonlightweb-macos-${ARCH}.pkg}"

[ -d "$APP" ] || { echo "error: app bundle not found: $APP" >&2; exit 1; }

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== 1/4 build InstallerPlugin bundle =="
BUNDLE="$WORK/plugins/MoonlightWebInstaller.bundle"
mkdir -p "$BUNDLE/Contents/MacOS" "$BUNDLE/Contents/Resources"
cp "$HERE/plugin/Info.plist" "$BUNDLE/Contents/Info.plist"
# Installer.app instantiates the framework's InstallerSection (NSPrincipalClass),
# which loads NSMainNibFile; the nib wires section.firstPane -> MWInternetPane.
xcrun ibtool --compile "$BUNDLE/Contents/Resources/MWInternetPane.nib" \
    "$HERE/plugin/MWInternetPane.xib"
# InstallerPlugins.framework is deprecated and header-stripped in the SDK, so we
# self-declare its InstallerPane API (see plugin/MWInstallerPane.h) and only LINK
# the framework binary. Do NOT add -F /System/Library/Frameworks: that points at
# the headerless runtime copy and shadows the SDK. Build via xcrun so the SDK
# sysroot (and its framework binaries) are found.
xcrun clang -bundle -fobjc-arc -mmacosx-version-min=12.0 \
    -isysroot "$(xcrun --show-sdk-path)" \
    -framework InstallerPlugins -framework Cocoa -framework Foundation \
    -I "$HERE/plugin" \
    -o "$BUNDLE/Contents/MacOS/MoonlightWebInstaller" \
    "$HERE/plugin/MWInternetPane.m"
codesign --force --timestamp=none -s - "$BUNDLE" || true
# productbuild --plugins expects the sections list beside the bundle(s).
cp "$HERE/plugins/InstallerSections.plist" "$WORK/plugins/InstallerSections.plist"

echo "== 2/4 stage app payload =="
STAGE="$WORK/stage"
mkdir -p "$STAGE"
cp -R "$APP" "$STAGE/$APP_NAME.app"

echo "== 3/4 pkgbuild component =="
COMPONENTS="$WORK/components"
mkdir -p "$COMPONENTS"
# --scripts dir must contain an executable file literally named `postinstall`.
# It is written for either edition; the names it acts on are filled in here.
SCRIPTS="$WORK/scripts"
mkdir -p "$SCRIPTS"
sed -e "s/@MW_APP_NAME@/$APP_NAME/g" -e "s/@MW_AGENT_LABEL@/$AGENT_LABEL/g" \
    "$HERE/scripts/postinstall" > "$SCRIPTS/postinstall"
chmod +x "$SCRIPTS/postinstall"
pkgbuild \
    --root "$STAGE" \
    --identifier "$IDENT" \
    --version "$VERSION" \
    --scripts "$SCRIPTS" \
    --install-location /Applications \
    "$COMPONENTS/$APP_NAME-component.pkg"

echo "== 4/4 productbuild archive =="
sed -e "s/@MW_VERSION@/$VERSION/g" -e "s/@MW_APP_NAME@/$APP_NAME/g" -e "s/@MW_IDENT@/$IDENT/g" \
    "$HERE/distribution.xml" > "$WORK/distribution.xml"
productbuild \
    --distribution "$WORK/distribution.xml" \
    --resources "$HERE/resources" \
    --plugins "$WORK/plugins" \
    --package-path "$COMPONENTS" \
    "$OUT"

echo "built $OUT"
