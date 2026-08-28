[← Architecture](02-Architecture.md) · **Backend** · [Next: Frontend →](04-Frontend.md)

---

# 3. Backend (C++ / Qt)

The backend is a single windowless executable (`MoonlightWeb` / `MoonlightWeb.exe`) built from `backend/`. It embeds the web server, the GameStream client and the streaming bridge. Everything is wired in `backend/src/main.cpp` (the composition root).

## 3.1 Module map

| Directory | Classes | Responsibility |
|---|---|---|
| `src/server/` | `HttpServer`, `RestRouter`, `StaticFileHandler`, `HttpParser` | HTTP :80 (redirect) + HTTPS :443 listener, request parsing, routing, static frontend serving, WebSocket proxying |
| | `AuthManager`, `ConnectionGuard` | PIN/certificate/host-key auth, sessions, rate limiting, in-process IP banning |
| | `AppSettings`, `Provisioning` | `settings.json` persistence; installer `provisioning.json` first-boot consumption |
| | `CertManager` | TLS cert discovery/loading (env var, file, CN matching), self-signed generation, hot reload |
| | `ControlChannel` | `/ws/control` single-tab dedup WebSocket server |
| | `ControlTunnel` | Host end of the WebRTC connection a remote browser arrives on — feeds `HttpServer::serveRequest()`, never its own pipeline (see [§3.4](#34-internet-access-and-the-way-in)) |
| | `routes/` | `AuthRoutes`, `HostRoutes`, `SystemRoutes` — see [REST API](08-REST-API.md) |
| `src/backend/` | `ComputerManager` | Host inventory: mDNS discovery, polling, box-art fetching, persistence. Polling is **suspended while a relay is active** (polling Sunshine's HTTP server mid-stream can wedge it). |
| | `NvHTTP`, `NvComputer`, `NvApp`, `NvAddress` | Async GameStream HTTP client + host/app data model. **Never leave a socket to Sunshine pooled**: its HTTPS server is single-threaded and Qt holds finished TLS sockets ~120 s, which wedges it for every other client. Each request either sends `Connection: close` or calls `dropPooledConnections()` when it completes — `/cancel` is issued by the long-lived parent, so a pooled one silenced the host for a full minute on 2026-08-07 and made the transport chain collapse. |
| | `NvPairingManager`, `IdentityManager` | GameStream pairing (challenge-response) + persistent RSA client identity |
| | `SunshineInstaller`, `SunshineRestClient` | On-demand Sunshine install; REST pairing (`/api/pin`) for the wizard. Sunshine liveness is probed on **port 47989** (pgrep is unreliable on macOS). |
| `src/streaming/` | see [§3.3](#33-streaming-layer) | The streaming bridge |
| `src/network/` | `InternetAccessManager`, `RendezvousClient`, `StunClient`, `UPNPClient`, `GeoIpService`, `UpdateChecker`, `SelfUpdater` | Internet access orchestration and the held line (see [§3.4](#34-internet-access-and-the-way-in)); update check + unattended self-update (see [Installers §9.4](09-Installers-and-Packaging.md#94-shared-runtime-behaviors)) |
| `src/common/` | `Logger`, `CrashHandler`, `RendezvousId`, `Types.h`, `MacActivity` | File logging (Qt messages captured via `qInstallMessageHandler`), Windows minidumps, the entry identifier's alphabet and folding rules, shared HTTP types, macOS activity assertions |
| `TrayManager`, `Autostart` | | Tray icon (server mode, or the reduced *client mode* that decorates a service running elsewhere) + login-item/autostart registration (exit code 0 = voluntary quit, supervisors don't restart) |

## 3.2 HTTP server

`HttpServer` (`src/server/HttpServer.h`) is a hand-rolled Qt TCP/SSL server (no QtHttpServer dependency):

- **Two listeners**: plain HTTP (redirects to HTTPS) and HTTPS, with port-fallback ranges so a second instance on the same machine still comes up. `activeHttpsPort()` — not the configured one — is what every entry point is built from: printing the configured 443 after a fallback would send the user to the wrong place.
- **`Connection: close`** after every response — this is why `ConnectionGuard`'s flood threshold is generous (a single page load is dozens of connections).
- **WebSocket upgrades** are detected and raw-proxied to internal WS servers: signaling (48001), stream relay (48002), control channel (48003), plus the second stream slot (`/ws1` → 48011, `/ws1/stream` → 48012). The proxy cleanup lambda uses a `shared_ptr` guard (a `bool*` guard historically caused a use-after-free during `~QSslSocket` on macOS).
- **Async routes**: handlers may take a `ResponseCallback` and answer later (30 s timeout); sockets pending an async answer are tracked in `m_PendingAsyncSockets`.
- **Auth enforcement**: every `/api/*` request except `health`, `server/hostname` and `auth/*` requires localhost or a valid `mw_session` cookie; `/api/admin/*` additionally requires localhost (or a host session). 401s are reported to `ConnectionGuard`.

`CertManager` resolves the TLS certificate in priority order: explicit `cert_pem`/`cert_key` from settings (each is *either an env-var name or a file path*), then file scan, then **self-signed generation** for LAN use. A CN-matching check prevents serving a stale cert after the domain changed. `reloadTls()` swaps certs hot, so a replaced certificate is picked up without a restart. A remote browser never meets any of this: it arrives over the tunnel, inside DTLS the bootstrap has already bound to this machine's key.

## 3.3 Streaming layer

| Class | Role |
|---|---|
| `StreamWorkerHost` + `worker/StreamWorkerMain` | Parent-side handle and child-side entry point for one `--stream-worker` process (JSON lines over stdin/stdout). Everything below runs **inside that child**; the parent keeps two slots alive (live + standby) — see [Streaming §5.6](05-Streaming-and-Transports.md#56-session-lifecycle--teardown-discipline). |
| `StreamSession` (`Session.cpp`) | Ephemeral orchestrator for one `/start` request: launches/resumes the app on Sunshine (per-browser `uniqueid`, `s_ActiveUniqueIds` registry decides launch-vs-resume), builds the `StreamConfig`, creates the shim + the relay for the chosen transport, answers the HTTP request with `{ws_url, transport_chain, transport_index, negotiated codec…}`, then self-deletes once streaming runs. AV1 is force-falled-back to H.264 here when needed. |
| `MoonlightShim` | The bridge into `moonlight-common-c` (`LiStartConnection` + decoder/audio/connection callbacks). Receives decoded-protocol H.264/HEVC/AV1 access units and Opus packets on moonlight's threads and hands them to the active relay. `stopConnection()` must always run **before** relay destruction (UAF otherwise). |
| `SignalingServer` | Per-session WebSocket server for SDP/ICE. Only advertises the private LAN ICE candidate to clients detected as local (never leaks the LAN IP to internet peers). |
| `DataChannelRelay` | **webrtc-dc** transport: libdatachannel PeerConnection with a video DataChannel (negotiated id 0), an **RTP Opus audio track** (like `MediaTrackRelay` — id 1 is reserved-unused) and an input DataChannel (id 2, JSON both ways: input in, stats/pong/rumble/clipboard out). Video DC is *ordered* with `maxRetransmits=3`; frames carry a `frameId` (gap detection triggers backend IDR); a 256 KB high-watermark with keyframe-priority avoids blocking; IDR requests are throttled/coalesced (250–500 ms cooldown, exponential backoff under congestion). |
| `MediaTrackRelay` | **webrtc-media** transport: native RTP H.264 track + Opus track (browser jitter buffer, FEC/PLC). H.264-only. Sends from the capture thread, dynamic RTP timestamps, proactive IDR every 250 ms until the first client PLI-equivalent (browsers send no PLI over this path). |
| `StreamRelay` | **wss** transport: everything multiplexed over one WebSocket (worst network conditions, always works). Runs on a dedicated thread. |
| `FrameSender` | Dedicated send thread used by the DC relay so a slow SCTP send never blocks the moonlight callback thread. |
| `EnetControlStream` | Reliable ENet channel to Sunshine for input + control (fire-and-forget START_A/B handshake). |
| `InputEncoder`, `InputCrypto` | Browser JSON input events → binary Limelight input packets → AES-128-GCM encryption (per-session key from the RTSP handshake). |
| `HostAudioSink` | Host-side audio capture normalization (the "Steam Streaming Speakers" sink volume is normalized — a 64% sink caused the infamous "stream at 60% volume"). |
| `ClipboardBridge` | Bidirectional text clipboard sync (gated to host == backend machine). |
| `StreamConfig` | Width/height/fps/bitrate/codec-mask/HDR/4:4:4 config passed to moonlight-common-c. |

Congestion handling (mobile networks) is **frontend-driven** (`app.js`), session-only — the user's stored settings are never touched — and combined with exponential IDR backoff on both sides (the fix for the "4G IDR spiral"). The **degradation ladder** (15 s minimum between rungs): every rung cuts the bitrate (−50% on the first, −30% after, floor 2 Mbps) and, while above 720p, also drops the resolution one rung and auto-enables SGSR upscaling so the drop stays discreet; then forces `webrtc-media-udp`; then caps at 60 fps; when every knob is at its floor, a plain transport restart. An **automatic upgrade** walks the same ladder back up (one knob per step: fps → transport → resolution → bitrate) after ~75 s without a congestion signal, with the stable period doubling (capped at 10 min) each time an upgrade flaps. Each step is applied through the **seamless standby relaunch** ([§5.6](05-Streaming-and-Transports.md#56-session-lifecycle--teardown-discipline)), so the quality change is not a visible reconnection. The whole ladder is **suspended while a share link is live** (`_sharePinsQuality` in `app.js`): every rung ping-pongs the owner between `/ws` and `/ws1`, and on the jittery link that made sharing interesting in the first place that became near-constant churn — with the guests carried along. A shared session keeps the profile it launched with until the owner changes it by hand.

## 3.4 Internet access, and the way in

Two pieces, one switch. `InternetAccessManager` decides what may be opened; `RendezvousClient` + `ControlTunnel` are how a browser actually arrives. Both follow `internet_access_enabled`, and that is deliberate: the line is an outbound connection announcing that this machine is online, and it shows a residential address on the socket — exactly the class of disclosure the consent describes, so refusing consent has to leave it closed like every other opening.

### `InternetAccessManager`

Runs the reachability consent as a state machine with a `phase` field driving the UI loader. **The consent is enforced end to end**: `internet_access_enabled` gates the per-session media-port mapping in the stream launch path (read live, so toggling applies to the next stream without a restart), and `stop()` removes every router mapping the manager owns the moment the consent is withdrawn.

1. `ensureIdentifiers()` — generates the 8-hex-char `unique_id`. It is **internal only**: it seeds the deterministic UPnP fallback port so two instances on one LAN never collide, and it is a public name for nothing.
2. Public IP detection: STUN first (the project's own server at `stream.{MW_DOMAIN}:3478`, public ones behind it), HTTP fallback (ipify/icanhazip); can be manual (`auto_ip_detection: false`). Feeds CGNAT detection and the NAT-hairpin test.
3. **Per-session UPnP media mapping** only — one port for the duration of a stream, removed when it ends. Ports 80 and 443 are never mapped: nothing needs them, since the interface arrives through the tunnel.
4. **Consent versioning**: a stored consent record carries the `mechanism` its wording described. A record predating the current wording parks the manager in phase `consent_required`, and nothing is opened until the user agrees again under the text they are actually shown.

**Bring-your-own-domain**: when `domain` in `settings.json` holds a valid FQDN, the manager keeps it verbatim and runs in a **network-only mode** — the zone, the certificate and the 443 forward on the router are the user's own. See [Settings Reference §7.5](07-Settings-Reference.md#75-bring-your-own-domain--certificate).

### `RendezvousClient` — the held line

`backend/src/network/RendezvousClient.{h,cpp}`. One outbound WebSocket to `https://stream.{MW_DOMAIN}` (`MW_RENDEZVOUS_URL` overrides), held open for as long as the consent stands. **The line is the reachability**: while it is up this machine is reachable at `https://stream.{MW_DOMAIN}/{rendezvous_id}`; while it is down it simply is not. Nothing is published, nothing is polled.

- `ensureIdentity()` draws a `rendezvous_token` (the ownership credential, of which the server stores only an HMAC) and a `rendezvous_id` — 26 Crockford base32 characters, a **locator, not a secret** ([Infrastructure §10.7](10-Infrastructure-Stack.md#107-the-introduction-server-mw-rendezvous)). A malformed stored value is *folded* before being redrawn: a value differing only by case or grouping is the same identifier, and redrawing it would abandon a claim still held.
- `entryUrl()` is **empty until the claim is accepted**. An identifier nobody acknowledged is an address that answers to nobody, and handing it to the tray menu or the admin page would look like success.
- Keep-alives, a watchdog, and reconnect with backoff. `onlineChanged` is what entry points follow; `isOnline()` is what the sharing board reports as `remote_reachable`.
- Only signalling crosses it — no media, no input, no credentials.

### `ControlTunnel` — the application, carried

`backend/src/server/ControlTunnel.{h,cpp}`. The instance publishes no name and opens no web port, so the browser cannot make an HTTP request to it. It loads the bootstrap from the introduction server instead, and that page opens a WebRTC connection straight here; the interface, the REST API and the streaming signalling all travel over it.

- **Not a second web server.** Requests off the tunnel go through `HttpServer::serveRequest()` — same router, same access decision. The one deliberate difference is that such a request never obtains the local-address exemption, and that rule lives inside `serveRequest()` so a future caller cannot forget it.
- **Bound by MW-BIND-v1.** The signalling is relayed by the introduction server, which is precisely the party the binding exists to keep out of the middle. A browser at the login screen holds no session yet, so it *presents* its key and the host signs against the key it was shown — which proves no peer was substituted, not who is holding the browser. Authorisation is still the PIN ([Security §6.6](06-Security.md#66-the-entry-identifier-and-mw-bind-v1)).
- Created unconditionally at startup and inert until the line announces a browser — and the line only runs with consent.

## 3.5 Startup sequence (`main.cpp`)

1. Qt app + icon, message handler → `Logger`, `CrashHandler::install` (Windows minidumps).
2. `loadEnvFile()` (`.env` next to exe, else project root; supports multi-line PEM values) then `applyEmbeddedEnvDefaults()` (CI-baked `MW_*` fallbacks).
3. CLI parse (`--port`, `--log`, `--ws-port`, `--autostart`, `--stream-worker` — the last one re-enters as a stream child process and skips everything below).
4. **Force Qt TLS backend to OpenSSL** (Windows Schannel cannot import PEM keys → a user-supplied certificate would silently be dropped in favour of the self-signed one).
5. `AppSettings` + `seedDocumentedDefaults()`; **single-instance `QLockFile`** — a second launch asks the running instance to focus the admin page (`/api/local/focus`) and exits 0, *or* stays alive as a **tray-only client** when the instance holding the lock has no desktop to draw on (Windows service in session 0, systemd unit) — see [Installers §9.4](09-Installers-and-Packaging.md#94-shared-runtime-behaviors).
6. `HttpServer` + domain/cert config; `ComputerManager.init()`; `IdentityManager` (RSA identity); eager OpenSSL init (avoids a libdatachannel DTLS init race).
7. `AuthManager` (+ first-boot certificate token), route registration (health/update/hostname/auth/hosts/start/quit/system).
8. Relay lifecycle wiring: take-over logic, revoked-session kill-switch, deferred start (see [Architecture §2.2](02-Architecture.md)).
9. `server.start()` (port fallback ranges), persist actual ports, `InternetAccessManager` wiring (rebind callback, `Provisioning::applyOnce`, auto-start if enabled).
10. `RendezvousClient` (started when the consent stands) and `ControlTunnel` (created unconditionally, inert until the line announces a browser).
11. Desktop-shortcut self-heal, `ControlChannel`, `TrayManager`, browser auto-open (`/setup` on first run for macOS/Linux, `/admin` otherwise; suppressed with `--autostart` or headless). The `/admin` open waits up to ~6 s for the line so it can use the remote entry link with a single-use host key, and falls back to loopback — which is what a machine with no internet does immediately.

Step 0, before `QApplication` even exists: `selectHeadlessPlatform()` swaps the xcb QPA plugin for **offscreen** when Linux offers no display server, so the same binary runs unchanged on a server or in a container (see [Installers §9.3.1](09-Installers-and-Packaging.md#headless-server-installs--931)). The operator CLI (§3.7) branches right after `AppSettings`, before the instance lock.

## 3.6 Data on disk

All under `QStandardPaths::AppDataLocation` (e.g. `%APPDATA%\MoonlightWeb\MoonlightWeb\` on Windows):

| File | Content |
|---|---|
| `settings.json` | All server settings — see [Settings Reference](07-Settings-Reference.md) |
| `sessions.json` | Persisted auth sessions (SHA-256 token hashes only) |
| `logs/moonlightweb.log` | Rolling log (all Qt messages captured) |
| `logs/moonlightweb-worker-<pid>.log` | One per `--stream-worker` child (a shared file would interleave and race on rotation) |
| `crashes/*.dmp` | Windows minidumps |
| `provisioning.status.json` | The live checklist the installer and the `/setup` wizard both drive, plus the `admin_url` the installer's post-install action opens |
| `moonlightweb.lock` | Single-instance lock |
| `moonlightweb-tray.lock` | Tray-client lock — one tray per desktop session, whoever owns the server |

## 3.7 Headless operator CLI

On a server there is no browser on the machine, and the admin API is localhost-only — so the two ends never meet. Three flags close the gap by driving the **already-running** instance through the very endpoints the admin page calls, printing the result as text and exiting. They branch **before** the single-instance `QLockFile` (they are queries against another process, not a second server) and route the log echo to stderr so stdout carries only the report.

| Flag | Endpoint(s) | Prints |
|---|---|---|
| `--status` | `server/status`, `setup/status`, `auth/status`, `internet/status`, `internet/upnp-probe` | Loopback/LAN URLs, the remote entry link (and whether its line is up — an address that exists but is not being held is a link that would fail), access PIN, Sunshine state, and — while Internet Access is off — whether a UPnP router answered or a port forward has to be typed in by hand |
| `--new-pin` | `POST /api/admin/pin/generate` | A fresh access PIN. Deliberately the non-revoking endpoint (`/api/auth/regenerate` destroys every session) |
| `--enable-internet [--yes]` | `POST /api/internet/enable` | Prints the consent text, requires `yes` on a TTY (or `--yes`), sends that exact text as `consent_message` so the consent record keeps what was shown, then the router verdict |

The loopback port is read from this user's `settings.json`, then falls back to 443 — running the CLI as a different user than the service (a `sudo`-less `moonlightweb --status` against a root-owned unit) reads a different file, or none. Peer verification is off for these calls: the certificate on 127.0.0.1 is the self-signed LAN one, and no certificate authenticates a loopback socket better than the kernel already does.

---

[← Architecture](02-Architecture.md) · [Home](Home.md) · [Next: Frontend →](04-Frontend.md)
