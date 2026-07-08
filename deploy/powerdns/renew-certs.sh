#!/usr/bin/env bash
# MoonlightWeb — (re)obtain the Caddy TLS certificates once DNS is ready.
#
# Caddy issues Let's Encrypt certs automatically at startup, but the first
# attempt FAILS if the registrar delegation (ns1/ns2 glue) or the cloud
# firewall's port 53 has not propagated yet — the CA cannot resolve the domain,
# so no cert is minted and browsers get ERR_SSL_PROTOCOL_ERROR / TLS alert 80.
#
# Run this AFTER `dig +short api.{MW_DOMAIN} @8.8.8.8` returns your VM's IP: it
# confirms public resolution, then restarts Caddy so it retries issuance, and
# tails the log until a certificate is obtained (or an error is shown).
#
#   ./renew-certs.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

c_green=$'\033[1;32m'; c_yellow=$'\033[1;33m'; c_red=$'\033[1;31m'; c_reset=$'\033[0m'
ok()   { printf '%s  ok%s %s\n'  "$c_green"  "$c_reset" "$*"; }
warn() { printf '%s warn%s %s\n' "$c_yellow" "$c_reset" "$*"; }
die()  { printf '%s fail%s %s\n' "$c_red"    "$c_reset" "$*" >&2; exit 1; }

# Compose command (plugin v2 vs legacy binary).
if docker compose version >/dev/null 2>&1; then COMPOSE="docker compose"
elif command -v docker-compose >/dev/null 2>&1; then COMPOSE="docker-compose"
else die "docker compose not found"; fi

[ -f .env ] || die "no .env in $HERE — run install.sh first"
# shellcheck disable=SC1091
set -a; . ./.env; set +a
[ -n "${MW_DOMAIN:-}" ] || die "MW_DOMAIN is empty in .env"

# ── Confirm the world can resolve the zone before touching Caddy ──────────────
# Querying a PUBLIC resolver (dns.google over HTTPS) is what Let's Encrypt does;
# it also works from the DNS box itself, which cannot test its own inbound :53.
echo "Checking public DNS resolution for ${MW_DOMAIN} ..."
all_ok=1
for host in "api.${MW_DOMAIN}" "stats.${MW_DOMAIN}" "${MW_DOMAIN}"; do
    ans="$(curl -fsS -m 10 "https://dns.google/resolve?name=${host}&type=A" 2>/dev/null || true)"
    case "$ans" in
        *'"Status":0'*)
            if printf '%s' "$ans" | grep -q "\"data\":\"${MW_PUBLIC_IP:-}\""; then
                ok "${host} -> ${MW_PUBLIC_IP}"
            else
                warn "${host} resolves publicly, but NOT to ${MW_PUBLIC_IP:-<MW_PUBLIC_IP>}:"
                printf '%s' "$ans" | grep -o '"data":"[^"]*"' | sed 's/^/         /'
                warn "a stale delegation or an old VM may still answer for this zone."
                all_ok=0
            fi
            ;;
        *'"Status":2'*)
            warn "${host}: SERVFAIL — delegation points here but resolvers cannot reach"
            warn "  the VM on :53. Open 53/udp AND 53/tcp on your CLOUD firewall / NSG."
            all_ok=0 ;;
        *'"Status":3'*)
            warn "${host}: NXDOMAIN — registrar glue/delegation (ns1/ns2 -> ${MW_PUBLIC_IP:-IP})"
            warn "  is missing or not propagated yet (minutes to hours)."
            all_ok=0 ;;
        *) warn "${host}: could not check public resolution (network?)"; all_ok=0 ;;
    esac
done

if [ "$all_ok" -ne 1 ]; then
    echo
    warn "DNS is not fully ready — fix the above first (see NEXT-STEPS.txt), then"
    warn "re-run this script. Restarting Caddy now would just burn Let's Encrypt"
    warn "attempts (limit: ~5 failures/hour per hostname)."
    exit 1
fi

# ── Retrigger issuance ────────────────────────────────────────────────────────
echo
ok "public DNS is ready — restarting Caddy to obtain certificates"
$COMPOSE restart caddy

echo
echo "Watching Caddy logs (Ctrl-C to stop once you see success)..."
echo "  success looks like:  certificate obtained successfully"
echo "  rate-limited?        too many failed authorizations  → wait ~1h, re-run"
echo
$COMPOSE logs -f --since 10s caddy
