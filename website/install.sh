#!/bin/sh
# ===========================================================================
#  MoonlightWeb — one-line installer for macOS and Linux.
#
#      curl -fsSL https://moonlightweb.top/install.sh | bash
#
#  macOS: the .pkg is not notarized (that needs a paid Apple Developer ID —
#  there is no free tier and no open-source programme), so a browser download
#  carries the com.apple.quarantine attribute and Gatekeeper refuses to open
#  it: "cannot be opened because it is from an unidentified developer". curl
#  never sets that attribute, and installer(8) never consults Gatekeeper — so
#  this path installs with no warning and no detour through System Settings.
#  Nothing is bypassed that would not be bypassed by the Homebrew cask
#  (backend/packaging/homebrew/moonlightweb.rb), which reaches the same
#  installer(8) the same way.
#
#  Linux: registers the signed APT or DNF repository and installs from it, so
#  the system updater keeps MoonlightWeb up to date afterwards and the app
#  appears in GNOME Software / KDE Discover / App Center. Distros without one
#  of those package managers fall back to the AppImage.
#
#  The packages themselves are unchanged — same postinstall (Sunshine,
#  pairing, service registration), only the clicking is skipped. On macOS the
#  installer's GUI panes are bypassed, so the postinstall uses its documented
#  CLI defaults (install Sunshine, admin/admin credentials, Internet Access
#  off) and the in-app wizard finishes the configuration.
#
#  Environment overrides:
#    MW_VERSION=0.2.4   pin a version (macOS and the AppImage fallback only;
#                       apt/dnf always resolve the newest in the repository)
#    MW_REPO=owner/name install from a fork
#
#  Served from website/install.sh; the canonical copy lives in the repo.
# ===========================================================================
set -eu

REPO="${MW_REPO:-linckosz/moonlight-web}"
# Where the `linux-repo` job publishes the signed apt/dnf repositories.
PAGES="https://linckosz.github.io/moonlight-web"

bold=""; dim=""; red=""; reset=""
if [ -t 1 ]; then
    bold="$(printf '\033[1m')"; dim="$(printf '\033[2m')"
    red="$(printf '\033[31m')"; reset="$(printf '\033[0m')"
fi
say() { printf '%s\n' "$*"; }
die() { printf '%s\n' "${red}error:${reset} $*" >&2; exit 1; }
have() { command -v "$1" >/dev/null 2>&1; }

# Root for the install step only. sudo reads its prompt from /dev/tty, which
# survives the `curl | bash` pipe — but a fully non-interactive shell has none.
SUDO=""
need_root() {
    [ "$(id -u)" -ne 0 ] || return 0
    have sudo || die "this needs administrator rights, and sudo is not installed"
    sudo -v || die "administrator rights are required to install system-wide"
    SUDO="sudo"
}

# The asset name embeds the version, so /releases/latest/download/<name> is not
# usable. Follow the /releases/latest redirect instead of hitting the API: no
# rate limit, no JSON parsing.
latest_version() {
    _url="$(curl -fsSL -o /dev/null -w '%{url_effective}' \
            "https://github.com/$REPO/releases/latest")" \
        || die "could not reach github.com"
    _tag="${_url##*/}"
    case "$_tag" in
        v[0-9]*) printf '%s' "${_tag#v}" ;;
        *) die "no published release found for $REPO" ;;
    esac
}

done_banner() {
    say ""
    say "${bold}MoonlightWeb installed.${reset}"
    say ""
    say "  Finish the setup in your browser:  ${bold}https://localhost/setup${reset}"
    say "  From another device on your LAN:   https://<this-machine-ip>"
    say ""
}

# ── macOS ──────────────────────────────────────────────────────────────────
install_macos() {
    # Only Apple Silicon is published; an Intel Mac has to build from source.
    [ "$(uname -m)" = "arm64" ] || die "no prebuilt package for Intel Macs — build from source:
  https://github.com/$REPO#fork--build"

    VERSION="${MW_VERSION:-}"
    if [ -z "$VERSION" ]; then
        say "${dim}Looking up the latest release…${reset}"
        VERSION="$(latest_version)"
    fi

    PKG="moonlightweb-${VERSION}-macos-arm64.pkg"
    URL="https://github.com/$REPO/releases/download/v${VERSION}/${PKG}"

    TMP="$(mktemp -d "${TMPDIR:-/tmp}/moonlightweb.XXXXXX")"
    trap 'rm -rf "$TMP"' EXIT INT TERM

    say "${bold}Downloading MoonlightWeb ${VERSION}${reset}"
    curl -fL --progress-bar -o "$TMP/$PKG" "$URL" || die "download failed: $URL"

    # A 404 page or a truncated transfer would otherwise reach installer(8) and
    # fail there with a much less obvious message. Every .pkg is a xar archive.
    case "$(head -c 4 "$TMP/$PKG" 2>/dev/null)" in
        "xar!") ;;
        *) die "the downloaded file is not a valid .pkg — check $URL" ;;
    esac

    # System-wide install into /Applications (rootVolumeOnly in distribution.xml).
    say ""
    say "${bold}Installing to /Applications${reset} ${dim}(administrator password required)${reset}"
    need_root
    $SUDO installer -pkg "$TMP/$PKG" -target / \
        || die "installer failed. Details: /tmp/moonlightweb-postinstall.log"

    done_banner
    say "  ${dim}macOS cannot grant screen capture programmatically: allow Sunshine in${reset}"
    say "  ${dim}System Settings → Privacy & Security → Screen Recording when it asks.${reset}"
    say ""
}

# ── Linux: APT (Debian, Ubuntu, Mint, Pop!_OS) ─────────────────────────────
install_apt() {
    say "${bold}Adding the MoonlightWeb APT repository${reset}"
    need_root
    # A dearmored keyring, so gnupg does not have to be installed. Scoped to
    # this one source via Signed-By — never added to the global trusted set.
    $SUDO install -d -m 0755 /etc/apt/keyrings
    curl -fsSL "$PAGES/moonlightweb.gpg" \
        | $SUDO tee /etc/apt/keyrings/moonlightweb.gpg > /dev/null \
        || die "could not fetch the repository signing key"
    $SUDO chmod 0644 /etc/apt/keyrings/moonlightweb.gpg
    curl -fsSL "$PAGES/moonlightweb.sources" \
        | $SUDO tee /etc/apt/sources.list.d/moonlightweb.sources > /dev/null \
        || die "could not fetch the repository definition"

    say "${bold}Installing${reset}"
    $SUDO apt-get update
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y moonlightweb
}

# ── Linux: DNF / YUM (Fedora, RHEL, Nobara) and zypper (openSUSE) ──────────
install_rpm() {
    _mgr=$1 _dir=$2
    say "${bold}Adding the MoonlightWeb repository${reset}"
    need_root
    curl -fsSL "$PAGES/moonlightweb.repo" | $SUDO tee "$_dir/moonlightweb.repo" > /dev/null \
        || die "could not fetch the repository definition"

    say "${bold}Installing${reset}"
    case "$_mgr" in
        # -y also accepts the one-time import of the repository signing key.
        dnf|yum) $SUDO "$_mgr" install -y moonlightweb ;;
        zypper)  $SUDO zypper --non-interactive --gpg-auto-import-keys refresh
                 $SUDO zypper --non-interactive install moonlightweb ;;
    esac
}

# ── Linux: AppImage fallback ───────────────────────────────────────────────
# For distros with none of the above (and for Arch when no AUR helper is
# present). No repository, so no automatic updates — re-run this script.
install_appimage() {
    VERSION="${MW_VERSION:-}"
    if [ -z "$VERSION" ]; then
        say "${dim}Looking up the latest release…${reset}"
        VERSION="$(latest_version)"
    fi
    APP="moonlightweb-${VERSION}-linux-x64.AppImage"
    URL="https://github.com/$REPO/releases/download/v${VERSION}/${APP}"
    DEST="$HOME/.local/bin"

    say "${bold}Downloading MoonlightWeb ${VERSION}${reset} ${dim}(AppImage)${reset}"
    mkdir -p "$DEST"
    curl -fL --progress-bar -o "$DEST/MoonlightWeb.AppImage" "$URL" \
        || die "download failed: $URL"
    # Every AppImage is an ELF; a 404 page is not.
    case "$(head -c 4 "$DEST/MoonlightWeb.AppImage" | od -An -c | tr -d ' \n')" in
        *177ELF*) ;;
        *) rm -f "$DEST/MoonlightWeb.AppImage"
           die "the downloaded file is not a valid AppImage — check $URL" ;;
    esac
    chmod +x "$DEST/MoonlightWeb.AppImage"

    # Menu entry, so it is not only reachable from a terminal.
    mkdir -p "$HOME/.local/share/applications"
    cat > "$HOME/.local/share/applications/top.moonlightweb.MoonlightWeb.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=MoonlightWeb
Comment=Sunshine streaming client for the browser
Exec=$DEST/MoonlightWeb.AppImage
Icon=moonlightweb
Categories=Network;Game;
Terminal=false
EOF
    update-desktop-database "$HOME/.local/share/applications" >/dev/null 2>&1 || true

    say ""
    say "${bold}MoonlightWeb ${VERSION} installed to $DEST.${reset}"
    say ""
    say "  Start it:  ${bold}$DEST/MoonlightWeb.AppImage${reset}"
    say "  Then open: ${bold}https://localhost/setup${reset}"
    say ""
    say "  ${dim}No repository on this distro, so no automatic updates:${reset}"
    say "  ${dim}re-run this script to upgrade.${reset}"
    say ""
    exit 0
}

# ── Linux ──────────────────────────────────────────────────────────────────
install_linux() {
    [ "$(uname -m)" = "x86_64" ] || die "only x86_64 Linux is published — build from source:
  https://github.com/$REPO#fork--build"

    if have apt-get; then
        install_apt
    elif have dnf; then
        install_rpm dnf /etc/yum.repos.d
    elif have zypper; then
        install_rpm zypper /etc/zypp/repos.d
    elif have yum; then
        install_rpm yum /etc/yum.repos.d
    elif have pacman; then
        # No repository for Arch: the AUR is the native path, but building from
        # it needs a helper and must not run as root.
        for helper in paru yay; do
            if have "$helper"; then
                say "${bold}Installing moonlightweb-bin from the AUR${reset} ${dim}($helper)${reset}"
                "$helper" -S --needed moonlightweb-bin
                done_banner
                exit 0
            fi
        done
        say "${dim}No AUR helper found (paru/yay). Either install one and run${reset}"
        say "${dim}  paru -S moonlightweb-bin${reset}"
        say "${dim}or continue with the AppImage.${reset}"
        say ""
        install_appimage
    else
        install_appimage
    fi

    done_banner
    say "  ${dim}Ports 443/tcp, 80/tcp and 47999/udp are opened in firewalld/ufw${reset}"
    say "  ${dim}when one of them is active.${reset}"
    say ""
}

case "$(uname -s)" in
    Darwin) install_macos ;;
    Linux)  install_linux ;;
    *) die "unsupported system: $(uname -s). See https://moonlightweb.top/#download" ;;
esac
