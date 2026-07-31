#!/usr/bin/env bash
# ============================================================================
# MoonlightWeb — build the signed APT and DNF repositories.
#
# Takes the .deb and .rpm the `linux` job just built, indexes them and signs the
# indexes. The result is a plain directory of static files, uploaded straight to
# GitHub Pages as a deployment artifact — nothing is ever committed, so a
# repository of binaries never lands in anyone's clone.
#
# Only the version being released is published. apt and dnf never resolve
# anything but the newest, and older packages stay on the GitHub releases page,
# so keeping a pool of past versions would cost bandwidth for nothing.
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
#   deb/dists/stable/main/dep11/{Components-amd64.yml.gz,icons-<size>.tar.gz}
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

# `appstreamcli compose` writes a catalog named after the origin and, despite
# the DEP-11 vocabulary around it, always in XML — the CLI exposes no format
# switch (asc_compose.c: "%s.xml.gz"). apt fetches DEP-11 *YAML* under the
# canonical dep11/Components-<arch> name, so the catalog is converted: the
# output format of `convert` is inferred from the input suffix, .xml.gz in
# giving YAML out.
CATALOG="$WORK/as/data/$ORIGIN.xml.gz"
if [ ! -f "$CATALOG" ]; then
    echo "error: appstreamcli compose produced no catalog at $CATALOG" >&2
    echo "       check /usr/share/metainfo in the .deb. It did write:" >&2
    find "$WORK/as" -type f >&2
    exit 1
fi
rm -f "$DIST/main/dep11"/*
appstreamcli convert "$CATALOG" "$DIST/main/dep11/Components-amd64.yml.gz"

# Icons are exported loose, as <size>/<component-id>.png; a DEP-11 client wants
# one tarball per size beside the catalog. Not fatal when absent: the listing
# still works, it just shows no icon. 48x48 is normally missing — compose
# refuses to upscale, and the package ships a single 512x512 source.
for d in "$WORK/as/icons"/*/; do
    [ -d "$d" ] || continue
    tar -C "$d" -czf "$DIST/main/dep11/icons-$(basename "$d").tar.gz" .
done

# ── APT indexes ─────────────────────────────────────────────────────────────
(
    cd "$SITE/deb"
    # No --arch: it does not filter on the Architecture field, it makes the
    # scanner glob for "*_<arch>.deb" (writer.cc, AddPattern). Our packages are
    # named moonlightweb-<version>-linux-x64.deb — the name UpdateChecker
    # matches — so the pattern matched nothing and published an empty index,
    # with apt then reporting "unable to locate package". Unfiltered, the
    # scanner reads Architecture from each control file, which is what we want:
    # the pool only ever holds one amd64 package anyway.
    apt-ftparchive packages pool > "$WORK/Packages"
    # An empty index is worse than a failed job: the repository stays online and
    # installs nothing.
    if [ ! -s "$WORK/Packages" ]; then
        echo "error: apt-ftparchive indexed no package. pool/ holds:" >&2
        find pool -type f >&2
        exit 1
    fi
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

# Artifact deployments skip Jekyll entirely, so this is belt and braces: it
# keeps the tree servable as-is should it ever be published from a branch.
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
# GitHub documents 1 GB as the size a published Pages site should stay under.
# A single release is nowhere near it; this fires only if the bundle balloons.
if [ "$SIZE_KB" -gt 819200 ]; then
    echo "::warning title=Package repository is large::$((SIZE_KB / 1024)) MB published to GitHub Pages (documented ceiling 1 GB) - the payload needs object storage."
fi
