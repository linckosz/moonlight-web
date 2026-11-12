#!/usr/bin/env bash
# errors.sh — fetch the instances' error reports from the PRODUCTION box, and
# delete them there once they have been read. Run from your own machine (Git
# Bash, Linux, macOS), not on the box.
#
#   MW_ERRORS_SSH=<ssh target> ./errors.sh status
#   MW_ERRORS_SSH=<ssh target> ./errors.sh pull  [local dir]
#   MW_ERRORS_SSH=<ssh target> ./errors.sh purge YYYY-MM-DD [local dir]
#
#   status   how many day files, reports and bytes the box holds, oldest first
#   pull     copy every day file that is new or has grown since the last pull
#            into the local dir (default ./mw-errors), each checked by sha256
#   purge    delete on the box the day files up to that date, and only those
#            whose local copy has the very same sha256; never today's (UTC),
#            still being written, nor one that changed since it was pulled
#
# One report is one JSON line in /data/error-reports/YYYY-MM-DD.jsonl of the
# mw-proxy volume (mw-proxy/errorlog.go). The box also deletes on its own the
# days past 90, and the oldest ones when the directory nears 1 GiB: pull often.
#
# MW_ERRORS_SSH is whatever `ssh` takes (a Host from ~/.ssh/config, user@host).
# MW_ERRORS_VOLUME overrides the volume name (default powerdns_mwproxy_data).
set -euo pipefail

usage() {
    sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'
    exit 2
}

target="${MW_ERRORS_SSH:-}"
volume="${MW_ERRORS_VOLUME:-powerdns_mwproxy_data}"
[[ -n "$target" ]] || { echo "errors.sh: set MW_ERRORS_SSH to the box's ssh target" >&2; exit 2; }
[[ "$volume" =~ ^[A-Za-z0-9_.-]+$ ]] || { echo "errors.sh: bad volume name" >&2; exit 2; }
cmd="${1:-}"
[[ -n "$cmd" ]] || usage
shift

# One command per ssh call, never a script on stdin: `ssh box bash -s` has
# swallowed the rest of a script before (deploy-prod.sh).
remote() { ssh -o BatchMode=yes "$target" "$1" </dev/null; }

dir_cmd="d=\"\$(docker volume inspect --format '{{.Mountpoint}}' $volume)/error-reports\""

# "name sha256 size" for every day file on the box, oldest first.
remote_list() {
    remote "$dir_cmd; [ -d \"\$d\" ] || exit 0; cd \"\$d\" && for f in [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9].jsonl; do [ -f \"\$f\" ] || continue; printf '%s %s %s\n' \"\$f\" \"\$(sha256sum \"\$f\" | cut -d' ' -f1)\" \"\$(stat -c %s \"\$f\")\"; done"
}

local_sha() { [[ -f "$1" ]] && sha256sum "$1" | cut -d' ' -f1 || true; }

valid_name() { [[ "$1" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}\.jsonl$ ]]; }

case "$cmd" in
status)
    remote "$dir_cmd; [ -d \"\$d\" ] || { echo 'no report yet'; exit 0; }; cd \"\$d\" && n=0; for f in [0-9]*.jsonl; do [ -f \"\$f\" ] || continue; n=\$((n+1)); printf '%s  %6s reports  %10s bytes\n' \"\$f\" \"\$(wc -l < \"\$f\")\" \"\$(stat -c %s \"\$f\")\"; done; echo \"\$n day files, \$(du -sb . | cut -f1) bytes in all\""
    ;;
pull)
    out="${1:-./mw-errors}"
    mkdir -p "$out"
    pulled=0
    while read -r name sha _size; do
        valid_name "$name" || { echo "skipped an unexpected name: $name" >&2; continue; }
        [[ "$(local_sha "$out/$name")" == "$sha" ]] && continue
        remote "$dir_cmd; cat \"\$d/$name\"" >"$out/$name.part"
        if [[ "$(local_sha "$out/$name.part")" != "$sha" ]]; then
            # Today's file grew while it was read: the next pull gets the rest.
            echo "$name changed while it was copied; kept the copy, pull again later" >&2
        fi
        mv -f "$out/$name.part" "$out/$name"
        echo "pulled $name"
        pulled=$((pulled + 1))
    done < <(remote_list)
    echo "$pulled file(s) pulled into $out"
    ;;
purge)
    until="${1:-}"
    out="${2:-./mw-errors}"
    [[ "$until" =~ ^[0-9]{4}-[0-9]{2}-[0-9]{2}$ ]] || { echo "errors.sh: purge needs a date, YYYY-MM-DD" >&2; exit 2; }
    # Today's file (UTC, the box's day) is still being written: a report kept
    # between the sha256 check and the rm would be lost. It goes tomorrow.
    today="$(date -u +%Y-%m-%d)"
    deleted=0
    while read -r name sha _size; do
        valid_name "$name" || continue
        [[ "${name%.jsonl}" > "$until" ]] && continue
        if [[ "${name%.jsonl}" == "$today" ]]; then
            echo "kept $name on the box: it is today's, still being written" >&2
            continue
        fi
        if [[ "$(local_sha "$out/$name")" != "$sha" ]]; then
            echo "kept $name on the box: no identical copy in $out (pull it first)" >&2
            continue
        fi
        # The name was checked against the day pattern above: nothing else can
        # reach this rm.
        remote "$dir_cmd; rm -f -- \"\$d/$name\""
        echo "deleted $name on the box"
        deleted=$((deleted + 1))
    done < <(remote_list)
    echo "$deleted file(s) deleted on the box"
    ;;
*)
    usage
    ;;
esac
