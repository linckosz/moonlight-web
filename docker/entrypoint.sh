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

exec "$APP" "$@"
