# syntax=docker/dockerfile:1
# =============================================================================
#  MoonlightWeb — official container image (Debian).
#
#      docker build -t moonlightweb .
#      docker run -d --network host -v mw-data:/data moonlightweb
#
#  Published by .github/workflows/docker.yml to
#  ghcr.io/linckosz/moonlight-web for linux/amd64 and linux/arm64.
#
#  Two stages, both on the same official Debian release:
#
#    build    — Debian's own Qt 6 and OpenSSL development packages, plus the
#               four vendored dependencies (moonlight-common-c, libdatachannel,
#               miniupnpc, qmdnsengine) compiled statically into one binary,
#               then `cmake --install` to stage that binary next to the frontend
#               it serves.
#    runtime  — debian:<release>-slim plus the matching Qt 6 *runtime* packages.
#               Nothing is bundled by hand: the distro's dynamic linker resolves
#               Qt and OpenSSL, so security updates arrive by rebuilding on a
#               newer base rather than by re-bundling a private Qt.
#
#  Why Debian's Qt rather than the 6.11 the desktop packages ship: an image is
#  rebuilt, not shipped once, so a distro-maintained Qt with a security team is
#  worth more here than the two minor versions. The code compiles against Qt
#  6.4+ (the QT_VERSION_CHECK guards in SignalingServer/StreamRelay), and
#  trixie carries 6.8 LTS.
#
#  This image is headless by construction: no display, so no tray icon, no
#  browser auto-open and no Sunshine — MoonlightWeb reaches its Sunshine hosts
#  over the network (add them by IP or, on the host network, by mDNS). See
#  docker/README.md for the ports and for why `--network host` is the supported
#  layout.
# =============================================================================

ARG DEBIAN_RELEASE=trixie

# ── Build ────────────────────────────────────────────────────────────────────
FROM debian:${DEBIAN_RELEASE} AS build

# qt6-base-dev covers Core/Gui/Widgets/Network; WebSockets is a separate module.
# libssl-dev is required by both the server (TLS listener, ACME) and
# moonlight-common-c (the GameStream crypto).
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        build-essential \
        ca-certificates \
        cmake \
        ninja-build \
        pkg-config \
        libssl-dev \
        qt6-base-dev \
        qt6-websockets-dev \
 && rm -rf /var/lib/apt/lists/*

# The .git directory is not in the build context (see .dockerignore), so the
# `git describe` fallback in backend/CMakeLists.txt cannot run — CI passes the
# release version here, and a local build without it reports the project()
# version. The value is baked into the binary (MW_VERSION) and is what
# `--version`, the update check and the User-Agent report.
ARG MW_VERSION=""

# Internet Access configuration, embedded at configure time exactly as the .deb
# and the installers embed it (backend/CMakeLists.txt reads these from the
# environment). Empty in a local build → the image is LAN-only, which is the
# sane default for someone building their own.
#
# These are build-stage ARGs, and the runtime stage below starts from its own
# FROM: nothing here reaches the published image's layers or `docker history`.
# The values do end up compiled into the binary, which is the same exposure the
# distributed packages already have.
ARG MW_DOMAIN=""
ARG MW_PDNS_URL=""
ARG MW_PDNS_TOKEN=""
ARG MW_ZEROSSL_EAB_KID=""
ARG MW_ZEROSSL_EAB_HMAC=""

WORKDIR /src
COPY backend backend
COPY frontend frontend

# The ARGs above are already in this shell's environment (Docker exports build
# arguments to RUN), which is where CMake looks for them.
RUN cmake -S backend -B /tmp/build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DMW_VERSION="${MW_VERSION}" \
 && cmake --build /tmp/build -j "$(nproc)" \
 && cmake --install /tmp/build --prefix /stage \
 && rm -rf /tmp/build

# ── Runtime ──────────────────────────────────────────────────────────────────
FROM debian:${DEBIAN_RELEASE}-slim

# libqt6widgets6 pulls Gui + Core, libqt6websockets6 pulls Network, and
# qt6-qpa-plugins carries the `offscreen` platform plugin main.cpp falls back to
# when neither DISPLAY nor WAYLAND_DISPLAY is set — without it a QApplication
# aborts at construction and the container never starts.
#
# jq is used by the entrypoint to merge MW_HTTP_PORT / MW_HTTPS_PORT / MW_UPNP
# into settings.json without clobbering the rest of the file.
RUN apt-get update \
 && apt-get install -y --no-install-recommends \
        ca-certificates \
        jq \
        libqt6svg6 \
        libqt6websockets6 \
        libqt6widgets6 \
        qt6-qpa-plugins \
        tzdata \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /stage /opt/moonlightweb
COPY docker/entrypoint.sh /usr/local/bin/entrypoint.sh

# `moonlightweb` on PATH, so the operator commands read exactly as they do on a
# .deb install: docker exec <container> moonlightweb --status | --new-pin | …
RUN ln -s /opt/moonlightweb/MoonlightWeb /usr/local/bin/moonlightweb \
 && chmod +x /usr/local/bin/entrypoint.sh

# HOME is the whole persistence story: Qt derives both of its state directories
# from it — AppDataLocation (settings.json, cert/, logs/) and QSettings (the
# paired hosts and the Moonlight client identity) — so one volume covers
# everything that must survive a `docker rm`. Layout: docker/README.md.
#
# LANG because a slim Debian carries no locale at all, and Qt prints a four-line
# complaint on every start before falling back to C.UTF-8 by itself.
#
# MW_SERVICE marks this as a supervised launch — no tray, no browser auto-open,
# and on a restart no stealing of a UPnP mapping another device owns — exactly
# as the systemd unit and the Windows service do.
ENV HOME=/data \
    LANG=C.UTF-8 \
    XDG_RUNTIME_DIR=/tmp/runtime-moonlightweb \
    QT_QPA_PLATFORM=offscreen \
    MW_SERVICE=1
VOLUME ["/data"]

# Documentation only — EXPOSE publishes nothing. 443 is the web UI and the
# signalling upgrade, 80 the redirect and the ACME http-01 challenge; the UDP
# ports carry WebRTC media in the case where UPnP mapped them (48010 and its
# four fallbacks, plus the 47999 Internet Access maps alongside 80/443).
# Without UPnP the media port is ephemeral and cannot be listed here at all —
# which is exactly why `--network host` is the supported layout. Full table:
# docker/README.md.
EXPOSE 443/tcp 80/tcp 47999/udp 48010-48014/udp

# --status talks to the running instance over loopback and exits non-zero when
# nothing answers, so it doubles as the liveness probe. start-period covers the
# first launch, where a self-signed certificate has to be generated first.
HEALTHCHECK --interval=30s --timeout=10s --start-period=60s --retries=3 \
    CMD moonlightweb --status > /dev/null || exit 1

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
CMD []
