# MoonlightWeb in Docker

Official images for Linux servers, NAS boxes and mini PCs, built from the
[`Dockerfile`](../Dockerfile) at the repository root on **official Debian**
(`debian:trixie`), published to the GitHub Container Registry for
**`linux/amd64`** and **`linux/arm64`**.

---

## Quick start — headless Linux server (Ubuntu x64, Debian, a VM, a NUC)

Four steps. Copy them in order; the container prints the same list back at you
in `docker logs` if you lose your place.

### 1. Run it

```sh
sudo docker run -d --name moonlightweb \
  --network host \
  --cap-drop ALL --cap-add NET_BIND_SERVICE \
  -v mw-data:/data \
  --restart unless-stopped \
  ghcr.io/linckosz/moonlight-web:latest
```

`--network host` is what puts the container on your LAN — without it, it can
only reach machines by IP and will never discover them. See
[Why `--network host`](#why---network-host).

### 2. Open the ports — this is NOT automatic

The `.deb` and `.rpm` add firewall rules for you in their postinstall. **A
container cannot and must not touch the host's firewall**, so this one is
yours:

```sh
sudo ufw allow 443/tcp        # the web UI — required
sudo ufw allow 80/tcp         # HTTP-to-HTTPS redirect
sudo ufw reload
```

`firewall-cmd --permanent --add-port=443/tcp --add-port=80/tcp && firewall-cmd
--reload` on Fedora/RHEL. If `ufw status` says *inactive*, nothing is blocking
and there is nothing to do. Full table under [Ports](#ports).

### 3. Set the two credentials

```sh
sudo docker exec -it moonlightweb moonlightweb --set-admin-password
sudo docker exec     moonlightweb moonlightweb --new-pin
sudo docker exec     moonlightweb moonlightweb --status
```

`--status` prints the URL to open, the PIN, and what is still missing. Neither
credential has a default, and neither is optional — see
[First run](#first-run--the-pin-is-not-optional-here) for why a container gets
no free pass.

### 4. Pair a Sunshine machine and stream

1. Open **`https://<server-ip>`** in Chrome/Edge/Safari (use the IP, not
   `localhost` — the server is not the machine you are sitting at).
2. Accept the self-signed certificate warning. Expected on a LAN — the
   server serves a self-signed certificate unless you configure your own
   domain and certificate.
3. Type the **PIN** from step 3. One PIN per device — re-run `--new-pin` for
   the next phone or laptop.
4. Your Sunshine machines appear by themselves (mDNS). If one does not, or you
   are on a bridge network, **Add computer** → its IP address.
5. Click the machine → MoonlightWeb shows a **4-digit pairing PIN**. Type
   *that* one into Sunshine's own web UI at `https://<sunshine-machine>:47990`
   → *PIN*. (This is the reverse direction from step 3, and the one people mix
   up: MoonlightWeb issues it, Sunshine consumes it.)
6. Pick a game and launch.

---

## First run — the PIN is not optional here

On a desktop install the app trusts the machine it runs on: a browser on
`127.0.0.1` gets in without a PIN, and the setup wizard opens by itself. **A
container never sees that loopback peer.** Behind a bridge every request
arrives from the Docker gateway (`172.17.0.1`), and even on the host network a
headless server has no local browser to be trusted — so the request is remote,
`/api/setup/status` answers `401 authentication_required`, and the page loads
into an authentication wall rather than the wizard.

That is the design working as intended, not a bug: what would otherwise be a
"trusted local machine" is, in a container, every device that can reach the
port. So the first thing to do after `docker run` is `--new-pin`.

Two other commands are worth running while you are there — both talk to the
server over the container's own loopback, which is why they are allowed to:

```sh
docker exec -it moonlightweb moonlightweb --set-admin-password
```

Without an admin password **the admin page cannot be opened from anywhere** —
there is no built-in default, and the usual "you are on the machine itself"
exemption does not apply to a container. The PIN grants streaming; the password
grants administration.

Accept the self-signed certificate warning on first connection — expected on a
LAN, unless you configure your own domain and certificate.

---

## Tags

Prefer Compose: [`docker-compose.yml`](docker-compose.yml) is the quick start in
a file. [`docker-compose.bridge.yml`](docker-compose.bridge.yml) is the fallback
for platforms with no host networking — read the caveats in it first.

| Tag | What it is |
|---|---|
| `latest` | The newest release. What you want. |
| `0.2.4` | That exact release, forever. |
| `0.2` | The newest `0.2.x` — minor updates, no major jump. |
| `sha-<commit>` | One commit, pinned forever. |

**Every tag here is a release.** There is no `edge` and no nightly: an image is
published only from a `v*` tag that already passed the quality and test gates,
so `latest` can never hand you work in progress.

Development builds exist, in a **separate and private** package —
`ghcr.io/linckosz/moonlight-web-dev:dev`. It is produced only by running the
Docker workflow by hand with `publish` ticked, and pulling it needs
`docker login ghcr.io` with an account that has access: an anonymous request
for its tag list is refused with 403, where the public package answers 200.
Nothing you do with `docker pull …/moonlight-web:latest` can reach it.

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
  Internet Access cannot open the per-session streaming port for you.

Host networking removes all three at once, and there is nothing to gain from
isolating a service whose whole job is to be reachable.

**This is not something the application can decide for itself.** A process
cannot move into another network namespace after it has started, so there is no
`--access-host-network` command to run and no setting to tick: it is chosen by
`docker run`, before the binary exists. What the image *can* do is tell you
which side of that line it landed on, which it does in the first lines of
`docker logs`.

### The three options, ranked

| | LAN reach | mDNS discovery | WebRTC | When |
|---|---|---|---|---|
| **`--network host`** | full | ✅ | direct | **Default. Any Linux host.** |
| **macvlan** | full, own LAN IP | ✅ | direct | You want the container isolated *and* on the LAN — a NAS with several services fighting over port 443. |
| **bridge** (published ports) | by IP only | ❌ | may fall back to TCP | Docker Desktop on macOS/Windows, rootless Podman, a NAS UI that only offers port mappings. |

macvlan gives the container its own MAC and its own address on your LAN, so
multicast works and nothing collides with the host's ports:

```sh
# Adjust parent= to your NIC (ip -br link) and the subnet to your LAN.
sudo docker network create -d macvlan \
  --subnet=192.168.1.0/24 --gateway=192.168.1.1 \
  -o parent=eth0 mw-lan

sudo docker run -d --name moonlightweb \
  --network mw-lan --ip 192.168.1.240 \
  -v mw-data:/data --restart unless-stopped \
  ghcr.io/linckosz/moonlight-web:latest
```

One caveat worth knowing before you pick it: by design, a macvlan container and
its **own host** cannot talk to each other. Fine when Sunshine runs elsewhere;
not fine when Sunshine runs on that same box — use host networking there.

If none of these are available, `docker-compose.bridge.yml` states its two
caveats up front and works for everything except discovery and the lowest
latency.

---

## Ports

### Inbound — what browsers connect to

These are the ports to publish (bridge) or to open in the **host's** firewall
(host networking), and to forward on your router if you want access from
outside the LAN.

| Port | Proto | Needed | What goes over it |
|---|---|---|---|
| **443** | TCP | **Always** | The web UI, the REST API, and the `wss://…/ws` upgrade that carries WebRTC signalling. The one port a browser cannot do without. |
| **80** | TCP | Recommended | Redirect to HTTPS, so `http://server` reaches the app instead of failing. |
| **48010–48014** | UDP | With UPnP | WebRTC media, audio and input. The app asks UPnP for 48010 and falls back through 48014; whichever it gets, it binds libdatachannel to that exact port so the router mapping and the socket agree. |
| **47999** | UDP | Legacy Internet Access | Mapped alongside 80/443 only by an install that still holds a legacy `moonlightweb.top` sub-domain. |
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
| Internet | 443 | TCP | Update check; plus ACME certificate renewals and the DNS API, only for an install that still holds a legacy sub-domain. |

---

## Streaming from outside the LAN (Internet Access)

Internet Access is an opt-in consent, off by default. While it is on, the app
asks the router (UPnP, SSDP multicast — so **host networking**) to open a
streaming port during each session; whoever connects reaches this machine
directly, and each side of a peer-to-peer connection sees the other's public IP
address. No DNS record is created, no certificate is issued, and ports 80/443
stay closed:

```sh
docker exec -it moonlightweb moonlightweb --enable-internet
```

A new remote entry link — reaching the web UI itself from outside without
opening it to the internet — is in development and will be announced in the
release notes. Until then a remote browser has no way in unless you forward
TCP 443 yourself and accept the self-signed warning, or bring your own domain
and certificate.

**A container that still holds a legacy `moonlightweb.top` sub-domain** (one
that enabled Internet Access on v0.2.4 or earlier) keeps its sub-domain,
certificate renewals and 80/443 forwards unchanged until the shared DNS
service shuts down in February 2027. For those, the official images carry the
same DNS and ACME configuration the installers do; pointing them at your own
PowerDNS and ACME account instead is a matter of setting `MW_DOMAIN`,
`MW_PDNS_URL`, `MW_PDNS_TOKEN`, `MW_ZEROSSL_EAB_KID` and
`MW_ZEROSSL_EAB_HMAC` — a runtime value always wins over the one compiled
in.

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
| `MW_HTTP_PORT` | `80` | HTTP listener (HTTP-to-HTTPS redirect). |
| `MW_UPNP` | `1` | `0` stops the app asking the router to map ports — set it on a bridge network, where the request cannot reach a router anyway. |
| `TZ` | `Etc/UTC` | Timestamps in the logs and the admin UI. |
| `MW_DOMAIN`, `MW_PDNS_URL`, `MW_PDNS_TOKEN`, `MW_ZEROSSL_EAB_KID`, `MW_ZEROSSL_EAB_HMAC` | baked in | DNS and ACME configuration for a legacy sub-domain (pre-v0.2.5 installs only). The official images carry the project's, exactly like the `.deb`. |

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
docker exec -it moonlightweb moonlightweb --enable-internet  # allow the internet link
docker logs -f moonlightweb
```

The image's `HEALTHCHECK` is `moonlightweb --status`, which answers only once
the TLS listener is up — so `docker ps` showing *healthy* means a browser can
actually connect.

### Updating

`docker pull` + recreate. The volume carries the settings, the certificate and
the pairings across, so nothing is re-entered:

```sh
sudo docker pull ghcr.io/linckosz/moonlight-web:latest
sudo docker stop moonlightweb && sudo docker rm moonlightweb
sudo docker run -d --name moonlightweb … # the same command as step 1
```

With Compose: `docker compose pull && docker compose up -d`.

**The in-app update button is deliberately absent in a container.** On Windows
and macOS it downloads the installer and runs it; here that would install a
`.deb` into the image's writable layer, appear to work, and be discarded by the
next `docker run` — the version would silently go *backwards* on the next
restart. `SelfUpdater` detects the container and reports `supported: false`, so
the UI never offers a button that cannot keep its promise.

The update *check* still runs: you are told a new version exists, and the
registry is where you get it. Pin `0.2` instead of `latest` if you would rather
take minor releases and skip a major one, or a `sha-…` tag to freeze entirely.

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

**`HTTPS server started on port 8443`** (not 443). Something else on the host
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
