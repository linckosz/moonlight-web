# mw-proxy — least-privilege filtering gateway in front of the PowerDNS API,
# plus the update relay / installed-version census (update.go).
# Static, dependency-free Go binary on a minimal runtime. Never published to the
# host: Caddy reverse-proxies it as https://dnsapi.{MW_DOMAIN} and
# https://updates.{MW_DOMAIN}.
FROM golang:1.22-alpine AS build
WORKDIR /src
# No third-party modules (stdlib only), so just copy the sources and build.
# Glob rather than a file list: a new source file must never be silently left
# out of the binary (`go build` ignores the _test.go files it also picks up).
COPY go.mod ./
COPY *.go ./
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /mw-proxy .

FROM alpine:3.20
RUN adduser -D -H -u 10001 mwproxy \
    && mkdir -p /data && chown mwproxy /data
COPY --from=build /mw-proxy /usr/local/bin/mw-proxy
USER mwproxy
EXPOSE 8080
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/mw-proxy"]
