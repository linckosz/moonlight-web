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
# The proxy is the only thing between a restricted key shared by every instance
# and the real PowerDNS key, and nothing else builds it — no CI job, and the Go
# toolchain is not installed on the machines this repo is developed on. So its
# tests run here, on every `docker compose up -d --build`. A red test failing the
# deploy is the point: the alternative is shipping the gateway untested.
# CGO stays off for the tests too — this image has no C toolchain.
RUN CGO_ENABLED=0 go vet ./... && CGO_ENABLED=0 go test ./...
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /mw-proxy .

FROM alpine:3.20
RUN adduser -D -H -u 10001 mwproxy \
    && mkdir -p /data && chown mwproxy /data
COPY --from=build /mw-proxy /usr/local/bin/mw-proxy
USER mwproxy
EXPOSE 8080
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/mw-proxy"]
