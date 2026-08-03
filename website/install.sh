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
#  off) and the in-app wizard finishes the configuration. Internet Access is
#  the one pane this script asks for itself — see ask_internet below.
#
#  Environment overrides:
#    MW_VERSION=0.2.4   pin a version (macOS and the AppImage fallback only;
#                       apt/dnf always resolve the newest in the repository)
#    MW_REPO=owner/name install from a fork
#    MW_INTERNET=1|0    answer the Internet Access question up front (default:
#                       0 — LAN only — whenever there is no terminal to ask on)
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

# How to invoke MoonlightWeb's operator commands on this install path. The
# packages put a `moonlightweb` symlink on PATH; the AppImage and the macOS
# bundle are only ever reachable by their own path. Empty until an install
# succeeds, so nothing below promises a command that is not there.
MW_CLI=""

# The operator commands worth knowing before closing the terminal. The web UI
# covers all of them, but it is not always the shortest route — and on a box
# reached over SSH it is not a route at all.
cli_hint() {
    [ -n "$MW_CLI" ] || return 0
    say "  ${bold}MoonlightWeb is a command-line tool too:${reset}"
    say ""
    printf '    %-34s %s\n' "$MW_CLI --status" \
        "${dim}URLs, access PIN, internet state${reset}"
    printf '    %-34s %s\n' "$MW_CLI --new-pin" \
        "${dim}let one more device in${reset}"
    printf '    %-34s %s\n' "$MW_CLI --enable-internet" \
        "${dim}publish it on a public sub-domain${reset}"
    printf '    %-34s %s\n' "$MW_CLI --help" \
        "${dim}every command${reset}"
    say ""
}

done_banner() {
    say ""
    say "${bold}MoonlightWeb installed.${reset}"
    say ""
    say "  Finish the setup in your browser:  ${bold}https://localhost/setup${reset}"
    say "  From another device on your LAN:   https://<this-machine-ip>"
    say ""
    cli_hint
}

# ── Internet Access consent ────────────────────────────────────────────────
# Asked here rather than left to the app, because this is the only moment a
# human is certainly watching: on a headless box nobody ever opens the setup
# wizard, and the answer decides whether the machine is reachable from outside
# the LAN at all.
#
# Drawn on /dev/tty, not stdin: under `curl | bash` stdin is the script itself.
# With no terminal to draw on (CI, image builds, `sh install.sh < /dev/null`)
# the answer is No — the private default — and MW_INTERNET answers it up front.
WANT_INTERNET=no

ask_internet() {
    case "${MW_INTERNET:-}" in
        1|yes|true) WANT_INTERNET=yes; return 0 ;;
        0|no|false) WANT_INTERNET=no; return 0 ;;
    esac
    { [ -t 1 ] && [ -r /dev/tty ] && have stty && have od && have dd; } || return 0

    say ""
    say "${bold}Expose this machine on the internet?${reset}"
    say ""
    say "  MoonlightWeb can claim a sub-domain of moonlightweb.top for this"
    say "  machine, point it at your public IP address and obtain a real TLS"
    say "  certificate for it — so you can stream from anywhere, not only from"
    say "  this network. The sub-domain and that IP address are stored on the"
    say "  MoonlightWeb DNS server for as long as you keep the link on."
    say ""
    say "  ${dim}No decides nothing for good: Internet Access can be turned on —"
    say "  and off again — at any time from the admin page.${reset}"
    say ""
    say "  ${dim}up/down arrows to choose, Enter to confirm${reset}"
    say ""

    # Cursor hidden and the terminal put in raw mode for the duration, so a
    # keypress arrives immediately and is not echoed into the drawing. Both are
    # undone on the way out, including on Ctrl-C — leaving a terminal with no
    # echo is a far worse outcome than an unanswered question.
    _sel=1
    _stty="$(stty -g < /dev/tty 2>/dev/null)" || _stty=""
    # Ctrl-C has to leave, not merely tidy up: the handler returns into the read
    # loop otherwise, and the loop would carry on with a cooked terminal.
    trap '[ -n "$_stty" ] && stty "$_stty" < /dev/tty 2>/dev/null
          printf "\033[?25h\n"; exit 130' INT TERM
    stty -echo -icanon min 1 time 0 < /dev/tty 2>/dev/null || true
    printf '\033[?25l'

    _drawn=""
    while :; do
        # Redraw in place: back up over the two option lines printed last pass.
        # Keep them short enough never to wrap, or the count would be wrong.
        [ -z "$_drawn" ] || printf '\033[2A'
        _drawn=1
        if [ "$_sel" = 0 ]; then
            printf '    %s> Yes%s  register the sub-domain now\033[K\n' "$bold" "$reset"
            printf '      No   %sstay on this network only%s\033[K\n' "$dim" "$reset"
        else
            printf '      Yes  %sregister the sub-domain now%s\033[K\n' "$dim" "$reset"
            printf '    %s> No%s   stay on this network only\033[K\n' "$bold" "$reset"
        fi

        # One byte at a time, as a decimal value: `read` cannot see an escape
        # sequence arrive and od keeps this to tools every POSIX system has.
        _key="$(dd bs=1 count=1 2>/dev/null < /dev/tty | od -An -tu1 | tr -d ' \n')"
        case "$_key" in
            27) # CSI: swallow the '[', then act on the final byte
                dd bs=1 count=1 < /dev/tty > /dev/null 2>&1
                case "$(dd bs=1 count=1 2>/dev/null < /dev/tty | od -An -tu1 | tr -d ' \n')" in
                    65|68) _sel=0 ;; # up, left
                    66|67) _sel=1 ;; # down, right
                esac ;;
            121|89) _sel=0 ;;                            # y, Y
            110|78) _sel=1 ;;                            # n, N
            32|9) [ "$_sel" = 0 ] && _sel=1 || _sel=0 ;; # space, tab
            10|13|"") break ;;                           # Enter — or EOF
        esac
    done

    printf '\033[?25h'
    [ -z "$_stty" ] || stty "$_stty" < /dev/tty 2>/dev/null || true
    trap - INT TERM
    [ "$_sel" = 0 ] && WANT_INTERNET=yes || WANT_INTERNET=no
    say ""
}

# Act on a Yes, once the app is installed. The registration is driven by the
# *running* instance (it holds the DNS credentials and the ACME account), which
# the package's postinstall has only just been told to start — so wait for it to
# answer on loopback first. `--yes` prints the full agreement and proceeds; the
# audit entry records those exact words, not the summary shown above.
enable_internet() {
    [ "$WANT_INTERNET" = "yes" ] || return 0
    if [ -z "$MW_CLI" ]; then
        say "  ${dim}Turn on Internet Access from the admin page once it is running.${reset}"
        return 0
    fi

    _try=0
    while [ "$_try" -lt 30 ]; do
        if "$MW_CLI" --status > /dev/null 2>&1; then break; fi
        _try=$((_try + 1))
        sleep 1
    done

    if ! "$MW_CLI" --enable-internet --yes; then
        say ""
        say "  ${red}Internet Access could not be enabled.${reset} Nothing else is affected —"
        say "  ${dim}retry from the admin page, or with: $MW_CLI --enable-internet${reset}"
    fi
}

# No desktop on this machine: no browser to open the setup wizard in, and no
# display to capture — so no Sunshine either. The package's postinstall reaches
# the same conclusion on its own (it installs and starts the systemd service
# instead of launching into a graphical session); this only decides how to
# report it, and hands over to the app's own operator command for the details.
#
# `systemctl get-default` is the discriminator rather than $DISPLAY alone: this
# script is very often run over SSH on a machine that does have a desktop.
# MW_HEADLESS=1 forces it either way.
is_headless() {
    [ "${MW_HEADLESS:-0}" = "1" ] && return 0
    [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && return 1
    have systemctl || return 1
    [ "$(systemctl get-default 2>/dev/null)" = "graphical.target" ] && return 1
    return 0
}

done_banner_headless() {
    say ""
    say "${bold}MoonlightWeb installed and running as a system service.${reset}"
    say ""
    # Real URLs, real PIN, real router verdict — straight from the running
    # server, so nothing here can drift from what it actually did. systemd has
    # only just been told to start it, and --status talks to a listening socket:
    # give the TLS listener a few seconds to come up before giving up on it.
    if have moonlightweb; then
        _try=0
        while [ "$_try" -lt 15 ]; do
            if moonlightweb --status 2>/dev/null; then break; fi
            _try=$((_try + 1))
            sleep 1
        done
        [ "$_try" -lt 15 ] || say "  ${dim}(server still starting — run 'moonlightweb --status')${reset}"
    fi
    say "  ${dim}Sunshine was not installed: this host has no display to capture and no${reset}"
    say "  ${dim}GPU to encode with. Add your streaming hosts by IP from the web UI.${reset}"
    say ""
    cli_hint
    say "  ${dim}Service:  sudo systemctl {status,restart,stop} moonlightweb${reset}"
    say ""
}

# ── macOS ──────────────────────────────────────────────────────────────────
install_macos() {
    # Only Apple Silicon is published; an Intel Mac has to build from source.
    [ "$(uname -m)" = "arm64" ] || die "no prebuilt package for Intel Macs — build from source:
  https://github.com/$REPO#fork--build"

    ask_internet

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

    # No symlink on PATH here: the .pkg installs an app bundle and nothing else.
    MW_CLI="/Applications/MoonlightWeb.app/Contents/MacOS/MoonlightWeb"
    enable_internet

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
    MW_CLI=moonlightweb
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
    MW_CLI=moonlightweb
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
    MW_CLI="$DEST/MoonlightWeb.AppImage"

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
    cli_hint
    # Nothing started the app here — unlike the packages, an AppImage has no
    # postinstall — so the sub-domain cannot be registered from this script.
    if [ "$WANT_INTERNET" = "yes" ]; then
        say "  ${dim}Internet Access: start it first, then run${reset}"
        say "  ${dim}  $MW_CLI --enable-internet${reset}"
        say ""
    fi
    say "  ${dim}No repository on this distro, so no automatic updates:${reset}"
    say "  ${dim}re-run this script to upgrade.${reset}"
    if [ "${MW_HEADLESS:-0}" = "1" ]; then
        say ""
        say "  ${dim}Headless host: only the .deb and .rpm register the systemd service for${reset}"
        say "  ${dim}you. To run this AppImage as one, adapt the unit shipped in the repo:${reset}"
        say "  ${dim}backend/packaging/systemd/moonlightweb.service (point ExecStart here).${reset}"
    fi
    say ""
    exit 0
}

# ── Linux ──────────────────────────────────────────────────────────────────
install_linux() {
    [ "$(uname -m)" = "x86_64" ] || die "only x86_64 Linux is published — build from source:
  https://github.com/$REPO#fork--build"

    ask_internet

    # Propagated into the package's postinstall, which uses it for the same
    # decision (dpkg/rpm keep the environment they were invoked with).
    if is_headless; then
        MW_HEADLESS=1
        export MW_HEADLESS
        say "${dim}No desktop detected — installing in headless (server) mode.${reset}"
    fi

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
                # Repackaged .deb: same /usr/bin/moonlightweb symlink.
                MW_CLI=moonlightweb
                enable_internet
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

    enable_internet

    if [ "${MW_HEADLESS:-0}" = "1" ]; then
        done_banner_headless
    else
        done_banner
    fi
    say "  ${dim}Ports 443/tcp, 80/tcp and 47999/udp are opened in firewalld/ufw${reset}"
    say "  ${dim}when one of them is active.${reset}"
    say ""
}

case "$(uname -s)" in
    Darwin) install_macos ;;
    Linux)  install_linux ;;
    *) die "unsupported system: $(uname -s). See https://moonlightweb.top/#download" ;;
esac
