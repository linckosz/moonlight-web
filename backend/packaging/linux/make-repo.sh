#!/usr/bin/env bash
# ============================================================================
# MoonlightWeb — build the signed APT and DNF repositories.
#
# Takes the .deb and .rpm the `linux` job just built, folds them into the
# existing published tree (so previous versions stay installable), regenerates
# every index and signs it. The result is a plain directory of static files:
# GitHub Pages serves it as-is, and so would any web host.
#
# What this buys over a downloaded .deb:
#   * `apt install moonlightweb` / `dnf install moonlightweb`
#   * automatic updates through the system's own updater
#   * a listing in GNOME Software / KDE Discover / Ubuntu App Center, via the
#     DEP-11 index compiled from the metainfo file inside the package
#
# Layout produced under <site>:
#   moonlightweb.gpg / .asc     public key (dearmored for apt, armored for rpm)
#   moonlightweb.sources        ready-made deb822 apt source
#   moonlightweb.repo           ready-made dnf repo definition
#   deb/pool/main/m/moonlightweb/*.deb
#   deb/dists/stable/{Release,Release.gpg,InRelease}
#   deb/dists/stable/main/binary-amd64/Packages{,.gz}
#   deb/dists/stable/main/dep11/{Components-*.yml.gz,icons-*.tar.gz}
#   rpm/x86_64/*.rpm  +  rpm/repodata/ (repomd.xml.asc)
#
# Usage: make-repo.sh <deb> <rpm> <version> <site-dir> <gpg-key-id>
# Requires: apt-utils, createrepo-c, rpm (rpmsign), appstream (appstreamcli),
#           gpg with <gpg-key-id> unlocked in the keyring.
# ============================================================================
set -euo pipefail

DEB=$(realpath "$1")
RPM=$(realpath "$2")
VERSION=$3
SITE=$(realpath -m "$4")
KEY=$5

# How many versions stay downloadable. apt/dnf only ever need the newest, so
# this is purely a rollback convenience — and GitHub Pages recommends staying
# under 1 GB total, which a self-contained Qt bundle eats quickly.
KEEP=2

ORIGIN=moonlightweb
BASEURL=https://linckosz.github.io/moonlight-web

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

POOL="$SITE/deb/pool/main/m/moonlightweb"
DIST="$SITE/deb/dists/stable"
RPMDIR="$SITE/rpm/x86_64"
mkdir -p "$POOL" "$DIST/main/binary-amd64" "$DIST/main/dep11" "$RPMDIR"

# ── Add this release ────────────────────────────────────────────────────────
cp -f "$DEB" "$POOL/"
cp -f "$RPM" "$RPMDIR/"

# dnf verifies package signatures when gpgcheck=1 (which the generated .repo
# sets). Unlike apt — where trust flows from the signed Release file alone —
# rpm signatures live inside each package, so they are applied here.
# --pinentry-mode loopback: the CI key has no passphrase and there is no tty.
rpmsign --key-id="$KEY" \
        --define "_gpg_sign_cmd_extra_args --pinentry-mode loopback" \
        --addsign "$RPMDIR/$(basename "$RPM")"

# ── Prune old versions ──────────────────────────────────────────────────────
# Filenames are moonlightweb-<version>-linux-x64.<ext>, so a version sort on the
# whole name orders releases correctly.
prune() {
    local dir=$1 ext=$2 n
    n=$(find "$dir" -maxdepth 1 -name "moonlightweb-*.$ext" | wc -l)
    if [ "$n" -gt "$KEEP" ]; then
        find "$dir" -maxdepth 1 -name "moonlightweb-*.$ext" -printf '%f\n' \
            | sort -V | head -n "$((n - KEEP))" \
            | while read -r old; do
                  echo "pruning $old"
                  rm -f "$dir/$old"
              done
    fi
}
prune "$POOL" deb
prune "$RPMDIR" rpm

# ── DEP-11: the software-centre index ───────────────────────────────────────
# Compiled from /usr/share/metainfo + the icon inside the package, so the
# listing can never drift from what actually gets installed. --no-net keeps the
# screenshot URLs as absolute links to moonlightweb.top instead of trying to
# mirror the images into the repository.
dpkg-deb -x "$POOL/$(basename "$DEB")" "$WORK/unpack"
appstreamcli compose \
    --origin="$ORIGIN" \
    --prefix=/usr \
    --result-root="$WORK/as" \
    --data-dir="$WORK/as/data" \
    --icons-dir="$WORK/as/icons" \
    --no-net \
    "$WORK/unpack"

# appstreamcli has moved these output paths between releases; locate them rather
# than hard-coding. Without them the packages install fine but never appear in a
# software centre, which is the whole point of this job — so this is fatal.
rm -f "$DIST/main/dep11"/*
find "$WORK/as" -name 'Components-*' -exec cp -f {} "$DIST/main/dep11/" \;
find "$WORK/as" -name 'icons-*.tar.gz' -exec cp -f {} "$DIST/main/dep11/" \;
if ! ls "$DIST/main/dep11"/Components-* >/dev/null 2>&1; then
    echo "error: appstreamcli compose produced no DEP-11 index" >&2
    echo "       check /usr/share/metainfo in the .deb" >&2
    exit 1
fi

# ── APT indexes ─────────────────────────────────────────────────────────────
(
    cd "$SITE/deb"
    apt-ftparchive --arch amd64 packages pool > "$WORK/Packages"
    cp -f "$WORK/Packages" "$DIST/main/binary-amd64/Packages"
    # -n: no timestamp in the gzip header, so an unchanged index stays
    # byte-identical and the checksums below do not churn.
    gzip -9cn "$WORK/Packages" > "$DIST/main/binary-amd64/Packages.gz"

    # Stale signatures must go before the Release file is hashed over the tree.
    rm -f "$DIST/Release" "$DIST/Release.gpg" "$DIST/InRelease"
    apt-ftparchive \
        -o APT::FTPArchive::Release::Origin=MoonlightWeb \
        -o APT::FTPArchive::Release::Label=MoonlightWeb \
        -o APT::FTPArchive::Release::Suite=stable \
        -o APT::FTPArchive::Release::Codename=stable \
        -o APT::FTPArchive::Release::Architectures=amd64 \
        -o APT::FTPArchive::Release::Components=main \
        -o APT::FTPArchive::Release::Description="MoonlightWeb — Sunshine streaming in the browser" \
        release dists/stable > "$WORK/Release"
    cp -f "$WORK/Release" "$DIST/Release"
)

# InRelease (inline signature, what modern apt fetches) + Release.gpg (detached,
# for older clients). Both, because they cost nothing and old apt still exists.
gpg --batch --yes --local-user "$KEY" --clearsign -o "$DIST/InRelease" "$DIST/Release"
gpg --batch --yes --local-user "$KEY" --detach-sign --armor -o "$DIST/Release.gpg" "$DIST/Release"

# ── DNF indexes ─────────────────────────────────────────────────────────────
createrepo_c --quiet "$SITE/rpm"
gpg --batch --yes --local-user "$KEY" --detach-sign --armor \
    "$SITE/rpm/repodata/repomd.xml"

# ── Public key + ready-made client config ───────────────────────────────────
gpg --batch --yes --armor --export "$KEY" > "$SITE/moonlightweb.asc"
gpg --batch --yes --export "$KEY" > "$SITE/moonlightweb.gpg"

cat > "$SITE/moonlightweb.sources" <<EOF
Types: deb
URIs: $BASEURL/deb
Suites: stable
Components: main
Architectures: amd64
Signed-By: /etc/apt/keyrings/moonlightweb.gpg
EOF

cat > "$SITE/moonlightweb.repo" <<EOF
[moonlightweb]
name=MoonlightWeb
baseurl=$BASEURL/rpm
enabled=1
gpgcheck=1
repo_gpgcheck=1
gpgkey=$BASEURL/moonlightweb.asc
EOF

# GitHub Pages serves the tree as a static site; without this it would try to
# run the directory listing through Jekyll and drop anything starting with "_".
touch "$SITE/.nojekyll"

cat > "$SITE/index.html" <<EOF
<!doctype html>
<meta charset="utf-8">
<title>MoonlightWeb package repository</title>
<meta name="viewport" content="width=device-width,initial-scale=1">
<style>
 body{font:16px/1.6 system-ui,sans-serif;max-width:44rem;margin:3rem auto;padding:0 1.2rem}
 pre{background:#f4f4f6;padding:.9rem 1rem;border-radius:8px;overflow-x:auto}
 code{font-family:ui-monospace,monospace}
 @media(prefers-color-scheme:dark){body{background:#16161a;color:#e8e8ea}pre{background:#242429}a{color:#8ab4f8}}
</style>
<h1>MoonlightWeb package repository</h1>
<p>Signed APT and DNF repositories for
   <a href="https://moonlightweb.top/">MoonlightWeb</a> — currently
   <strong>$VERSION</strong>. Installing from here means the system updater keeps
   the app up to date, and it shows up in your software centre.</p>

<h2>Debian, Ubuntu, Mint, Pop!_OS</h2>
<pre><code>sudo install -d /etc/apt/keyrings
curl -fsSL $BASEURL/moonlightweb.gpg | sudo tee /etc/apt/keyrings/moonlightweb.gpg > /dev/null
curl -fsSL $BASEURL/moonlightweb.sources | sudo tee /etc/apt/sources.list.d/moonlightweb.sources > /dev/null
sudo apt update &amp;&amp; sudo apt install moonlightweb</code></pre>

<h2>Fedora, RHEL, openSUSE, Nobara</h2>
<pre><code>sudo curl -fsSL -o /etc/yum.repos.d/moonlightweb.repo $BASEURL/moonlightweb.repo
sudo dnf install moonlightweb</code></pre>

<h2>Everything else</h2>
<p>Arch and derivatives: <code>moonlightweb-bin</code> on the AUR. Immutable
   distros (SteamOS, Bazzite): the AppImage from the
   <a href="https://github.com/linckosz/moonlight-web/releases">releases page</a>.
   Or run <code>curl -fsSL https://moonlightweb.top/install.sh | bash</code>,
   which picks the right one for you.</p>
EOF

# ── Report ──────────────────────────────────────────────────────────────────
SIZE_KB=$(du -sk "$SITE" | cut -f1)
echo "Repository built at $SITE ($((SIZE_KB / 1024)) MB)"
ls -lh "$POOL" "$RPMDIR"
# GitHub Pages publishes above 1 GB but warns; 800 MB is the point to lower KEEP
# or move the pool to object storage.
if [ "$SIZE_KB" -gt 819200 ]; then
    echo "::warning title=Package repository is large::$((SIZE_KB / 1024)) MB published to GitHub Pages (soft limit 1 GB) - lower KEEP in make-repo.sh."
fi
