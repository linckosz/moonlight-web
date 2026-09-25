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
#  The packages themselves are unchanged — same postinstall (service
#  registration, LaunchAgent, provisioning.json), only the clicking is skipped.
#  No streaming server is installed on any platform: the app captures and
#  encodes this machine itself. On macOS the installer's GUI pane is bypassed,
#  so the postinstall uses its documented CLI default (Internet Access off) and
#  the in-app wizard finishes the configuration. Internet Access is the one
#  question this script asks for itself — see ask_internet below.
#
#  Environment overrides:
#    MW_VERSION=0.2.4   pin a version (macOS and the AppImage fallback only;
#                       apt/dnf always resolve the newest in the repository)
#    MW_REPO=owner/name install from a fork
#    MW_PACKAGES=<url>  take the apt/dnf repository from somewhere other than
#                       packages.moonlightweb.top — how a repository built from
#                       an unpublished CI run is tried out, see
#                       backend/packaging/linux/try-install.sh. MW_PAGES is
#                       still read, under its old name.
#    MW_INTERNET=1|0    answer the Internet Access question up front (default:
#                       0 — LAN only — whenever there is no terminal to ask on)
#
#  Served from website/install.sh; the canonical copy lives in the repo.
# ===========================================================================
set -eu

REPO="${MW_REPO:-linckosz/moonlight-web}"
# Where the signed apt/dnf repositories built by the `linux-repo` job are
# served from. Not a Pages site any more: Pages is one site per repository and
# the bootstrap already owns it — the package tree raced it after every tag
# and won (2026-09-14) — so release.yml only attaches the tree to the release
# and deploy/powerdns/deploy-packages.sh ships it to this host by hand.
#
# A fork has no such host, so MW_REPO keeps the old Pages convention unless it
# says otherwise: Pages host names are lower-case, the path keeps the
# repository's own spelling.
_owner="$(printf '%s' "${REPO%%/*}" | tr '[:upper:]' '[:lower:]')"
PACKAGES="${MW_PACKAGES:-${MW_PAGES:-}}"
if [ -z "$PACKAGES" ]; then
    if [ "$REPO" = "linckosz/moonlight-web" ]; then
        PACKAGES="https://packages.moonlightweb.top"
    else
        PACKAGES="https://$_owner.github.io/${REPO#*/}"
    fi
fi

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
    printf '    %-34s %s\n' "$MW_CLI --set-admin-password" \
        "${dim}open the admin page to your LAN${reset}"
    printf '    %-34s %s\n' "$MW_CLI --enable-internet" \
        "${dim}allow the internet link${reset}"
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
    say "  MoonlightWeb can allow streaming from outside this network, in a"
    say "  highly secure way."
    say ""
    say "  Streaming is direct, from this machine to the browser you invited."
    say "  While a session runs, your router is asked (UPnP) to open a single"
    say "  port for it: UDP, with TCP on the same number for networks that"
    say "  block UDP. It carries nothing but the encrypted stream, every"
    say "  connection on it has to authenticate first, and it closes again"
    say "  when the session ends."
    say ""
    say "  A rendezvous server introduces the two sides, so nothing about this"
    say "  machine is published. No public DNS record is created, no certificate"
    say "  is issued, and ports 80/443 stay closed. Your public IP address is"
    say "  listed nowhere: only that server and whoever holds the link you sent"
    say "  ever see it. To learn its own public address, this machine asks a"
    say "  MoonlightWeb STUN server, or a public one (Google, Cloudflare) if"
    say "  that one cannot be reached."
    say ""
    say "  ${dim}Skip decides nothing for good: Internet Access can be turned on,"
    say "  and off again, at any time from the admin page.${reset}"
    say ""
    say "  ${dim}up/down arrows to choose, Enter to confirm${reset}"
    say ""

    # Cursor hidden and the terminal put in raw mode for the duration, so a
    # keypress arrives immediately and is not echoed into the drawing. Both are
    # undone on the way out, including on Ctrl-C — leaving a terminal with no
    # echo is a far worse outcome than an unanswered question.
    _sel=0
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
            printf '    %s> Skip%s   stay on this network only\033[K\n' "$bold" "$reset"
            printf '      Accept %sallow the internet link%s\033[K\n' "$dim" "$reset"
        else
            printf '      Skip   %sstay on this network only%s\033[K\n' "$dim" "$reset"
            printf '    %s> Accept%s allow the internet link\033[K\n' "$bold" "$reset"
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
            121|89|97|65) _sel=1 ;;                      # y, Y, a, A
            110|78|115|83) _sel=0 ;;                     # n, N, s, S
            32|9) [ "$_sel" = 0 ] && _sel=1 || _sel=0 ;; # space, tab
            10|13|"") break ;;                           # Enter — or EOF
        esac
    done

    printf '\033[?25h'
    [ -z "$_stty" ] || stty "$_stty" < /dev/tty 2>/dev/null || true
    trap - INT TERM
    [ "$_sel" = 1 ] && WANT_INTERNET=yes || WANT_INTERNET=no
    say ""
}

# Act on a Yes, once the app is installed. The enable is driven by the
# *running* instance (it records the consent and owns the router mapping),
# which the package's postinstall has only just been told to start — so wait
# for it to answer on loopback first. `--yes` prints the full agreement and
# proceeds; the consent record keeps those exact words, not the summary shown
# above.
# The operator commands all talk to the running instance over loopback, and the
# package's postinstall has only just been told to start it. Give it a moment to
# answer before asking it anything.
wait_for_server() {
    _try=0
    while [ "$_try" -lt 30 ]; do
        if "$MW_CLI" --status > /dev/null 2>&1; then return 0; fi
        _try=$((_try + 1))
        sleep 1
    done
    return 1
}

# The admin page is restricted to the machine itself, and a headless box has no
# browser to open it with — so this password is the only way its owner ever
# reaches it, from another computer on the same network. There is no built-in
# default, which means skipping this leaves that door shut rather than leaving it
# open with a password everyone knows. Ask now, while someone is watching.
#
# Read from /dev/tty, not stdin: under `curl | bash` stdin is the script itself.
set_admin_password() {
    [ -n "$MW_CLI" ] || return 0
    { [ -t 1 ] && [ -r /dev/tty ]; } || return 0

    say ""
    say "${bold}Set an admin password for this server${reset}"
    say ""
    say "  You type it to open the admin page from another computer on your"
    say "  network — the PIN above only grants streaming. Without a password the"
    say "  admin page cannot be opened at all on a machine with no desktop."
    say ""
    say "  ${dim}At least 8 characters. Leave it empty to skip; you can set one"
    say "  later with: $MW_CLI --set-admin-password${reset}"
    say ""

    _stty="$(stty -g < /dev/tty 2>/dev/null)" || _stty=""
    # Leaving a terminal with echo off is a far worse outcome than an unanswered
    # question, so Ctrl-C restores it and leaves.
    trap '[ -n "$_stty" ] && stty "$_stty" < /dev/tty 2>/dev/null
          printf "\n"; exit 130' INT TERM
    stty -echo < /dev/tty 2>/dev/null || true
    printf '  Password: '
    IFS= read -r _pw < /dev/tty || _pw=""
    [ -n "$_stty" ] && stty "$_stty" < /dev/tty 2>/dev/null
    trap - INT TERM
    printf '\n'

    if [ -z "$_pw" ]; then
        say "  ${dim}Skipped — the admin page stays reachable from this machine only.${reset}"
        return 0
    fi

    if ! wait_for_server; then
        say "  ${red}The server is not answering yet.${reset} ${dim}Set it with:"
        say "  $MW_CLI --set-admin-password${reset}"
        _pw=""
        return 0
    fi

    if printf '%s\n' "$_pw" | "$MW_CLI" --set-admin-password > /dev/null 2>&1; then
        say "  ${dim}Admin password set.${reset}"
    else
        say "  ${red}Could not set it.${reset} ${dim}It may be too short — retry with:"
        say "  $MW_CLI --set-admin-password${reset}"
    fi
    _pw=""
}

enable_internet() {
    [ "$WANT_INTERNET" = "yes" ] || return 0
    if [ -z "$MW_CLI" ]; then
        say "  ${dim}Turn on Internet Access from the admin page once it is running.${reset}"
        return 0
    fi

    wait_for_server || true

    if ! "$MW_CLI" --enable-internet --yes; then
        say ""
        say "  ${red}Internet Access could not be enabled.${reset} Nothing else is affected —"
        say "  ${dim}retry from the admin page, or with: $MW_CLI --enable-internet${reset}"
    fi
}

# No desktop on this machine: no browser to open the setup wizard in, and no
# display to capture — so this box relays other hosts rather than streaming
# itself. The package's postinstall reaches the same conclusion on its own (it
# installs and starts the systemd service instead of launching into a graphical
# session); this only decides how to report it, and hands over to the app's own
# operator command for the details.
#
# `systemctl get-default` is the discriminator rather than $DISPLAY alone: this
# script is very often run over SSH on a machine that does have a desktop.
# MW_HEADLESS=1 forces it either way.
# Docker, Podman, LXC. Used for two different verdicts below — whether this is
# a headless install, and whether the LAN can reach it at all.
in_container() {
    [ -f /.dockerenv ] || [ -f /run/.containerenv ] ||
        grep -qE '(docker|lxc|containerd)' /proc/1/cgroup 2>/dev/null
}

is_headless() {
    [ "${MW_HEADLESS:-0}" = "1" ] && return 0
    [ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ] && return 1
    # A container has no desktop and nobody logged into one, whether or not it
    # runs systemd. Without this the `have systemctl` line below calls a plain
    # `debian:12` image a desktop machine — it has no systemctl, so the test
    # gives up and returns "not headless" — and the install then promises a
    # setup wizard in a browser that nothing ever opened.
    in_container && return 0
    have systemctl || return 1
    [ "$(systemctl get-default 2>/dev/null)" = "graphical.target" ] && return 1
    return 0
}

done_banner_headless() {
    # Real URLs, real PIN, real router verdict — straight from the running
    # server, so nothing here can drift from what it actually did. systemd has
    # only just been told to start it, and --status talks to a listening socket:
    # give the TLS listener a few seconds to come up before giving up on it.
    _status=""
    if have moonlightweb; then
        _try=0
        while [ "$_try" -lt 15 ]; do
            if _status="$(moonlightweb --status 2>/dev/null)"; then break; fi
            _try=$((_try + 1))
            sleep 1
        done
        [ "$_try" -lt 15 ] || _status=""
    fi

    say ""
    if [ -n "$_status" ]; then
        say "${bold}MoonlightWeb installed and running as a system service.${reset}"
        say ""
        say "$_status"
    elif [ ! -d /run/systemd/system ]; then
        # No systemd, so the postinstall had no service to register and nothing
        # is running. Saying "installed and running" here — as this banner used
        # to, unconditionally — sends the operator to a URL that answers
        # nothing. Plain container images are the common case.
        say "${bold}MoonlightWeb installed.${reset}"
        say ""
        say "  ${bold}It is not running yet${reset} — there is no systemd on this system to run"
        say "  it as a service, so nothing started it. Start it yourself:"
        say ""
        say "      ${bold}moonlightweb &${reset}"
        say ""
        say "  ${dim}Then 'moonlightweb --status' prints the URLs and the access PIN.${reset}"
        say ""
    else
        say "${bold}MoonlightWeb installed and running as a system service.${reset}"
        say ""
        say "  ${dim}(server still starting — run 'moonlightweb --status')${reset}"
    fi

    say "  ${dim}This host cannot stream itself: no display to capture and no GPU to${reset}"
    say "  ${dim}encode with. Add your streaming hosts by IP from the web UI.${reset}"
    say ""
    cli_hint
    [ -d /run/systemd/system ] &&
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
    say "  ${dim}macOS cannot grant screen capture programmatically, and this Mac cannot${reset}"
    say "  ${dim}stream its own screen without it: allow MoonlightWeb in System Settings →${reset}"
    say "  ${dim}Privacy & Security → Screen Recording, then quit and reopen MoonlightWeb${reset}"
    say "  ${dim}(macOS applies it on relaunch only).${reset}"
    say ""
}

# ── Linux: the signed repository, and what to do without one ───────────────
# `curl … | sudo tee … || die` reports *tee's* exit status, not curl's: a 404
# body is happily written out, tee exits 0, and die never fires — leaving an
# empty keyring and an empty source under /etc, and surfacing twenty lines
# later as "Package moonlightweb not found" (issue #21). So every repository
# file is fetched to a temp file and checked before anything under /etc is
# touched — and a repository that is not there is not fatal at all: the
# release carries the very same .deb and .rpm.
fetch_repo_file() { # url dest
    curl -fsSL -o "$2" "$1" || return 1
    # A 200 with an empty body is no better here than a 404.
    [ -s "$2" ] || return 1
}

# What an earlier run of the broken version left behind, empty. apt-get update
# chokes on a source whose Signed-By keyring is unreadable, and nothing else
# would ever clear them — only ever removes a file that holds nothing.
clean_empty() {
    for _f in "$@"; do
        if [ -e "$_f" ] && [ ! -s "$_f" ]; then $SUDO rm -f "$_f"; fi
    done
    return 0
}

repo_missing_note() {
    say ""
    say "  ${dim}No package repository at $PACKAGES — installing the package from${reset}"
    say "  ${dim}the release instead. Same package, same service, but the system${reset}"
    say "  ${dim}updater will not see it: re-run this script to upgrade.${reset}"
    say ""
}

# The same .deb/.rpm the repository would have served, taken straight from the
# GitHub release. The package manager still installs it — dependencies and
# postinstall included — only the automatic updates are lost.
install_release_package() { # deb|rpm [manager]
    _ext=$1 _mgr=${2:-}
    VERSION="${MW_VERSION:-}"
    if [ -z "$VERSION" ]; then
        say "${dim}Looking up the latest release…${reset}"
        VERSION="$(latest_version)"
    fi
    PKG="moonlightweb-${VERSION}-linux-x64.${_ext}"
    URL="https://github.com/$REPO/releases/download/v${VERSION}/${PKG}"

    _tmp="$(mktemp -d "${TMPDIR:-/tmp}/moonlightweb.XXXXXX")"
    say "${bold}Downloading MoonlightWeb ${VERSION}${reset}"
    curl -fL --progress-bar -o "$_tmp/$PKG" "$URL" \
        || { rm -rf "$_tmp"; die "download failed: $URL"; }

    # A 404 page or a truncated transfer would otherwise reach dpkg/rpm and
    # fail there with a much less obvious message. Every .deb is an ar
    # archive; every .rpm starts with its own four-byte magic.
    _magic_ok=no
    case "$_ext" in
        deb) [ "$(head -c 7 "$_tmp/$PKG" 2>/dev/null)" = '!<arch>' ] && _magic_ok=yes ;;
        rpm) [ "$(head -c 4 "$_tmp/$PKG" 2>/dev/null | od -An -tx1 | tr -d ' \n')" \
               = "edabeedb" ] && _magic_ok=yes ;;
    esac
    [ "$_magic_ok" = yes ] \
        || { rm -rf "$_tmp"; die "the downloaded file is not a valid .$_ext — check $URL"; }

    say "${bold}Installing${reset}"
    need_root
    case "$_ext" in
        deb)
            # A local .deb still resolves its dependencies through apt — but
            # only against an index this machine already has; refresh it and
            # retry once rather than fail on a package list from last month.
            if ! DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y "$_tmp/$PKG"; then
                $SUDO apt-get update || true
                DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y "$_tmp/$PKG" \
                    || { rm -rf "$_tmp"; die "installing $PKG failed"; }
            fi ;;
        rpm)
            # The release packages are signed, but the public key only ever
            # reaches the system with the repository — which is what is
            # missing here, so the one-off check has to be waived.
            case "$_mgr" in
                zypper) $SUDO zypper --non-interactive install --allow-unsigned-rpm "$_tmp/$PKG" \
                            || { rm -rf "$_tmp"; die "installing $PKG failed"; } ;;
                *) $SUDO "${_mgr:-dnf}" install -y --nogpgcheck "$_tmp/$PKG" \
                            || { rm -rf "$_tmp"; die "installing $PKG failed"; } ;;
            esac ;;
    esac
    rm -rf "$_tmp"
    MW_CLI=moonlightweb
}

# ── Linux: APT (Debian, Ubuntu, Mint, Pop!_OS) ─────────────────────────────
install_apt() {
    say "${bold}Adding the MoonlightWeb APT repository${reset}"
    need_root
    clean_empty /etc/apt/keyrings/moonlightweb.gpg \
                /etc/apt/sources.list.d/moonlightweb.sources

    _tmp="$(mktemp -d "${TMPDIR:-/tmp}/moonlightweb.XXXXXX")"
    if ! fetch_repo_file "$PACKAGES/moonlightweb.gpg" "$_tmp/moonlightweb.gpg" \
    || ! fetch_repo_file "$PACKAGES/moonlightweb.sources" "$_tmp/moonlightweb.sources" \
    || ! grep -q '^Types:' "$_tmp/moonlightweb.sources"; then
        rm -rf "$_tmp"
        repo_missing_note
        install_release_package deb
        return 0
    fi

    # A dearmored keyring, so gnupg does not have to be installed. Scoped to
    # this one source via Signed-By — never added to the global trusted set.
    $SUDO install -d -m 0755 /etc/apt/keyrings
    $SUDO install -m 0644 "$_tmp/moonlightweb.gpg" /etc/apt/keyrings/moonlightweb.gpg
    $SUDO install -m 0644 "$_tmp/moonlightweb.sources" \
        /etc/apt/sources.list.d/moonlightweb.sources
    rm -rf "$_tmp"

    say "${bold}Installing${reset}"
    # Not gated on apt-get update's exit status: it is non-zero when *any*
    # source fails, and a user with one broken third-party repository would
    # lose ours over it. What matters is whether the package installs.
    $SUDO apt-get update || true
    if ! DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y moonlightweb; then
        # The repository answered, but it carries nothing this machine can
        # install. Take the source back out rather than leave every later
        # apt-get run complaining about it.
        $SUDO rm -f /etc/apt/sources.list.d/moonlightweb.sources \
                    /etc/apt/keyrings/moonlightweb.gpg
        repo_missing_note
        install_release_package deb
        return 0
    fi
    MW_CLI=moonlightweb
}

# ── Linux: DNF / YUM (Fedora, RHEL, Nobara) and zypper (openSUSE) ──────────
install_rpm() {
    _mgr=$1 _dir=$2
    say "${bold}Adding the MoonlightWeb repository${reset}"
    need_root
    clean_empty "$_dir/moonlightweb.repo"

    _tmp="$(mktemp -d "${TMPDIR:-/tmp}/moonlightweb.XXXXXX")"
    if ! fetch_repo_file "$PACKAGES/moonlightweb.repo" "$_tmp/moonlightweb.repo" \
    || ! grep -q '^baseurl=' "$_tmp/moonlightweb.repo"; then
        rm -rf "$_tmp"
        repo_missing_note
        install_release_package rpm "$_mgr"
        return 0
    fi
    $SUDO install -m 0644 "$_tmp/moonlightweb.repo" "$_dir/moonlightweb.repo"
    rm -rf "$_tmp"

    say "${bold}Installing${reset}"
    _failed=0
    case "$_mgr" in
        # -y also accepts the one-time import of the repository signing key.
        dnf|yum) $SUDO "$_mgr" install -y moonlightweb || _failed=1 ;;
        zypper)  $SUDO zypper --non-interactive --gpg-auto-import-keys refresh || _failed=1
                 if [ "$_failed" = 0 ]; then
                     $SUDO zypper --non-interactive install moonlightweb || _failed=1
                 fi ;;
    esac
    if [ "$_failed" = 1 ]; then
        $SUDO rm -f "$_dir/moonlightweb.repo"
        repo_missing_note
        install_release_package rpm "$_mgr"
        return 0
    fi
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
Comment=Stream this PC to any browser
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
    # File capabilities do not survive inside an AppImage (its FUSE mount is
    # nosuid), and the KMS screen capture needs one — only the .deb/.rpm and
    # the AUR package can grant it. Said here rather than discovered later as a
    # host card that never appears.
    say "  ${dim}An AppImage cannot capture this machine's own screen (no file capability${reset}"
    say "  ${dim}inside it): it streams from Sunshine/Apollo/Wolf hosts on your network.${reset}"
    say "  ${dim}To host from this machine, install the .deb, the .rpm or the AUR package.${reset}"
    say ""
    cli_hint
    # Nothing started the app here — unlike the packages, an AppImage has no
    # postinstall — so the internet link cannot be enabled from this script.
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

# ── Can a device on the LAN actually get in? ───────────────────────────────
# Whether a *remote* machine can open a TCP connection to this one is not
# knowable from this one: only the far end can answer it, and there is nothing
# here to ask. What is knowable is what stands in the way locally — so this
# reports the blocker it finds, and says so plainly when it finds none, rather
# than claiming a reachability it cannot verify.
#
# It replaces a flat "the ports are opened in firewalld/ufw when one of them is
# active", which was true and yet useless: it never said whether either one was
# active here, and it said nothing at all about the two cases that actually bite
# — a container whose ports were never published, and a plain iptables/nftables
# policy, which the postinstall does not touch.
#
# Root is available: need_root ran for every path that reaches this point.
PORTS_HINT="443/tcp, 80/tcp and 48550-48573/udp"

lan_access_note() {
    # A container is reached only through the ports its host published. The
    # firewall *inside* it is irrelevant, and usually absent — so check first,
    # or the branches below would happily report "nothing is blocking it".
    if in_container; then
        say "  ${bold}This is a container${reset} — nothing on your LAN reaches it unless the"
        say "  ports were published when it was started:"
        say ""
        say "      ${dim}docker run -p 443:443 -p 80:80 -p 48550-48573:48550-48573/udp …${reset}"
        say ""
        say "  ${dim}Published ports cannot be added to a running container; it has to be${reset}"
        say "  ${dim}recreated. --net=host sidesteps the question entirely.${reset}"
        say ""
        return 0
    fi

    if have firewall-cmd && $SUDO firewall-cmd --state > /dev/null 2>&1; then
        # The media block counts as much as 443: without it the page loads and
        # every stream dies at ICE, which looks like a broken transport.
        fw_ports=$($SUDO firewall-cmd --list-ports 2>/dev/null)
        if echo "$fw_ports" | grep -q '443/tcp' && echo "$fw_ports" | grep -q '48550-48573/udp'; then
            say "  ${dim}firewalld is active and lets $PORTS_HINT through.${reset}"
        else
            say "  ${red}firewalld is active and does not let $PORTS_HINT through${reset} —"
            say "  LAN devices will be refused, or reach the page but never a stream."
            say "  Open the ports with:"
            say ""
            say "      ${dim}sudo firewall-cmd --permanent --add-port=443/tcp \\${reset}"
            say "      ${dim}     --add-port=80/tcp --add-port=48550-48573/udp${reset}"
            say "      ${dim}sudo firewall-cmd --reload${reset}"
        fi
        say ""
        return 0
    fi

    if have ufw && $SUDO ufw status 2>/dev/null | grep -qi '^Status: active'; then
        ufw_rules=$($SUDO ufw status 2>/dev/null)
        if echo "$ufw_rules" | grep -q '443' && echo "$ufw_rules" | grep -q '48550:48573/udp'; then
            say "  ${dim}ufw is active and lets $PORTS_HINT through.${reset}"
        else
            say "  ${red}ufw is active and does not let $PORTS_HINT through${reset} — LAN devices"
            say "  will be refused, or reach the page but never a stream. Open the ports with:"
            say ""
            say "      ${dim}sudo ufw allow 443/tcp${reset}"
            say "      ${dim}sudo ufw allow 80/tcp${reset}"
            say "      ${dim}sudo ufw allow 48550:48573/udp${reset}"
        fi
        say ""
        return 0
    fi

    # No managed firewall, but a bare netfilter policy of DROP/REJECT blocks
    # just as effectively — and the postinstall, which only knows firewalld and
    # ufw, left it alone. Worth naming, since nothing else would explain it.
    if have iptables && $SUDO iptables -S INPUT 2>/dev/null | grep -qE '^-P INPUT (DROP|REJECT)'; then
        say "  ${red}A packet filter is active${reset} (iptables/nftables, default policy DROP)"
        say "  and neither firewalld nor ufw manages it, so nothing opened the ports"
        say "  for you. Allow $PORTS_HINT, or LAN devices will be refused."
        say ""
        return 0
    fi

    say "  ${dim}Nothing on this machine blocks a LAN device from connecting. If one${reset}"
    say "  ${dim}still cannot, the ports to open are $PORTS_HINT.${reset}"
    say ""
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
        set_admin_password
        done_banner_headless
    else
        done_banner
    fi
    lan_access_note
}

case "$(uname -s)" in
    Darwin) install_macos ;;
    Linux)  install_linux ;;
    *) die "unsupported system: $(uname -s). See https://moonlightweb.top/#download" ;;
esac
