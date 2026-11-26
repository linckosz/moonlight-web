# mw-feedback — the "Send feedback" relay: an instance's message → a private
# Discord channel. The only holder of the webhook URL (see main.go).
# Static, dependency-free Go binary on a minimal runtime. Never published to the
# host: Caddy reverse-proxies /v1/feedback on metrics.{MW_DOMAIN}.
FROM golang:1.27-alpine AS build
WORKDIR /src
COPY go.mod ./
COPY *.go ./
# As for mw-proxy, nothing else runs these tests — no CI job, no Go toolchain on
# the development machines — so a red test fails the deploy, on purpose.
RUN CGO_ENABLED=0 go vet ./... && CGO_ENABLED=0 go test ./...
RUN CGO_ENABLED=0 GOOS=linux go build -trimpath -ldflags="-s -w" -o /mw-feedback .

FROM alpine:3.22
# Non-root, and nothing to write: no volume, so the compose service can run it
# with a read-only root filesystem.
RUN adduser -D -H -u 10002 mwfeedback
COPY --from=build /mw-feedback /usr/local/bin/mw-feedback
USER mwfeedback
EXPOSE 8091
ENTRYPOINT ["/usr/local/bin/mw-feedback"]
