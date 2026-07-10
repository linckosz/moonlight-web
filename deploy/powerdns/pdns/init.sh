#!/bin/sh
# PowerDNS zone bootstrap, then hand off to the OFFICIAL startup wrapper.
# Mounted into the stock PowerDNS image and used as its entrypoint. Idempotent.
# Runs as the image's non-root "pdns" user — DNS (:5300) and API (:8081) are
# non-privileged ports, so no extra capabilities are needed.
#
#   MW_DOMAIN          parent zone (e.g. example.top)              [required]
#   MW_PUBLIC_IP       public IPv4 of this DNS box                 [required]
#   PDNS_AUTH_API_KEY  REST API key (set via MW_PDNS_API_KEY in
#                      .env; consumed by the official wrapper)     [required]
set -eu

: "${MW_DOMAIN:?MW_DOMAIN is required (parent zone, e.g. example.top)}"
: "${MW_PUBLIC_IP:?MW_PUBLIC_IP is required (public IPv4 of this DNS server)}"
: "${PDNS_AUTH_API_KEY:?set MW_PDNS_API_KEY in .env (maps to PDNS_AUTH_API_KEY)}"

DB="/var/lib/powerdns/pdns.sqlite3"
SCHEMA="/usr/local/share/doc/pdns/schema.sqlite3.sql"
PDNSUTIL="pdnsutil --config-dir=/etc/powerdns"

# A named volume is auto-populated from the image's pre-created DB; this guard
# only matters for a fresh bind mount.
if [ ! -f "$DB" ]; then
    echo "[mw] SQLite DB missing — creating from schema"
    sqlite3 "$DB" < "$SCHEMA"
fi

# Create + populate the zone if absent.
if ! $PDNSUTIL list-all-zones 2>/dev/null | grep -qx "$MW_DOMAIN"; then
    echo "[mw] Creating zone $MW_DOMAIN -> $MW_PUBLIC_IP"
    $PDNSUTIL create-zone "$MW_DOMAIN"
    $PDNSUTIL add-record "$MW_DOMAIN" @   A  "$MW_PUBLIC_IP"
    $PDNSUTIL add-record "$MW_DOMAIN" www A  "$MW_PUBLIC_IP"
    $PDNSUTIL add-record "$MW_DOMAIN" stats A "$MW_PUBLIC_IP"
    $PDNSUTIL add-record "$MW_DOMAIN" stream A "$MW_PUBLIC_IP"
    $PDNSUTIL add-record "$MW_DOMAIN" ns1 A  "$MW_PUBLIC_IP"
    $PDNSUTIL add-record "$MW_DOMAIN" ns2 A  "$MW_PUBLIC_IP"
    $PDNSUTIL add-record "$MW_DOMAIN" api A  "$MW_PUBLIC_IP"
    $PDNSUTIL add-record "$MW_DOMAIN" @   NS "ns1.${MW_DOMAIN}."
    $PDNSUTIL add-record "$MW_DOMAIN" @   NS "ns2.${MW_DOMAIN}."
    $PDNSUTIL secure-zone "$MW_DOMAIN"
    $PDNSUTIL rectify-zone "$MW_DOMAIN"
    echo "[mw] Zone ready. DS record to submit to your registrar:"
    $PDNSUTIL export-zone-ds "$MW_DOMAIN" || true
else
    echo "[mw] Zone $MW_DOMAIN already exists — skipping init"
fi

# Ensure the presentation-site records exist even on a pre-existing zone (created
# before the website was added). Idempotent: only adds each when missing, so
# re-running on an already-deployed DNS box safely backfills www/apex/api.
ensure_a() {  # ensure_a <name>  — add an A record to MW_PUBLIC_IP if absent
    name="$1"
    fqdn="$( [ "$name" = "@" ] && echo "$MW_DOMAIN" || echo "${name}.${MW_DOMAIN}" )"
    if ! $PDNSUTIL list-zone "$MW_DOMAIN" 2>/dev/null \
         | grep -qiE "^${fqdn}\.?[[:space:]].*[[:space:]]A[[:space:]]"; then
        echo "[mw] Adding missing A record ${fqdn} -> $MW_PUBLIC_IP"
        $PDNSUTIL add-record "$MW_DOMAIN" "$name" A "$MW_PUBLIC_IP"
        NEED_RECTIFY=1
    fi
}
NEED_RECTIFY=0
ensure_a @     # apex  — presentation site
ensure_a www   # www   — presentation site
ensure_a api   # api   — PowerDNS REST API
ensure_a stats  # stats  — Umami analytics dashboard
ensure_a stream # stream — vanity alias, redirected to the apex by Caddy

# Zones created before default-soa-content was set (zz-mw.conf) carry the image
# placeholder SOA ("a.misconfigured.dns.server.invalid"); swap in a real one.
if $PDNSUTIL list-zone "$MW_DOMAIN" 2>/dev/null | grep -q 'misconfigured\.dns\.server\.invalid'; then
    echo "[mw] Replacing placeholder SOA for $MW_DOMAIN"
    $PDNSUTIL replace-rrset "$MW_DOMAIN" @ SOA 3600 \
        "ns1.${MW_DOMAIN}. hostmaster.${MW_DOMAIN}. $(date -u +%Y%m%d01) 10800 3600 604800 3600"
    NEED_RECTIFY=1
fi
[ "$NEED_RECTIFY" = "1" ] && $PDNSUTIL rectify-zone "$MW_DOMAIN" || true

# Hand off to the official wrapper: it renders the REST API config from
# PDNS_AUTH_API_KEY, then execs pdns_server.
echo "[mw] Starting PowerDNS (official wrapper)"
exec /usr/local/sbin/pdns_server-startup
