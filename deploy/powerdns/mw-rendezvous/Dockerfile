# mw-rendezvous — the introduction server: held host lines and signalling relay.
# Static, dependency-free Go binary on a minimal runtime. Never published to the
# host: Caddy reverse-proxies it under https://stream.{MW_DOMAIN}/v1/.
FROM golang:1.22-alpine AS build
WORKDIR /src
# No third-party modules (stdlib only), so just copy the sources and build.
# Glob rather than a file list: a new source file must never be silently left
# out of the binary (`go build` ignores the _test.go files it also picks up).
COPY go.mod ./
COPY *.go ./
# This service hand-rolls RFC 6455 framing and arbitrates who owns which
# identifier, and nothing else builds it — no CI job, and the Go toolchain is not
# installed on the machines this repo is developed on. So its tests run here, on
# every `docker compose up -d --build`. A red test failing the deploy is the
# point: the alternative is shipping the relay untested.
# CGO stays off for the tests too — this image has no C toolchain. -race would
# need it, so the concurrency tests rely on the scheduler rather than the
# detector; run `go test -race ./...` by hand when touching hub.go.
RUN CGO_ENABLED=0 go vet ./... && CGO_ENABLED=0 go test ./...
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /mw-rendezvous .

FROM alpine:3.20
RUN adduser -D -H -u 10002 mwrdv \
    && mkdir -p /data && chown mwrdv /data
COPY --from=build /mw-rendezvous /usr/local/bin/mw-rendezvous
USER mwrdv
EXPOSE 8090
VOLUME ["/data"]
ENTRYPOINT ["/usr/local/bin/mw-rendezvous"]
