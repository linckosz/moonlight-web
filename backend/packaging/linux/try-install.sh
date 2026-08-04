#!/usr/bin/env bash
# ============================================================================
# MoonlightWeb — run the one-line installer against a CI run, in a container.
#
#     backend/packaging/linux/try-install.sh [run-id | dir] [image]
#
# The first argument is a CI run to pull the packages from (or `latest`), or a
# directory that already holds a .deb and a .rpm — a local make-packages.sh
# build, or artifacts downloaded by hand. A directory needs no gh at all.
#
# What the user will eventually type is
#
#     curl -fsSL https://moonlightweb.top/install.sh | bash
#
# and on Linux that reaches for two things this repository only produces at
# release time: the signed apt/dnf repository on GitHub Pages (the `linux-repo`
# job is gated on refs/tags) and, for the AppImage fallback, a published
# release. So there is nothing to install from until the moment it is too late
# to find out that the installer is broken.
#
# This rebuilds that missing half locally, from the artifacts of any CI run:
#
#   1. downloads moonlightweb-linux-x64-* from the run (gh)
#   2. runs make-repo.sh over the .deb and .rpm — the same script the release
#      workflow runs — signed with a throwaway key generated on the spot, and
#      pointed at http://repo instead of the Pages site (MW_BASEURL)
#   3. serves that tree, plus website/install.sh, from an nginx container
#   4. drops you into a fresh distro container that pipes the served install.sh
#      into bash exactly as a user would, with MW_PAGES pointing at the local
#      repository
#
# Everything but the two URLs is the real thing: the real packages, the real
# postinstall, the real repository metadata, the real signature check.
#
# Notes
#   * Answering "Yes" to the Internet Access question registers a *real*
#     sub-domain of moonlightweb.top against this container's public IP — the
#     CI binaries carry the production DNS credentials. Answer No, or export
#     MW_INTERNET=0, unless that is what you are testing.
#   * No ports are published: nothing on your LAN reaches the container, and
#     the installer says so itself. Add -p to TRY_RUN_ARGS if you want in.
#   * The container has no systemd, so nothing starts the service — the
#     installer takes its "not running yet" branch and tells you to run
#     `moonlightweb &`. To exercise the systemd path, use an image that runs
#     systemd as PID 1 (jrei/systemd-ubuntu, etc.) instead.
#
# Requires: docker, gh (authenticated). Runs from Git Bash on Windows too.
# ============================================================================
set -euo pipefail

RUN_ID=${1:-latest}
IMAGE=${2:-debian:12}
REPO_SLUG=${MW_REPO:-linckosz/moonlight-web}

ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
WORK=${MW_TRY_DIR:-${TMPDIR:-/tmp}/moonlightweb-try}

NET=mw-try
SERVER=mw-try-repo

bold=$(printf '\033[1m'); dim=$(printf '\033[2m'); reset=$(printf '\033[0m')
say() { printf '%s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }

# Git Bash rewrites any argument that looks like a unix path before the program
# it is passed to ever sees it, so `docker run … bash /work/build-repo.sh` runs
# `bash C:/msys64/work/build-repo.sh` inside the container. These turn that off
# for this script; they mean nothing anywhere else.
export MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*'

# Docker on Windows wants a drive-letter path, which is not what Git Bash hands
# out for -v arguments; cygpath -m produces the E:/... form it accepts.
hostpath() { if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }

command -v docker > /dev/null || die "docker is required"

# ── 1. The packages ─────────────────────────────────────────────────────────
if [ -d "$RUN_ID" ]; then
    RUN_DIR="$WORK/local"
    PKGS=$(cd "$RUN_ID" && pwd)
    say "${dim}Packages taken from $PKGS${reset}"
    mkdir -p "$RUN_DIR"
else
    command -v gh > /dev/null || die "gh is required to fetch a run (and must be authenticated)"

    if [ "$RUN_ID" = latest ]; then
        say "${dim}Looking up the most recent CI run…${reset}"
        RUN_ID=$(gh run list --repo "$REPO_SLUG" --workflow CI --limit 1 \
                     --json databaseId --jq '.[0].databaseId') \
            || die "could not list runs of $REPO_SLUG"
        [ -n "$RUN_ID" ] || die "no CI run found in $REPO_SLUG"
    fi

    RUN_DIR="$WORK/$RUN_ID"
    PKGS="$RUN_DIR/pkgs"
    # Kept between runs: the artifact is over 100 MB and, once built, is fixed.
    if [ -z "$(ls "$PKGS"/*.deb 2> /dev/null || true)" ]; then
        ARTIFACT=$(gh api "repos/$REPO_SLUG/actions/runs/$RUN_ID/artifacts" \
                       --jq '.artifacts[] | select(.name | startswith("moonlightweb-linux-x64-")) | .name' \
                   | head -n1) || die "could not read the artifacts of run $RUN_ID"
        [ -n "$ARTIFACT" ] || die "run $RUN_ID published no Linux packages
  (the Installers job is skipped on pull_request, and artifacts expire)"

        say "${bold}Downloading $ARTIFACT${reset} ${dim}(run $RUN_ID)${reset}"
        mkdir -p "$PKGS"
        gh run download "$RUN_ID" --repo "$REPO_SLUG" -n "$ARTIFACT" -D "$PKGS" \
            || die "download failed"
    else
        say "${dim}Reusing the packages already in $PKGS${reset}"
    fi
fi

DEB=$(ls "$PKGS"/*.deb 2> /dev/null | head -n1) || true
RPM=$(ls "$PKGS"/*.rpm 2> /dev/null | head -n1) || true
[ -n "$DEB" ] && [ -n "$RPM" ] || die "no .deb/.rpm in $PKGS"
VERSION=$(basename "$DEB"); VERSION=${VERSION#moonlightweb-}; VERSION=${VERSION%-linux-x64.deb}

# ── 2. The repository ───────────────────────────────────────────────────────
# Built inside a container: apt-ftparchive, createrepo-c, rpmsign and
# appstreamcli compose are not going to be on a Windows host, and the CI runs
# them on Ubuntu anyway.
cat > "$RUN_DIR/build-repo.sh" <<'BUILD'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq apt-utils createrepo-c rpm appstream gnupg > /dev/null
appstreamcli compose --help > /dev/null 2>&1 || apt-get install -y -qq appstream-compose > /dev/null

# rpmsign execs whatever %__gpg points at, which is /usr/bin/gpg2 — a path the
# stock ubuntu:24.04 image no longer has (the GitHub runner image does, which is
# why the release workflow never trips over this).
command -v gpg2 > /dev/null 2>&1 || ln -s "$(command -v gpg)" /usr/bin/gpg2

# Throwaway key, thrown away with the container. Passphrase-less so gpg and
# rpmsign never look for a tty, exactly as the CI key is.
gpg --batch --quiet --passphrase '' \
    --quick-gen-key 'MoonlightWeb local test <test@localhost>' default default never
echo allow-loopback-pinentry >> ~/.gnupg/gpg-agent.conf
gpgconf --reload gpg-agent
FPR=$(gpg --list-secret-keys --with-colons | awk -F: '/^fpr:/ {print $10; exit}')

DEB=$(ls /pkgs/*.deb | head -n1)
RPM=$(ls /pkgs/*.rpm | head -n1)
rm -rf /work/site
mkdir -p /work/site
# http://repo is the name the installer container resolves the server under.
MW_BASEURL=http://repo bash /packaging/make-repo.sh "$DEB" "$RPM" "$VERSION" /work/site "$FPR"

# Served from the repository root, so the pipe below is the real one-liner.
cp /website/install.sh /work/site/install.sh
chmod -R a+rX /work/site
BUILD

say ""
say "${bold}Building the apt + dnf repository for $VERSION${reset}"
docker run --rm \
    -e VERSION="$VERSION" \
    -v "$(hostpath "$RUN_DIR"):/work" \
    -v "$(hostpath "$PKGS"):/pkgs:ro" \
    -v "$(hostpath "$ROOT/backend/packaging/linux"):/packaging:ro" \
    -v "$(hostpath "$ROOT/website"):/website:ro" \
    ubuntu:24.04 bash /work/build-repo.sh \
    || die "the repository build failed"

# ── 3. Serve it ─────────────────────────────────────────────────────────────
cleanup() {
    docker rm -f "$SERVER" > /dev/null 2>&1 || true
    docker network rm "$NET" > /dev/null 2>&1 || true
}
trap cleanup EXIT INT TERM
cleanup

docker network create "$NET" > /dev/null
docker run -d --name "$SERVER" --network "$NET" --network-alias repo \
    -v "$(hostpath "$RUN_DIR/site"):/usr/share/nginx/html:ro" \
    nginx:alpine > /dev/null || die "could not start the repository server"

# ── 4. Install, as a user would ─────────────────────────────────────────────
# curl is the one thing the one-liner assumes and a bare image does not have;
# fetching it is not part of what is under test. sudo is not needed — the
# container is root, and need_root returns early for root.
cat > "$RUN_DIR/try.sh" <<'TRY'
set -eu
if command -v apt-get > /dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq && apt-get install -y -qq curl ca-certificates > /dev/null
elif command -v dnf > /dev/null; then
    dnf install -y -q curl > /dev/null
fi
printf '\n\033[1m$ curl -fsSL https://moonlightweb.top/install.sh | bash\033[0m\n'
curl -fsSL http://repo/install.sh | MW_PAGES=http://repo bash || printf '\n\033[31minstall.sh exited %s\033[0m\n' "$?"
printf '\n\033[2mShell in the test container. `exit` tears everything down.\033[0m\n'
exec "$(command -v bash || command -v sh)"
TRY

# -t only when there is a terminal to attach: without one the installer skips
# its prompts, which is the right behaviour for a scripted run.
TTY=-i; [ -t 0 ] && [ -t 1 ] && TTY=-it

say ""
say "${bold}Installing in $IMAGE${reset}"
# shellcheck disable=SC2086 # TRY_RUN_ARGS is deliberately word-split
docker run --rm $TTY --network "$NET" \
    ${MW_INTERNET+-e MW_INTERNET="$MW_INTERNET"} \
    ${MW_HEADLESS+-e MW_HEADLESS="$MW_HEADLESS"} \
    -v "$(hostpath "$RUN_DIR/try.sh"):/try.sh:ro" \
    ${TRY_RUN_ARGS:-} \
    "$IMAGE" sh /try.sh || true
