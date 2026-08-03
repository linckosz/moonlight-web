#!/usr/bin/env bash
# ============================================================================
# MoonlightWeb — build .deb and .rpm from the linuxdeploy AppDir.
#
# The AppDir already contains the binary, the bundled Qt runtime (libs +
# plugins, ELF rpaths rewritten to $ORIGIN-relative by linuxdeploy) and the
# frontend, so the same tree is simply relocated under /opt/moonlightweb and
# packaged twice with fpm. Result: self-contained packages that install with a
# double-click on every major desktop distro:
#   .deb → Debian, Ubuntu, Mint, Pop!_OS...    (App Center / GDebi)
#   .rpm → Fedora, RHEL, openSUSE, Nobara...   (GNOME Software / YaST)
# Arch-based and immutable gaming distros (SteamOS, Bazzite) use the AppImage.
#
# Usage: make-packages.sh <AppDir> <version> <outdir>
# Requires: fpm (gem install fpm) + rpmbuild (apt-get install rpm).
# ============================================================================
set -euo pipefail

APPDIR=$(realpath "$1")
VERSION=$2
OUT=$(realpath "$3")

PREFIX=/opt/moonlightweb
ROOT=$(mktemp -d)
PKG="$ROOT/pkgroot"
trap 'rm -rf "$ROOT"' EXIT

mkdir -p "$PKG$PREFIX" "$PKG/usr/bin" "$PKG/usr/share/applications" \
         "$PKG/usr/share/metainfo" \
         "$PKG/usr/share/icons/hicolor/512x512/apps"

# Relocatable bundle straight from the AppDir (bin/ also holds frontend/).
for d in bin lib plugins translations; do
    if [ -d "$APPDIR/usr/$d" ]; then cp -a "$APPDIR/usr/$d" "$PKG$PREFIX/"; fi
done

# Plugin/library resolution when launched outside the AppImage runtime.
cat > "$PKG$PREFIX/bin/qt.conf" <<EOF
[Paths]
Prefix = ..
Plugins = plugins
EOF

# CLI entry point on PATH. Also what the headless operator commands are invoked
# as (`moonlightweb --status`, `--new-pin`, `--enable-internet`).
ln -sfn "$PREFIX/bin/MoonlightWeb" "$PKG/usr/bin/moonlightweb"

# systemd unit for headless installs. Vendor directory, not /etc/systemd/system:
# a unit that merely *exists* there does nothing until something enables it, so
# a desktop install is unaffected, upgrades refresh it like any other packaged
# file, and /etc stays free for an admin override (systemd's own precedence
# rules). postinst enables it only when the machine has no desktop.
mkdir -p "$PKG/usr/lib/systemd/system"
cp "$(dirname "$0")/../systemd/moonlightweb.service" \
   "$PKG/usr/lib/systemd/system/moonlightweb.service"

cp "$APPDIR/MoonlightWeb.png" \
   "$PKG/usr/share/icons/hicolor/512x512/apps/moonlightweb.png"

# Menu entry (absolute Exec: /opt is not on PATH for .desktop resolution).
# Named after the AppStream component ID so a software centre matches the two
# without ambiguity; the <launchable> in the metainfo points back here.
APPID=top.moonlightweb.MoonlightWeb
cat > "$PKG/usr/share/applications/$APPID.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=MoonlightWeb
Comment=Sunshine streaming client for the browser
Exec=$PREFIX/bin/MoonlightWeb
Icon=moonlightweb
Categories=Network;Game;
Keywords=streaming;sunshine;moonlight;gaming;remote;
Terminal=false
StartupWMClass=MoonlightWeb
EOF

# AppStream metadata: what GNOME Software / KDE Discover / Ubuntu App Center
# actually index. Without it the app is installable but invisible in every
# graphical software centre. The same file is compiled into the repository
# index by the `linux-repo` job (see .github/workflows/release.yml).
sed -e "s/@MW_VERSION@/$VERSION/g" -e "s/@MW_DATE@/$(date -u +%Y-%m-%d)/g" \
    "$(dirname "$0")/$APPID.metainfo.xml" \
    > "$PKG/usr/share/metainfo/$APPID.metainfo.xml"

# Catches a malformed template before it reaches a user's software centre.
# Advisory only: appstreamcli is not installed on every build host, and its
# newer releases add warnings that would otherwise break packaging.
if command -v appstreamcli >/dev/null 2>&1; then
    appstreamcli validate --no-net "$PKG/usr/share/metainfo/$APPID.metainfo.xml" \
        || echo "warning: AppStream validation reported issues (not fatal)"
fi

# Post-install / pre-remove hooks shared by both package formats.
cat > "$ROOT/postinst.sh" <<'EOF'
#!/bin/sh
update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true

# Open the server ports in the system firewall (best-effort). Unlike Windows /
# macOS, Linux netfilter firewalls are port-based (no per-program rule), and this
# runs before the app picks a port, so we open the defaults: 443/tcp + 80/tcp
# (HTTPS + HTTP→HTTPS redirect) and 47999/udp (stream). A non-default or
# per-instance parity port under an *active* firewall would need a manual rule.
# firewalld (Fedora/RHEL — active by default) and ufw (Debian/Ubuntu) only.
if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
    for p in 443/tcp 80/tcp 47999/udp; do
        firewall-cmd --permanent --add-port="$p" >/dev/null 2>&1 || true
    done
    firewall-cmd --reload >/dev/null 2>&1 || true
elif command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -qi '^Status: active'; then
    for p in 443/tcp 80/tcp 47999/udp; do
        ufw allow "$p" >/dev/null 2>&1 || true
    done
fi

# Upgrade over a headless install: the service owns the binary we just replaced.
# Restart it onto the new one and stop here — everything below is first-install
# wiring that is already done. `restart` rather than `try-restart` because prerm
# has just stopped it, so it is inactive and try-restart would leave it down;
# gating on is-enabled is what keeps a deliberately disabled service disabled.
if command -v systemctl >/dev/null 2>&1 &&
   systemctl is-enabled --quiet moonlightweb.service 2>/dev/null; then
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl restart moonlightweb.service >/dev/null 2>&1 || true
    exit 0
fi

# Best-effort: start MoonlightWeb inside the active graphical session so the
# first-run setup page opens right after install (postinst runs as root with no
# display; systemd-run --user runs the app under the user's session manager).
# Skipped when already running (e.g. upgrade) or when no session user is found —
# the Apps menu entry covers those.
started=""
if pgrep -f /opt/moonlightweb/bin/MoonlightWeb >/dev/null 2>&1; then
    started=1
else
    for u in $(loginctl list-users --no-legend 2>/dev/null | awk '{print $2}'); do
        uid=$(id -u "$u" 2>/dev/null) || continue
        rt="/run/user/$uid"
        [ -S "$rt/bus" ] || continue
        asuser="runuser -u $u -- env XDG_RUNTIME_DIR=$rt DBUS_SESSION_BUS_ADDRESS=unix:path=$rt/bus"

        # The app only opens the browser when it sees a display server
        # (hasGuiSession(), backend/src/main.cpp), and the user's systemd manager
        # does not reliably carry DISPLAY/WAYLAND_DISPLAY — a unit started
        # without them stays silently headless. Resolve them here and pass them
        # explicitly: ask the manager first, then fall back to the Wayland socket
        # in XDG_RUNTIME_DIR and to logind's X11 display name.
        userenv=$($asuser systemctl --user show-environment 2>/dev/null)
        wl=$(printf '%s\n' "$userenv" | sed -n 's/^WAYLAND_DISPLAY=//p')
        dp=$(printf '%s\n' "$userenv" | sed -n 's/^DISPLAY=//p')
        if [ -z "$wl" ]; then
            for s in "$rt"/wayland-*; do
                case "$s" in *.lock) continue ;; esac
                if [ -S "$s" ]; then wl=${s##*/}; break; fi
            done
        fi
        if [ -z "$dp" ]; then
            sid=$(loginctl show-user "$u" -p Display --value 2>/dev/null)
            [ -n "$sid" ] && dp=$(loginctl show-session "$sid" -p Display --value 2>/dev/null)
        fi
        # No display at all: this user is on a text console, not a desktop.
        [ -n "$wl" ] || [ -n "$dp" ] || continue

        setenv=""
        [ -n "$wl" ] && setenv="$setenv --setenv=WAYLAND_DISPLAY=$wl"
        [ -n "$dp" ] && setenv="$setenv --setenv=DISPLAY=$dp"

        # $setenv is deliberately unquoted: it must split into separate flags
        # (values are socket/display names — no whitespace).
        if $asuser systemd-run --user --collect --quiet $setenv \
            /opt/moonlightweb/bin/MoonlightWeb >/dev/null 2>&1; then
            started=1
            break
        fi
    done
fi

# ── Headless install ───────────────────────────────────────────────────────
# Nothing above could start the app: no logged-in user has a display. On a
# server, a container host or a box installed over SSH, that is the normal
# state, and the right answer is a system service that runs before (and
# without) any login — not a menu entry nobody will ever click.
#
# `systemctl get-default` is the discriminator: a desktop distribution boots
# graphical.target and merely happens to have nobody logged in right now, so
# installing a system-wide service there would fight the per-session launch on
# the next login. MW_HEADLESS=1 forces the branch (containers, images).
if [ -z "$started" ] && command -v systemctl >/dev/null 2>&1 && [ -d /run/systemd/system ]; then
    default_target=$(systemctl get-default 2>/dev/null || echo "")
    if [ "${MW_HEADLESS:-0}" = "1" ] || [ "$default_target" != "graphical.target" ]; then
        # The unit file is already on disk (/usr/lib/systemd/system, shipped by
        # this package) — enabling is the whole install step.
        systemctl daemon-reload >/dev/null 2>&1 || true
        if systemctl enable --now moonlightweb.service >/dev/null 2>&1; then
            # Sunshine is deliberately absent: it captures a display and encodes
            # on a GPU, neither of which exists here. The app detects that on its
            # own (mw::hasDesktopSession) and never offers to install it — this
            # box is a relay to Sunshine/GameStream hosts elsewhere on the LAN.
            cat <<'BANNER'

MoonlightWeb is installed and running as a system service (headless mode).

  Status, URLs and access PIN:   moonlightweb --status
  Allow a new device:            moonlightweb --new-pin
  Publish it on the internet:    moonlightweb --enable-internet

On your LAN there is nothing else to configure — open https://<this-machine-ip>
from any browser. Sunshine was not installed: this host has no display to
capture and no GPU to encode with; add your streaming hosts by IP instead.

BANNER
        else
            echo "warning: could not enable the moonlightweb service — start it with" >&2
            echo "         sudo systemctl enable --now moonlightweb" >&2
        fi
    fi
fi
exit 0
EOF
cat > "$ROOT/prerm.sh" <<'EOF'
#!/bin/sh
# Is the headless service the thing running MoonlightWeb here?
service_active=""
if command -v systemctl >/dev/null 2>&1 &&
   systemctl is-enabled --quiet moonlightweb.service 2>/dev/null; then
    service_active=1
fi

# Stop a running instance so the package files are not held open. Under the
# service, go through systemctl rather than pkill: a signal counts as a failure
# for Restart=on-failure, so pkill would have systemd bring the old binary
# straight back up in the middle of the file copy. `systemctl stop` is an
# operator-initiated stop and is never restarted.
if [ -n "$service_active" ]; then
    systemctl stop moonlightweb.service >/dev/null 2>&1 || true
else
    pkill -f /opt/moonlightweb/bin/MoonlightWeb >/dev/null 2>&1 || true
fi

# Remove the firewall ports opened at install (best-effort; skip on upgrade).
if [ "${1:-}" != "upgrade" ] && [ "${1:-0}" != "1" ]; then
    # Real removal: drop the enablement symlinks before the package manager
    # deletes the unit file, or systemd is left with a dangling enabled unit.
    if [ -n "$service_active" ]; then
        systemctl disable moonlightweb.service >/dev/null 2>&1 || true
    fi

    if command -v firewall-cmd >/dev/null 2>&1 && firewall-cmd --state >/dev/null 2>&1; then
        for p in 443/tcp 80/tcp 47999/udp; do
            firewall-cmd --permanent --remove-port="$p" >/dev/null 2>&1 || true
        done
        firewall-cmd --reload >/dev/null 2>&1 || true
    elif command -v ufw >/dev/null 2>&1 && ufw status 2>/dev/null | grep -qi '^Status: active'; then
        for p in 443/tcp 80/tcp 47999/udp; do
            ufw delete allow "$p" >/dev/null 2>&1 || true
        done
    fi
fi
exit 0
EOF

# Qt and OpenSSL are bundled, but linuxdeploy deliberately leaves the graphics
# and font stack to the system — those libraries are driver- and distro-coupled
# and must never be shipped. They are NEEDED entries of the executable itself,
# not lazily dlopen'd, so the process does not start without them:
#
#   moonlightweb: error while loading shared libraries: libGLX.so.0
#
# Every desktop session has them, which is why nothing was declared here at
# first. A minimal server or container image does not — and that is precisely
# where the headless install is meant to run, so the CLI (`moonlightweb
# --status`) died before printing a line. Declaring them is what makes the
# package manager pull them in.
#
# The list is what `ldd` reports unresolved on a bare debian:12 (plus libxcb,
# needed by the xcb platform plugin on a desktop). Keep it in step with
# backend/packaging/aur/PKGBUILD, which names the same set for Arch.
deb_depends=(
    --depends libgl1 --depends libopengl0 --depends libegl1
    --depends libfontconfig1 --depends libfreetype6
    --depends libx11-6 --depends libx11-xcb1 --depends libxcb1
)
# RPM resolves soname provides, which every RPM distro generates the same way —
# unlike package names, which differ between Fedora (libglvnd-glx) and openSUSE
# (Mesa-libGL1). Depending on the soname keeps one .rpm valid for both.
rpm_depends=(
    --depends "libGLX.so.0()(64bit)" --depends "libOpenGL.so.0()(64bit)"
    --depends "libGL.so.1()(64bit)" --depends "libEGL.so.1()(64bit)"
    --depends "libfontconfig.so.1()(64bit)" --depends "libfreetype.so.6()(64bit)"
    --depends "libX11.so.6()(64bit)" --depends "libX11-xcb.so.1()(64bit)"
    --depends "libxcb.so.1()(64bit)"
)

common=(
    -s dir -n moonlightweb -v "$VERSION"
    --license GPL-3.0 --vendor MoonlightWeb
    --url "https://moonlightweb.top/"
    --description "Sunshine streaming client for the browser (self-contained, bundled Qt runtime)"
    --maintainer "MoonlightWeb"
    --after-install "$ROOT/postinst.sh"
    --before-remove "$ROOT/prerm.sh"
    -C "$PKG"
)

fpm "${common[@]}" "${deb_depends[@]}" \
    -t deb -a amd64  -p "$OUT/moonlightweb-$VERSION-linux-x64.deb" .
fpm "${common[@]}" "${rpm_depends[@]}" \
    -t rpm -a x86_64 -p "$OUT/moonlightweb-$VERSION-linux-x64.rpm" .

echo "Packages written to $OUT:"
ls -lh "$OUT"/moonlightweb-"$VERSION"-linux-x64.{deb,rpm}
