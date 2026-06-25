# PowerDNS authoritative server on Alpine (SQLite backend + DNSSEC).
# Mirrors the manual VM install: pdns-server + pdns-backend-sqlite3 + sqlite3.
FROM alpine:3.20

RUN apk add --no-cache \
        pdns \
        pdns-backend-sqlite3 \
        sqlite \
        tini \
    && mkdir -p /var/lib/powerdns /etc/pdns/conf.d

# Config template (API key substituted at boot) + entrypoint.
COPY pdns/pdns.conf.tmpl /etc/pdns/pdns.conf.tmpl
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

# DNS (UDP/TCP) + REST API. Published selectively in docker-compose.
EXPOSE 53/udp 53/tcp 8081/tcp

ENTRYPOINT ["/sbin/tini", "--", "/usr/local/bin/entrypoint.sh"]
