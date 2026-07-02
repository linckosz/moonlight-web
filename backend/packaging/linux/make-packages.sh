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
cat > "$PKG/usr/share/applications/moonlightweb.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=MoonlightWeb
Comment=Sunshine streaming client for the browser
Exec=$PREFIX/bin/MoonlightWeb
Icon=moonlightweb
Categories=Network;Game;
Terminal=false
EOF

# Post-install / pre-remove hooks shared by both package formats.
cat > "$ROOT/postinst.sh" <<'EOF'
#!/bin/sh
update-desktop-database /usr/share/applications >/dev/null 2>&1 || true
gtk-update-icon-cache -q /usr/share/icons/hicolor >/dev/null 2>&1 || true
exit 0
EOF
cat > "$ROOT/prerm.sh" <<'EOF'
#!/bin/sh
# Stop a running instance so the package files are not held open.
pkill -f /opt/moonlightweb/bin/MoonlightWeb >/dev/null 2>&1 || true
exit 0
EOF

# No hard dependencies: Qt/OpenSSL are bundled; the libraries linuxdeploy leaves
# to the system (glibc, libX11, libGL, fontconfig...) exist on any desktop
# session, and naming them per-distro would make the packages distro-specific.
common=(
    -s dir -n moonlightweb -v "$VERSION"
    --license GPL-3.0 --vendor MoonlightWeb
    --url "https://github.com/moonlight-stream/moonlight-web"
    --description "Sunshine streaming client for the browser (self-contained, bundled Qt runtime)"
    --maintainer "MoonlightWeb"
    --after-install "$ROOT/postinst.sh"
    --before-remove "$ROOT/prerm.sh"
    -C "$PKG"
)

fpm "${common[@]}" -t deb -a amd64  -p "$OUT/moonlightweb-linux-x64.deb" .
fpm "${common[@]}" -t rpm -a x86_64 -p "$OUT/moonlightweb-linux-x64.rpm" .

echo "Packages written to $OUT:"
ls -lh "$OUT"/moonlightweb-linux-x64.{deb,rpm}
