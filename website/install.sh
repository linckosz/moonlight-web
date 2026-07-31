#!/bin/sh
# ===========================================================================
#  MoonlightWeb — one-line macOS installer.
#
#      curl -fsSL https://moonlightweb.top/install.sh | bash
#
#  Why this exists: the .pkg is not notarized (that needs a paid Apple
#  Developer ID — there is no free tier and no open-source programme), so a
#  browser download carries the com.apple.quarantine attribute and Gatekeeper
#  refuses to open it: "cannot be opened because it is from an unidentified
#  developer". curl never sets that attribute, and installer(8) never consults
#  Gatekeeper — so this path installs with no warning and no detour through
#  System Settings. Nothing is bypassed that would not be bypassed by the
#  Homebrew cask (backend/packaging/homebrew/moonlightweb.rb), which reaches
#  the same installer(8) the same way.
#
#  The .pkg itself is unchanged: same guided installer, same postinstall
#  (Sunshine, pairing, LaunchAgent). Only the GUI panes are skipped, so the
#  postinstall falls back to its documented CLI defaults (install Sunshine,
#  admin/admin credentials, Internet Access off) and the in-app wizard at
#  https://localhost/setup finishes the configuration.
#
#  Environment overrides:
#    MW_VERSION=0.2.4   install a specific version instead of the latest
#    MW_REPO=owner/name install from a fork
#
#  Served from website/install.sh; the canonical copy lives in the repo.
# ===========================================================================
set -eu

REPO="${MW_REPO:-linckosz/moonlight-web}"

bold=""; dim=""; red=""; reset=""
if [ -t 1 ]; then
    bold="$(printf '\033[1m')"; dim="$(printf '\033[2m')"
    red="$(printf '\033[31m')"; reset="$(printf '\033[0m')"
fi
say() { printf '%s\n' "$*"; }
die() { printf '%s\n' "${red}error:${reset} $*" >&2; exit 1; }

# ── Platform gate ──────────────────────────────────────────────────────────
# Only Apple Silicon is published; an Intel Mac has to build from source.
[ "$(uname -s)" = "Darwin" ] || die "this installer is for macOS. See https://moonlightweb.top/#download"
[ "$(uname -m)" = "arm64" ] || die "no prebuilt package for Intel Macs — build from source:
  https://github.com/$REPO#fork--build"

# ── Resolve the version ────────────────────────────────────────────────────
# The asset name embeds the version, so /releases/latest/download/<name> is not
# usable. Follow the /releases/latest redirect instead of hitting the API: no
# rate limit, no JSON parsing.
VERSION="${MW_VERSION:-}"
if [ -z "$VERSION" ]; then
    say "${dim}Looking up the latest release…${reset}"
    url="$(curl -fsSL -o /dev/null -w '%{url_effective}' "https://github.com/$REPO/releases/latest")" \
        || die "could not reach github.com"
    tag="${url##*/}"
    case "$tag" in
        v[0-9]*) VERSION="${tag#v}" ;;
        *) die "no published release found for $REPO" ;;
    esac
fi

PKG="moonlightweb-${VERSION}-macos-arm64.pkg"
URL="https://github.com/$REPO/releases/download/v${VERSION}/${PKG}"

# ── Download ───────────────────────────────────────────────────────────────
TMP="$(mktemp -d "${TMPDIR:-/tmp}/moonlightweb.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT INT TERM

say "${bold}Downloading MoonlightWeb ${VERSION}${reset}"
curl -fL --progress-bar -o "$TMP/$PKG" "$URL" \
    || die "download failed: $URL"

# A 404 page or a truncated transfer would otherwise reach installer(8) and
# fail there with a much less obvious message. Every .pkg is a xar archive.
case "$(head -c 4 "$TMP/$PKG" 2>/dev/null)" in
    "xar!") ;;
    *) die "the downloaded file is not a valid .pkg — check $URL" ;;
esac

# ── Install ────────────────────────────────────────────────────────────────
# System-wide install into /Applications (rootVolumeOnly in distribution.xml),
# so root is required. sudo reads its prompt from /dev/tty, which survives the
# `curl | bash` pipe — but a fully non-interactive shell has no tty at all.
say ""
say "${bold}Installing to /Applications${reset} ${dim}(administrator password required)${reset}"
if [ "$(id -u)" -ne 0 ]; then
    sudo -v || die "administrator rights are required to install into /Applications"
    SUDO="sudo"
else
    SUDO=""
fi

$SUDO installer -pkg "$TMP/$PKG" -target / \
    || die "installer failed. Details: /tmp/moonlightweb-postinstall.log"

say ""
say "${bold}MoonlightWeb ${VERSION} installed.${reset}"
say ""
say "  Finish the setup in your browser:  ${bold}https://localhost/setup${reset}"
say "  From another device on your LAN:   https://<this-mac-ip>"
say ""
say "  ${dim}macOS cannot grant screen capture programmatically: allow Sunshine in${reset}"
say "  ${dim}System Settings → Privacy & Security → Screen Recording when it asks.${reset}"
say ""
