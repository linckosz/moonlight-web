#!/bin/sh
# Render the Caddyfile from env, then run Caddy. User cert files take priority
# over automatic Let's Encrypt.
#
#   MW_DOMAIN     parent domain (API served at api.{MW_DOMAIN})   [required]
#   MW_TLS_CERT   path to a user TLS cert (PEM, fullchain)        [optional]
#   MW_TLS_KEY    path to the matching private key (PEM)          [optional]
#   MW_TLS_EMAIL  ACME account email for Let's Encrypt            [optional]
set -eu

: "${MW_DOMAIN:?MW_DOMAIN is required (parent domain, e.g. example.top)}"

# TLS for the API host: user cert wins, else ACME (email if provided).
TLS_LINE=""
# TLS for the apex/www site: never the (api-only) user cert — just ACME so the
# presentation domain always obtains its own valid certificate.
SITE_TLS_LINE=""
if [ -n "${MW_TLS_CERT:-}" ] && [ -n "${MW_TLS_KEY:-}" ]; then
    echo "[mw-caddy] API TLS: using user-supplied cert ($MW_TLS_CERT)"
    TLS_LINE="tls ${MW_TLS_CERT} ${MW_TLS_KEY}"
fi
if [ -n "${MW_TLS_EMAIL:-}" ]; then
    echo "[mw-caddy] TLS: automatic Let's Encrypt (account: $MW_TLS_EMAIL)"
    [ -z "$TLS_LINE" ] && TLS_LINE="tls ${MW_TLS_EMAIL}"
    SITE_TLS_LINE="tls ${MW_TLS_EMAIL}"
else
    echo "[mw-caddy] TLS: automatic Let's Encrypt (no account email)"
fi

sed -e "s|@MW_DOMAIN@|${MW_DOMAIN}|g" \
    -e "s|@TLS_LINE@|${TLS_LINE}|g" \
    -e "s|@SITE_TLS_LINE@|${SITE_TLS_LINE}|g" \
    /etc/caddy/Caddyfile.tmpl > /etc/caddy/Caddyfile

echo "[mw-caddy] Starting Caddy"
exec caddy run --config /etc/caddy/Caddyfile --adapter caddyfile
