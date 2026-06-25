#!/bin/sh
# PowerDNS init + run. Idempotent: safe to restart the container.
#
# Reproduces the manual VM setup driven by two env vars:
#   MW_DOMAIN     parent zone (e.g. example.top)
#   MW_PUBLIC_IP  public IPv4 of THIS DNS box (ns1/ns2 + apex + api glue)
#
# On first boot it creates the SQLite schema, the zone, the NS/A/SOA records
# and DNSSEC-secures the zone. Per-instance subdomains "{uniqueId}.{MW_DOMAIN}"
# are NOT created here — the Moonlight-Web backend manages them at runtime
# through the PowerDNS REST API.
set -eu

: "${MW_DOMAIN:?MW_DOMAIN is required (parent zone, e.g. example.top)}"
: "${MW_PUBLIC_IP:?MW_PUBLIC_IP is required (public IPv4 of this DNS server)}"
: "${MW_PDNS_API_KEY:?MW_PDNS_API_KEY is required (REST API X-API-Key)}"

DB="/var/lib/powerdns/pdns.sqlite3"
CONF="/etc/pdns/pdns.conf"

# Locate the SQLite schema shipped by pdns-backend-sqlite3 (path varies).
SCHEMA="$(find /usr/share -name 'schema.sqlite3.sql' 2>/dev/null | head -n1)"

# ── Render pdns.conf from the template, substituting the API key ─────────────
sed "s|@MW_PDNS_API_KEY@|${MW_PDNS_API_KEY}|g" /etc/pdns/pdns.conf.tmpl > "$CONF"

# ── Initialize the database on first boot ────────────────────────────────────
if [ ! -f "$DB" ]; then
    echo "[entrypoint] Creating SQLite schema at $DB"
    [ -n "$SCHEMA" ] || { echo "[entrypoint] FATAL: SQLite schema file not found"; exit 1; }
    sqlite3 "$DB" < "$SCHEMA"
fi

# ── Create + populate the zone if absent (idempotent) ────────────────────────
if ! pdnsutil --config-dir=/etc/pdns list-all-zones 2>/dev/null | grep -qx "$MW_DOMAIN"; then
    echo "[entrypoint] Creating zone $MW_DOMAIN -> $MW_PUBLIC_IP"
    pdnsutil --config-dir=/etc/pdns create-zone "$MW_DOMAIN"

    # Apex + nameserver glue + API host, all pointing at this box.
    pdnsutil --config-dir=/etc/pdns add-record "$MW_DOMAIN" @   A  "$MW_PUBLIC_IP"
    pdnsutil --config-dir=/etc/pdns add-record "$MW_DOMAIN" ns1 A  "$MW_PUBLIC_IP"
    pdnsutil --config-dir=/etc/pdns add-record "$MW_DOMAIN" ns2 A  "$MW_PUBLIC_IP"
    pdnsutil --config-dir=/etc/pdns add-record "$MW_DOMAIN" api A  "$MW_PUBLIC_IP"
    pdnsutil --config-dir=/etc/pdns add-record "$MW_DOMAIN" @   NS "ns1.${MW_DOMAIN}."
    pdnsutil --config-dir=/etc/pdns add-record "$MW_DOMAIN" @   NS "ns2.${MW_DOMAIN}."

    # DNSSEC-secure the zone (matches secure-zone on the VM).
    pdnsutil --config-dir=/etc/pdns secure-zone "$MW_DOMAIN"
    pdnsutil --config-dir=/etc/pdns rectify-zone "$MW_DOMAIN"

    echo "[entrypoint] Zone ready. DS record to hand to your registrar:"
    pdnsutil --config-dir=/etc/pdns export-zone-ds "$MW_DOMAIN" || true
else
    echo "[entrypoint] Zone $MW_DOMAIN already exists — skipping init"
fi

echo "[entrypoint] Starting PowerDNS (foreground)"
exec pdns_server --config-dir=/etc/pdns --daemon=no --guardian=no
