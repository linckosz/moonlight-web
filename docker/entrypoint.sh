#!/bin/sh
# =============================================================================
#  MoonlightWeb container entrypoint.
#
#  Three jobs, then it gets out of the way with exec so the server is PID 1 and
#  receives Docker's signals directly:
#
#    1. make $HOME (the mounted volume) usable,
#    2. apply the handful of settings that have to be decided before the server
#       starts — the listener ports and UPnP — from environment variables,
#    3. drop the single-instance lock left behind by a container that was
#       killed rather than stopped.
#
#  Anything after the image name is passed straight through to the binary, so
#  `docker run … moonlightweb-image --port 8080` and
#  `docker exec <c> moonlightweb --status` both behave like the packaged app.
# =============================================================================
set -eu

APP=/opt/moonlightweb/MoonlightWeb
# Qt's AppDataLocation for organization=application=MoonlightWeb, under the
# $HOME the image sets to the volume. settings.json, cert/, the access PIN and
# the logs all live here; QSettings (paired hosts + the Moonlight client
# identity) sits next door in $HOME/.config/MoonlightWeb.
DATA="$HOME/.local/share/MoonlightWeb/MoonlightWeb"
SETTINGS="$DATA/settings.json"

# ── Operator commands run against the *other* process ────────────────────────
# `--status`, `--new-pin`, `--set-admin-password` and `--enable-internet` talk
# to the instance that is already running and exit. None of the preparation
# below applies to them — and the lock cleanup would be actively wrong, since
# the lock they must not disturb belongs to a live server.
for arg in "$@"; do
    case "$arg" in
        --status|--new-pin|--set-admin-password|--enable-internet|--version|-v|--help|-h)
            exec "$APP" "$@"
            ;;
    esac
done

mkdir -p "$DATA" "$HOME/.config" "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# ── Settings that must be decided before the listeners bind ──────────────────
# Everything else is configured from the admin page or by editing settings.json
# in the volume; these three are the ones you cannot reach the admin page
# without having set correctly, so they are worth an environment variable.
#
# Re-applied on every start rather than seeded once: the server writes the port
# it actually bound back into settings.json, so a seed-if-absent rule would let
# a fallback port outlive the compose file that asked for 443.
apply_setting() {
    _key=$1 _value=$2
    [ -n "$_value" ] || return 0
    [ -f "$SETTINGS" ] || echo '{}' > "$SETTINGS"
    _tmp="$SETTINGS.tmp"
    if jq --argjson v "$_value" ".\"$_key\" = \$v" "$SETTINGS" > "$_tmp" 2>/dev/null; then
        mv "$_tmp" "$SETTINGS"
    else
        rm -f "$_tmp"
        echo "entrypoint: could not set $_key in $SETTINGS (is it valid JSON?)" >&2
    fi
}

apply_setting http_port "${MW_HTTP_PORT:-}"
apply_setting https_port "${MW_HTTPS_PORT:-}"
# UPnP port mapping is how a stream reaches a browser outside the LAN without
# hand-written router rules, and it is driven by SSDP multicast — which reaches
# a router from the host network and from nowhere else. Left on by default (the
# image is meant to be run with --network host); MW_UPNP=0 stops a bridged
# container from asking a router it cannot see to map ports for it. The
# capability probe InternetAccessManager runs at startup is separate and always
# runs, so "No UPnP devices found" in the log is expected either way.
case "${MW_UPNP:-}" in
    0|no|false) apply_setting upnp_enabled false ;;
    1|yes|true) apply_setting upnp_enabled true ;;
esac

# ── Stale single-instance lock ───────────────────────────────────────────────
# main.cpp guards against a second server with a QLockFile whose staleness test
# is "is that PID still alive". In a container the server is PID 1 — so after a
# `docker kill`, an OOM kill or a host reboot, the lock left on the volume names
# PID 1 and the *replacement* container is also PID 1. The new server would
# conclude that it is the second instance and exit 0, and the container would
# quietly refuse to come up until someone deleted the file by hand.
#
# Safe to delete unconditionally at this point: the entrypoint runs once per
# container start, before any server exists, and the operator commands above
# already left via exec.
rm -f "$DATA/moonlightweb.lock" "$DATA/moonlightweb-tray.lock"

# ── What the operator still has to do ────────────────────────────────────────
# A container cannot finish its own setup. Two of the three steps below need a
# human (a password, and a PIN typed into a browser), and the third — putting
# the container on the LAN — was decided by `docker run` before this process
# existed: nothing the app can do about its own network namespace afterwards.
#
# So it says so, once, at the top of `docker logs`, rather than leaving someone
# to discover it from a page that will not load. Printed before exec, so it is
# the first thing in the log and never buried under the server's own output.
#
# Network mode is a heuristic, and phrased as one. A bridged container sees
# exactly one interface of its own; sharing the host's namespace means seeing
# the host's — the docker/podman bridge among them, which is the tell.
lan_mode() {
    for _if in /sys/class/net/*; do
        case "${_if##*/}" in
            lo | eth0) ;;
            docker0 | br-* | cni-* | podman*) echo host; return ;;
            *) echo host; return ;;
        esac
    done
    echo bridge
}

# Only the persisted half is checkable here: the access PIN is single-use and
# lives in memory, so it is always worth (re)issuing. `admin_password` is a
# settings key, so a second start does not nag about something already done.
_admin_set=no
[ -f "$SETTINGS" ] && jq -e 'has("admin_password")' "$SETTINGS" > /dev/null 2>&1 && _admin_set=yes

# The container id, unless --hostname was given. Not as pretty as the name the
# operator chose — which the container has no way to know — but it is what
# `docker exec` accepts, so every line below is copy-pasteable as-is.
# shellcheck disable=SC3028  # HOSTNAME is not a POSIX shell variable, but Docker
# puts it in the environment of every container, so this is an env var like any
# other here. /etc/hostname covers a runtime that does not.
_name="${HOSTNAME:-}"
[ -n "$_name" ] || _name="$(cat /etc/hostname 2>/dev/null)"
[ -n "$_name" ] || _name="<container>"

# Drawn through one padding helper rather than by hand: every line is one
# `%-69s`, so the right-hand border cannot drift when a container id is a
# different length or a sentence gets reworded.
#
# KEEP THE TEXT PASSED TO row() PURE ASCII. dash's printf pads to a byte count,
# not a column count, so one em dash (3 bytes, 1 column) pulls that line's
# border two columns left. The box characters themselves are safe: they live in
# the format string, at a fixed position, and are never padded.
# 71 = the 69-wide text field plus the space either side of it. Built rather
# than typed: a hand-counted run of box characters is the one thing here that
# would silently go crooked, and nobody re-counts it after an edit.
_bar=''
_i=0
while [ "$_i" -lt 71 ]; do
    _bar="$_bar─"
    _i=$((_i + 1))
done
row() { printf '  │ %-69s │\n' "$1"; }
echo
printf '  ┌%s┐\n' "$_bar"
if [ "$(lan_mode)" = host ]; then
    row "Network   host: this container is ON your LAN."
    row "          Sunshine machines are discovered automatically (mDNS)."
else
    row "Network   bridge: this container is NOT on your LAN."
    row "          Other machines are reachable by IP but will NOT be"
    row "          discovered: mDNS is multicast and stops at the bridge,"
    row "          and WebRTC media may fall back to TCP (more latency)."
    row "          Fix: recreate with --network host (Linux hosts), or use"
    row "          a macvlan network. See docker/README.md."
fi
printf '  ├%s┤\n' "$_bar"
row "Still to do, on the Docker host:"
row ""
[ "$_admin_set" = yes ] || row "  docker exec -it $_name moonlightweb --set-admin-password"
row "  docker exec $_name moonlightweb --new-pin"
row "  docker exec $_name moonlightweb --status"
row ""
row "--status prints the URL to open. The PIN is single-use: one per"
row "device. Without it the page stops at an authentication wall: a"
row "container has no trusted local browser to let in."
printf '  └%s┘\n' "$_bar"
echo

exec "$APP" "$@"
