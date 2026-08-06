# MoonlightWeb in Docker

Official images for Linux servers, NAS boxes and mini PCs, built from the
[`Dockerfile`](../Dockerfile) at the repository root on **official Debian**
(`debian:trixie`), published to the GitHub Container Registry for
**`linux/amd64`** and **`linux/arm64`**.

```sh
docker run -d --name moonlightweb \
  --network host \
  --cap-drop ALL --cap-add NET_BIND_SERVICE \
  -v mw-data:/data \
  --restart unless-stopped \
  ghcr.io/linckosz/moonlight-web:latest
```

Then open **`https://<server-ip>`** from any device on your network and accept
the self-signed certificate. The access PIN and the URLs come from:

```sh
docker exec moonlightweb moonlightweb --status
```

Prefer Compose: [`docker-compose.yml`](docker-compose.yml) is the same thing in
a file. [`docker-compose.bridge.yml`](docker-compose.bridge.yml) is the fallback
for platforms with no host networking — read the caveats in it first.

| Tag | What it is |
|---|---|
| `latest` | The newest release. |
| `0.2.4` · `0.2` | That exact release · the newest `0.2.x`. |
| `edge` | The current tip of `main`. Newer, less tested. |
| `sha-<commit>` | One commit, pinned forever. |

**Architectures: `linux/amd64` and `linux/arm64` only.** Both are built on native
runners; no 32-bit `armv7`/`armhf` image is published. There is no hosted 32-bit
ARM runner, so it would have to be emulated under QEMU — and emulating a full
Qt + libdatachannel C++ build costs hours per commit for a target that Raspberry
Pi OS has shipped a 64-bit default for since 2022. A Pi 3, 4, 5 or Zero 2 W runs
the `arm64` image; genuinely 32-bit hardware can
[build from source](#building-it-yourself), which works — it is only the CI cost
that rules it out.

---

## Why `--network host`

MoonlightWeb is not a plain web server; it is one end of a WebRTC connection
and an mDNS client. Three things break behind a bridge, and publishing more
ports fixes none of them:

- **WebRTC media.** libdatachannel binds an **ephemeral** UDP port for the
  media and input transport, unless UPnP handed it a fixed one. An ephemeral
  port cannot be published in advance. When the browser cannot reach it, the
  stream falls back to its TCP/WebSocket transport — it works, but you pay for
  it in latency.
- **Host discovery.** Sunshine machines announce themselves over mDNS
  (multicast UDP 5353), which does not cross the default bridge. On a bridge,
  add your hosts by IP address instead — a one-time step, then everything else
  behaves normally.
- **UPnP.** The router is found by SSDP multicast — same story. Without it,
  Internet Access cannot open your router's ports for you.

Host networking removes all three at once, and there is nothing to gain from
isolating a service whose whole job is to be reachable. If your platform has no
host networking (Docker Desktop on macOS/Windows, some NAS UIs, rootless
Podman), use `docker-compose.bridge.yml` and accept the two caveats above.

---

## Ports

### Inbound — what browsers connect to

These are the ports to publish (bridge) or to open in the **host's** firewall
(host networking), and to forward on your router if you want access from
outside the LAN.

| Port | Proto | Needed | What goes over it |
|---|---|---|---|
| **443** | TCP | **Always** | The web UI, the REST API, and the `wss://…/ws` upgrade that carries WebRTC signalling. The one port a browser cannot do without. |
| **80** | TCP | Recommended | Redirect to HTTPS, so `http://server` reaches the app instead of failing. Also the `http-01` challenge address when Internet Access issues a public TLS certificate — required if you turn that on. |
| **48010–48014** | UDP | With UPnP | WebRTC media, audio and input. The app asks UPnP for 48010 and falls back through 48014; whichever it gets, it binds libdatachannel to that exact port so the router mapping and the socket agree. |
| **47999** | UDP | Internet Access | Mapped alongside 80/443 when Internet Access is enabled. |
| *ephemeral* | UDP | No UPnP | With no UPnP mapping there is no fixed port: libdatachannel takes whatever the kernel gives it. On the host network that is fine unless a host firewall blocks the range. |

> **In short:** on a LAN, **443/tcp** and **80/tcp**. If a firewall on the host
> drops inbound UDP, either allow the ephemeral range or let UPnP pin 48010.
> The `.deb` and `.rpm` open `443/tcp`, `80/tcp` and `47999/udp` for you in
> firewalld/ufw; a container does not touch the host's firewall, so that is
> yours to do.

Ports 80 and 443 are the defaults; `MW_HTTP_PORT` / `MW_HTTPS_PORT` move them.

### Outbound — what MoonlightWeb connects to

Nothing to publish, but these paths have to exist. On the host network they do.

| Destination | Port | Proto | What for |
|---|---|---|---|
| Sunshine host | 47989 | TCP | GameStream HTTP: pairing, app list, host info. |
| Sunshine host | 47984 · 47990 | TCP | GameStream HTTPS (GeForce Experience · Sunshine defaults). |
| Sunshine host | 48010 | TCP | RTSP handshake that sets the stream up. |
| Sunshine host | 47998 · 47999 · 48000 | UDP | Video · control/input · audio. |
| LAN multicast | 5353 | UDP | mDNS host discovery (host networking only). |
| LAN multicast | 1900 | UDP | SSDP, to find the UPnP router (host networking only). |
| Internet | 19302 | UDP | STUN, to learn the public address for a stream from outside the LAN. |
| Internet | 443 | TCP | Update check, ACME certificate issuance, DNS API for Internet Access. |

---

## Storage

One volume, `/data`, mounted at the container's `$HOME`. Qt derives every path
from it:

| Path inside `/data` | What lives there |
|---|---|
| `.local/share/MoonlightWeb/MoonlightWeb/settings.json` | Ports, codec, transport, admin password hash, access PIN. |
| `.local/share/MoonlightWeb/MoonlightWeb/cert/` | The TLS certificate and key the browser sees. |
| `.local/share/MoonlightWeb/MoonlightWeb/logs/` | `moonlightweb.log`, plus one file per stream worker. |
| `.config/MoonlightWeb/MoonlightWeb.conf` | Paired hosts **and the Moonlight client identity**. |

Losing the volume un-pairs every host — the identity is what Sunshine
recognises. Back it up with the container stopped:

```sh
docker run --rm -v mw-data:/data -v "$PWD:/backup" debian:trixie-slim \
  tar czf /backup/moonlightweb-data.tgz -C /data .
```

---

## Environment variables

| Variable | Default | What it does |
|---|---|---|
| `MW_HTTPS_PORT` | `443` | HTTPS listener. Written into `settings.json` on every start, so it survives the fallback the server applies when a port is taken. |
| `MW_HTTP_PORT` | `80` | HTTP listener (redirect + ACME challenge). |
| `MW_UPNP` | `1` | `0` stops the app asking the router to map ports — set it on a bridge network, where the request cannot reach a router anyway. |
| `TZ` | `Etc/UTC` | Timestamps in the logs and the admin UI. |
| `MW_DOMAIN`, `MW_PDNS_URL`, `MW_PDNS_TOKEN`, `MW_ZEROSSL_EAB_KID`, `MW_ZEROSSL_EAB_HMAC` | baked in | Internet Access's DNS and ACME configuration. The official images carry the project's, exactly like the `.deb`; set them to point the feature at your own PowerDNS + ACME account. |

Everything else is configured from the admin page, or by editing
`settings.json` in the volume and restarting — see
[`docs/wiki/07-Settings-Reference.md`](../docs/wiki/07-Settings-Reference.md).

Anything after the image name is passed straight to the binary:

```sh
docker run … ghcr.io/linckosz/moonlight-web:latest --port 8080
```

---

## Operating it

```sh
docker exec moonlightweb moonlightweb --status               # URLs, PIN, internet state
docker exec moonlightweb moonlightweb --new-pin              # let one more device in
docker exec -it moonlightweb moonlightweb --set-admin-password
docker exec -it moonlightweb moonlightweb --enable-internet  # public sub-domain + TLS
docker logs -f moonlightweb
```

The image's `HEALTHCHECK` is `moonlightweb --status`, which answers only once
the TLS listener is up — so `docker ps` showing *healthy* means a browser can
actually connect.

**Updating** is `docker pull` + recreate; the in-app update button does not
apply here (it installs a package onto the machine it runs on, which in a
container the next `docker run` would throw away).

---

## Hardware acceleration: none needed

There is nothing to pass through — no `--device /dev/dri`, no NVIDIA container
runtime, no VA-API packages.

MoonlightWeb never decodes or re-encodes the stream. Sunshine encodes on the
gaming PC's GPU; this server re-packetises the encrypted RTP into WebRTC and
the **browser** decodes it on the viewer's device. The upscaling and sharpening
(FSR1 / SGSRv1) run in the browser on WebGPU, on the viewer's GPU. The
container's job is packet plumbing and TLS, and it is comfortable on a
Raspberry Pi 4 or an N100 mini PC.

Gamepads, keyboard and mouse are the same story: input is captured by the
browser, arrives over the WebRTC data channel and leaves as GameStream input
packets to Sunshine. No `/dev/input`, no `uinput`, no privileged container.

**Sunshine is not in this image and cannot be.** A container has no display to
capture. Run Sunshine on the machine with the GPU, and add it here by IP.

---

## Running as a service

`--restart unless-stopped` (or `restart: unless-stopped` in Compose) is enough
on any host where Docker itself starts at boot. For a systemd unit that owns
the Compose project instead:

```ini
# /etc/systemd/system/moonlightweb-docker.service
[Unit]
Description=MoonlightWeb (Docker Compose)
Requires=docker.service
After=docker.service network-online.target
Wants=network-online.target

[Service]
Type=oneshot
RemainAfterExit=yes
WorkingDirectory=/opt/moonlightweb
ExecStart=/usr/bin/docker compose up -d
ExecStop=/usr/bin/docker compose down
ExecReload=/usr/bin/docker compose pull && /usr/bin/docker compose up -d

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl enable --now moonlightweb-docker
```

Not running in Docker at all? [`../backend/packaging/systemd/moonlightweb.service`](../backend/packaging/systemd/moonlightweb.service)
is the native unit the `.deb` and `.rpm` install.

---

## Running as a non-root user

The image runs as root so it can bind 80 and 443, with every capability except
`NET_BIND_SERVICE` dropped. To run as somebody else, move the listeners above
1024 **and** give that user the volume — a fresh named volume is created
root-owned, and the container has no privilege left to fix that itself:

```sh
mkdir -p /srv/moonlightweb && chown 1000:1000 /srv/moonlightweb
docker run -d --name moonlightweb --network host \
  --user 1000:1000 --cap-drop ALL \
  -e MW_HTTP_PORT=8080 -e MW_HTTPS_PORT=8443 \
  -v /srv/moonlightweb:/data \
  ghcr.io/linckosz/moonlight-web:latest
```

The URL becomes `https://<server-ip>:8443`.

---

## Building it yourself

```sh
git clone --recurse-submodules https://github.com/linckosz/moonlight-web
cd moonlight-web
docker build -t moonlightweb --build-arg MW_VERSION=0.0.0-dev .
```

The submodules are not optional: moonlight-common-c, libdatachannel, miniupnpc
and qmdnsengine are compiled into the binary, and `.dockerignore` sends only
`backend/`, `frontend/` and `docker/` to the daemon. `--build-arg
DEBIAN_RELEASE=bookworm` builds against Debian 12 (Qt 6.4) instead of trixie
(Qt 6.8) if you need the older base.

The build stage uses Debian's Qt 6 and OpenSSL development packages and the
runtime stage the matching runtime ones, so nothing is privately bundled:
rebuilding on a patched base is how this image gets its security updates.

---

## Troubleshooting

**The container starts and immediately exits 0.** A single-instance lock left
by a previous container. The entrypoint deletes it on every start — if you see
this, you are running the binary without the entrypoint. Delete
`/data/.local/share/MoonlightWeb/MoonlightWeb/moonlightweb.lock`.

**`HTTPS server started on port 49443`** (not 443). Something else on the host
already holds 443 — on host networking that is a real conflict with a reverse
proxy or another web server. Either free the port or set `MW_HTTPS_PORT` and
put your proxy in front.

**The web UI loads but the stream never starts.** The signalling worked and the
media path did not — the usual bridge-network symptom. Check
`docker logs` for the ICE state, and move to host networking.

**No hosts found.** mDNS does not cross a bridge, and some LANs block multicast
outright. Add the host by IP from *Add computer* — discovery is a convenience,
not a requirement.

**`Qt platform: offscreen (no display — headless)` in the log.** Expected. It
is what the image is supposed to say.
