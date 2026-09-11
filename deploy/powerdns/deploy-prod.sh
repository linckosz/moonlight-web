#!/usr/bin/env bash
# deploy-prod.sh — move the PRODUCTION checkout to a release tag, rebuilding
# only the containers that tag actually changed.
#
#   ./deploy-prod.sh v0.3.1          show what would change — writes nothing
#   ./deploy-prod.sh v0.3.1 --apply  check it out and rebuild what it touched
#
# Why this exists instead of `git pull && docker compose up -d --build`:
#
#   - Production follows a TAG, never main. The bootstrap directory is
#     bind-mounted into Caddy, so on this box `git pull` alone is a deploy —
#     every browser gets the new entry page on its next request — and pulling
#     main would put unreleased code in front of every installed instance.
#     Development happens on the staging copy (docker-compose.dev.yml) from a
#     separate clone; this checkout only ever moves from one tag to the next.
#
#   - `up -d --build caddy` also restarts mw-rendezvous, because caddy
#     depends_on it. That drops every held host line — every installed machine
#     goes unreachable for a few seconds and reconnects — for a change that
#     never touched the rendezvous. Here each service is rebuilt with --no-deps,
#     and only if its own directory differs between the two commits.
#
#   - Nothing is asked interactively. These lines get pasted into a terminal,
#     and a prompt in the middle of a paste eats the next pasted line as its
#     answer. The dry run IS the confirmation step.
#
# The whole script is a function called on the last line, so bash has parsed
# all of it before `git checkout` replaces this very file on disk.
set -euo pipefail

main() {
    local ref="${1:-}" mode="${2:-}"
    if [[ -z "$ref" || "$ref" == "-h" || "$ref" == "--help" ]]; then
        sed -n '2,7p' "$0"
        exit 2
    fi
    if [[ -n "$mode" && "$mode" != "--apply" ]]; then
        echo "unknown option: $mode (only --apply is accepted)" >&2
        exit 2
    fi
    local apply=0
    [[ "$mode" == "--apply" ]] && apply=1

    cd "$(dirname "$0")"
    # Run from outside any checkout, it deploys the production one. That is the
    # one-time move of a checkout older than this script (README §One-time
    # setup): the script is taken from the target tag with `git show` into
    # /tmp, because the running checkout does not have it yet.
    if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
        cd "$HOME/moonlight-web/deploy/powerdns"
    fi

    # Guard: this is for the production checkout and nothing else. The dev
    # clone has an identical copy of this file, and running it there would
    # move the dev tree onto a tag — harmless, but not what anybody meant.
    local top
    top="$(git rev-parse --show-toplevel)"
    if [[ "$top" != "$HOME/moonlight-web" ]]; then
        echo "refusing: $top is not the production checkout (\$HOME/moonlight-web)" >&2
        exit 1
    fi
    if [[ ! -f .env ]]; then
        echo "refusing: no .env here — is this really the deployed stack?" >&2
        exit 1
    fi

    # A dirty tree means somebody edited production by hand. Deploying over it
    # would either lose that edit or carry it forward unrecorded; both are
    # worse than stopping.
    if ! git diff --quiet || ! git diff --cached --quiet; then
        echo "refusing: the working tree has local changes — commit or stash them first:" >&2
        git status --short >&2
        exit 1
    fi

    echo "fetching tags…"
    git fetch --quiet --tags origin

    local target current
    if ! target="$(git rev-parse --verify --quiet "${ref}^{commit}")"; then
        echo "refusing: '$ref' is not a commit or tag this repository knows" >&2
        exit 1
    fi
    current="$(git rev-parse HEAD)"

    echo
    echo "current : $(git describe --tags --always "$current")  ($current)"
    echo "target  : $ref  ($target)"

    if [[ "$current" == "$target" ]]; then
        echo "already on $ref — nothing to do"
        exit 0
    fi
    if git merge-base --is-ancestor "$target" "$current"; then
        echo "note    : this is a ROLLBACK — $ref is older than what is running"
    fi

    # What changed where it matters. Only these directories reach the
    # containers: deploy/powerdns is built or mounted, bootstrap and website are
    # mounted. The pathspecs are anchored at the top (":/"): this runs from
    # deploy/powerdns, where plain ones would match nothing and the list would
    # read "none" over every change — which the mw-dns rehearsal caught.
    local -a reach=(':/deploy/powerdns' ':/bootstrap' ':/website')
    echo
    echo "changes reaching this box:"
    git --no-pager diff --stat "$current" "$target" -- "${reach[@]}" | sed 's/^/  /'
    [[ -n "$(git diff --name-only "$current" "$target" -- "${reach[@]}")" ]] \
        || echo "  (none — the tag only changes the application)"

    # Which containers that means. A directory that is COPIED into an image
    # needs a rebuild; a bind-mounted one is served on the next request.
    local -a rebuild=()
    local -a notes=()
    local changed
    changed="$(git diff --name-only "$current" "$target")"
    grep -q '^deploy/powerdns/caddy/'         <<<"$changed" && rebuild+=(caddy)
    grep -q '^deploy/powerdns/mw-rendezvous/' <<<"$changed" && rebuild+=(mw-rendezvous)
    grep -q '^deploy/powerdns/mw-proxy/'      <<<"$changed" && rebuild+=(mw-proxy)
    grep -q '^bootstrap/' <<<"$changed" \
        && notes+=("bootstrap/ is bind-mounted: served on the next request, nothing to rebuild")
    grep -q '^website/' <<<"$changed" \
        && notes+=("website/ is bind-mounted: served on the next request, nothing to rebuild")
    grep -q '^deploy/powerdns/pdns/' <<<"$changed" \
        && notes+=("pdns/ changed: bind-mounted, takes effect on 'docker compose restart pdns' (DNS blips for a second — pick the moment)")
    grep -q '^deploy/powerdns/dnsdist/' <<<"$changed" \
        && notes+=("dnsdist/ changed: bind-mounted, takes effect on 'docker compose restart dnsdist'")
    grep -qE '^deploy/powerdns/(docker-compose\.yml|\.env\.sample)$' <<<"$changed" \
        && notes+=("docker-compose.yml or .env.sample changed: read the diff, then 'docker compose up -d --no-deps <service>' for what it touches — this script will not guess")

    echo
    if ((${#rebuild[@]})); then
        echo "containers to rebuild (each with --no-deps, so nothing else restarts):"
        printf '  %s\n' "${rebuild[@]}"
    else
        echo "containers to rebuild: none"
    fi
    if ((${#notes[@]})); then
        echo "also:"
        printf '  - %s\n' "${notes[@]}"
    fi

    if ((!apply)); then
        echo
        echo "dry run — nothing was changed. Re-run with --apply to do the above."
        exit 0
    fi

    echo
    echo "checking out $ref…"
    git checkout --quiet --detach "$target"
    echo "now on $(git describe --tags --always HEAD)"

    local svc
    for svc in "${rebuild[@]}"; do
        echo
        echo "rebuilding $svc…"
        docker compose up -d --no-deps --build "$svc"
    done

    echo
    docker compose ps
    echo
    echo "done: production is on $ref"
}

main "$@"
exit $?
