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

# CLI entry point on PATH.
ln -sfn "$PREFIX/bin/MoonlightWeb" "$PKG/usr/bin/moonlightweb"

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

# Best-effort: start MoonlightWeb inside the active graphical session so the
# first-run setup page opens right after install (postinst runs as root with no
# display; systemd-run --user runs the app under the user's session manager).
# Skipped when already running (e.g. upgrade) or when no session user is found —
# the Apps menu entry covers those.
if ! pgrep -f /opt/moonlightweb/bin/MoonlightWeb >/dev/null 2>&1; then
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
            break
        fi
    done
fi
exit 0
EOF
cat > "$ROOT/prerm.sh" <<'EOF'
#!/bin/sh
# Stop a running instance so the package files are not held open.
pkill -f /opt/moonlightweb/bin/MoonlightWeb >/dev/null 2>&1 || true

# Remove the firewall ports opened at install (best-effort; skip on upgrade).
if [ "${1:-}" != "upgrade" ] && [ "${1:-0}" != "1" ]; then
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

# No hard dependencies: Qt/OpenSSL are bundled; the libraries linuxdeploy leaves
# to the system (glibc, libX11, libGL, fontconfig...) exist on any desktop
# session, and naming them per-distro would make the packages distro-specific.
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

fpm "${common[@]}" -t deb -a amd64  -p "$OUT/moonlightweb-$VERSION-linux-x64.deb" .
fpm "${common[@]}" -t rpm -a x86_64 -p "$OUT/moonlightweb-$VERSION-linux-x64.rpm" .

echo "Packages written to $OUT:"
ls -lh "$OUT"/moonlightweb-"$VERSION"-linux-x64.{deb,rpm}
