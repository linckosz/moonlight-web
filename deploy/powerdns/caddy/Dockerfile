# Dedicated Caddy image: official Caddy + the caddy-ratelimit plugin (built via
# xcaddy). One process, one container — reverse-proxies the PowerDNS REST API
# over HTTPS with automatic TLS (or user-supplied cert files).
FROM caddy:2-builder AS builder
RUN xcaddy build --with github.com/mholt/caddy-ratelimit

FROM caddy:2
COPY --from=builder /usr/bin/caddy /usr/bin/caddy
COPY Caddyfile.tmpl /etc/caddy/Caddyfile.tmpl
COPY entrypoint.sh  /usr/local/bin/mw-caddy-entrypoint.sh
RUN chmod +x /usr/local/bin/mw-caddy-entrypoint.sh
# The presentation website (repo-root ./website) is bind-mounted at /srv/site
# by docker-compose — see the caddy service volumes.

EXPOSE 80/tcp 443/tcp
ENTRYPOINT ["/usr/local/bin/mw-caddy-entrypoint.sh"]
