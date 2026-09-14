#!/usr/bin/env bash
# deploy-packages.sh — ship the APT/DNF repository tree built by release.yml's
# linux-repo job to packages.{MW_DOMAIN}, served by this box's Caddy from a
# fixed host path (docker-compose.yml: /root/mw-packages:/srv/packages:ro).
#
#   ./deploy-packages.sh v0.3.1          show what would change — writes nothing
#   ./deploy-packages.sh v0.3.1 --apply  download it and put it in place
#
# Why this exists instead of another GitHub Pages deployment: Pages is one
# site per repository, and pages.yml already owns it for the bootstrap
# (bootstrap-watch.yml's reference copy). release.yml's package-repo job used
# to deploy there too and raced it on every tag push — usually winning, since
# building three package formats takes longer than publishing a few KB of
# bootstrap, so it finished, and deployed, later. That clobbered the entry
# page in production for hours before anyone noticed (2026-09-14). Now
# release.yml only builds and attaches the tree to the GitHub Release; this
# script is the other half, run by hand, same as deploy-prod.sh.
#
# Why not automatic from CI: nothing in this stack pushes to production on its
# own (see deploy-prod.sh's own reasoning) — a release goes live when a human
# ships it, not the moment a workflow happens to finish.
#
# The asset is a public GitHub Release download, not a workflow-run artifact:
# the latter needs a GitHub login to fetch, and this box authenticates to
# nothing.
set -Eeuo pipefail
trap 'echo "deploy-packages.sh: stopped at line $LINENO — \"$BASH_COMMAND\" failed (exit $?)" >&2' ERR

REPO=linckosz/moonlight-web
DEST=/root/mw-packages

main() {
    local tag="${1:-}" mode="${2:-}"
    if [[ -z "$tag" || "$tag" == "-h" || "$tag" == "--help" ]]; then
        sed -n '2,7p' "$0"
        exit 2
    fi
    local version="${tag#v}"
    local asset="moonlightweb-repo-${version}.tar.gz"
    local url="https://github.com/${REPO}/releases/download/${tag}/${asset}"

    echo "tag:   $tag"
    echo "asset: $url"

    local tmp
    tmp=$(mktemp -d)
    trap 'rm -rf "$tmp"' EXIT

    echo "--- downloading"
    curl -fSL --retry 3 -o "$tmp/repo.tar.gz" "$url"

    echo "--- extracting"
    mkdir -p "$tmp/site"
    tar xzf "$tmp/repo.tar.gz" -C "$tmp/site"
    echo "$(find "$tmp/site" -type f | wc -l) files, $(du -sh "$tmp/site" | cut -f1)"

    if [[ "$mode" != "--apply" ]]; then
        echo
        echo "dry run — would replace $DEST with the tree above. Re-run with --apply."
        exit 0
    fi

    echo "--- installing to $DEST"
    mkdir -p "$DEST"
    # --delete: an old release's files must not linger once superseded — the
    # repository metadata (Packages, repodata/) says what's current, and a
    # stale .deb nothing references any more would still satisfy a client that
    # somehow still asks for it by name.
    rsync -a --delete "$tmp/site/" "$DEST/"

    local domain
    domain=$(grep '^MW_DOMAIN=' .env 2>/dev/null | cut -d= -f2 | tr -d '[:space:]')
    echo "done — https://packages.${domain:-<MW_DOMAIN>}/ now serves $tag"
}

main "$@"
