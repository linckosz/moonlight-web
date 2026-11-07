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
#   - Restarting Caddy closes every WebSocket of stream.: the host lines and
#     the browsers', one by one. When a browser's goes before its host's, the
#     rendezvous tells that host "the browser left", and the host takes down a
#     tunnel a stream may still be using — a stream on the host's own LAN too.
#     So the rendezvous is stopped before Caddy restarts and started after: a
#     SIGTERM ends it without those goodbyes, and hosts reconnect by themselves
#     within seconds. Images are built first, while everything still serves,
#     so that window is a container restart, not a compile.
#
#   - Nothing is asked interactively. These lines get pasted into a terminal,
#     and a prompt in the middle of a paste eats the next pasted line as its
#     answer. The dry run IS the confirmation step.
#
# The whole script is a function called on the last line, so bash has parsed
# all of it before `git checkout` replaces this very file on disk.
set -Eeuo pipefail
# A failed command ends the script (set -e); say which one. -E carries the trap
# into main(). Without it, a git or docker failure that prints nothing — a
# --quiet fetch refusing a tag did exactly that — reads like a hang.
trap 'echo "deploy-prod.sh: stopped at line $LINENO — \"$BASH_COMMAND\" failed (exit $?)" >&2' ERR

# Who is on the line. The rendezvous counts the host lines it holds and the
# browser sessions on them (/healthz "online" and "sessions"). A rendezvous
# older than "sessions" leaves the browsers to Caddy, which counts the
# requests open to the rendezvous (admin API, num_requests) — a WebSocket is
# one long request, so that is host lines plus browsers. Best effort: a stack
# that does not answer prints "unknown" and stops nothing.
line_census() {
    local health online browsers open
    health="$(docker exec moonlight-mw-rendezvous wget -qO- http://127.0.0.1:8090/healthz 2>/dev/null)" || health=""
    online="$(python3 -c 'import json, sys; print(json.load(sys.stdin)["online"])' <<<"$health" 2>/dev/null)" || online=""
    browsers="$(python3 -c 'import json, sys; print(json.load(sys.stdin)["sessions"])' <<<"$health" 2>/dev/null)" || browsers=""
    if [[ -n "$online" && -z "$browsers" ]]; then
        open="$(docker exec moonlight-caddy wget -qO- http://127.0.0.1:2019/reverse_proxy/upstreams 2>/dev/null \
            | python3 -c 'import json, sys; print(sum(u.get("num_requests", 0) for u in json.load(sys.stdin) if u["address"].startswith("mw-rendezvous:")))' 2>/dev/null)" || open=""
        [[ -n "$open" ]] && browsers=$((open > online ? open - online : 0))
    fi
    if [[ -z "$online" || -z "$browsers" ]]; then
        echo "  host lines / browsers: unknown (the rendezvous or Caddy did not answer)"
        return 0
    fi
    echo "  host lines online : $online"
    echo "  browsers connected: $browsers"
}

main() {
    # Nothing here reads stdin, and nothing may. backup-prod.sh's `docker
    # compose exec` keeps it open (-T only drops the TTY): fed a script through
    # `ssh box bash -s < script`, it swallowed the rest of that script after
    # the checkout, and in a terminal it would take the next pasted line.
    exec </dev/null
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

    # --force: origin is the only authority on what a tag names. A tag moved
    # there after this box last fetched it is otherwise refused ("would clobber
    # existing tag"), and --quiet hides that refusal: the fetch fails without a
    # word and set -e ends the script right after "fetching tags…" — which is
    # exactly what the first run on the real box did.
    echo "fetching tags…"
    git fetch --quiet --force --tags origin

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
    grep -q '^website/' <<<"$changed" \
        && notes+=("website/ changed: once it is served, resubmit sitemap.xml in Search Console and request indexing of the new or changed pages")
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

    local restarts_caddy=0
    [[ " ${rebuild[*]} " == *" caddy "* ]] && restarts_caddy=1
    if ((restarts_caddy)); then
        echo
        echo "Caddy restarts: the rendezvous (and staging's, if it runs) is stopped"
        echo "just before and started just after. Streams carry on; a browser page"
        echo "loaded before srv-2026-10-14-b1 still loses its tunnel — on the line now:"
        line_census
    fi

    if ((!apply)); then
        echo
        echo "dry run — nothing was changed. Re-run with --apply to do the above."
        exit 0
    fi

    # Cold backup before anything moves (backup-prod.sh: dated ZIP, three
    # kept). No backup, no deploy. A checkout older than the backup script
    # takes it from the target tag, so the first deploy that ships it is
    # covered too.
    echo
    if [[ -f backup-prod.sh ]]; then
        bash ./backup-prod.sh
    elif git cat-file -e "$target:deploy/powerdns/backup-prod.sh" 2>/dev/null; then
        git show "$target:deploy/powerdns/backup-prod.sh" > /tmp/mw-backup-prod.sh
        bash /tmp/mw-backup-prod.sh
    else
        echo "refusing: neither this checkout nor $ref has backup-prod.sh — no backup, no deploy" >&2
        exit 1
    fi

    echo
    echo "checking out $ref…"
    git checkout --quiet --detach "$target"
    echo "now on $(git describe --tags --always HEAD)"

    if ((${#rebuild[@]})); then
        echo
        echo "building ${rebuild[*]} while everything still serves…"
        docker compose build "${rebuild[@]}"
    fi

    local held_rdv=0 held_dev=0
    if ((restarts_caddy)); then
        echo
        echo "before Caddy restarts:"
        line_census
        echo "stopping the rendezvous first, so that it tells no host its browser left…"
        docker compose stop mw-rendezvous
        held_rdv=1
        # stream.dev's lines run through this Caddy too.
        if [[ "$(docker inspect -f '{{.State.Running}}' mw-dev-rendezvous 2>/dev/null)" == true ]]; then
            docker stop mw-dev-rendezvous >/dev/null
            held_dev=1
        fi
    fi

    # Caddy comes before mw-rendezvous in the list. A failure does not end the
    # script here: a stopped rendezvous must be started again whatever happens.
    local svc failed=""
    for svc in "${rebuild[@]}"; do
        echo
        echo "restarting $svc on its new image…"
        docker compose up -d --no-deps "$svc" || { failed="$svc"; break; }
    done

    if ((held_rdv)); then
        echo
        echo "starting the rendezvous again…"
        docker compose start mw-rendezvous
        ((held_dev)) && docker start mw-dev-rendezvous >/dev/null
        # Hosts retry their line within seconds; give them that, then count.
        sleep 20
        echo "after:"
        line_census
    fi
    if [[ -n "$failed" ]]; then
        echo "deploy-prod.sh: '$failed' did not come up on its new image — see above" >&2
        exit 1
    fi

    echo
    docker compose ps
    echo
    echo "done: production is on $ref"
}

main "$@"
exit $?
