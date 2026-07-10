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
| | `routes/` | `AuthRoutes`, `HostRoutes`, `SystemRoutes` — see [REST API](08-REST-API.md) |
| `src/backend/` | `ComputerManager` | Host inventory: mDNS discovery, polling, box-art fetching, persistence. Polling is **suspended while a relay is active** (polling Sunshine's HTTP server mid-stream can wedge it). |
| | `NvHTTP`, `NvComputer`, `NvApp`, `NvAddress` | Async GameStream HTTP client + host/app data model |
| | `NvPairingManager`, `IdentityManager` | GameStream pairing (challenge-response) + persistent RSA client identity |
| | `SunshineInstaller`, `SunshineRestClient` | On-demand Sunshine install; REST pairing (`/api/pin`) for the wizard. Sunshine liveness is probed on **port 47989** (pgrep is unreliable on macOS). |
| `src/streaming/` | see [§3.3](#33-streaming-layer) | The streaming bridge |
| `src/network/` | `InternetAccessManager`, `PdnsClient`, `StunClient`, `UPNPClient`, `AcmeClient`, `GeoIpService`, `UpdateChecker` | Internet access orchestration (see [§3.4](#34-internet-access)) |
| `src/common/` | `Logger`, `CrashHandler`, `Types.h`, `MacActivity` | File logging (Qt messages captured via `qInstallMessageHandler`), Windows minidumps, shared HTTP types, macOS activity assertions |
| `TrayManager`, `Autostart` | | Tray icon + login-item/autostart registration (exit code 0 = voluntary quit, supervisors don't restart) |

## 3.2 HTTP server

`HttpServer` (`src/server/HttpServer.h`) is a hand-rolled Qt TCP/SSL server (no QtHttpServer dependency):

- **Two listeners**: plain HTTP (redirects to HTTPS) and HTTPS. An optional **secondary HTTPS listener** exists for *port parity*: when another instance behind the same NAT owns external :443, this instance adds a listener on its deterministic fallback port for the public domain while the primary keeps serving localhost/LAN (so an open admin page never loses its origin).
- **`Connection: close`** after every response — this is why `ConnectionGuard`'s flood threshold is generous (a single page load is dozens of connections).
- **WebSocket upgrades** are detected and raw-proxied to internal WS servers: signaling (48001), stream relay (48002), control channel (48003). The proxy cleanup lambda uses a `shared_ptr` guard (a `bool*` guard historically caused a use-after-free during `~QSslSocket` on macOS).
- **Async routes**: handlers may take a `ResponseCallback` and answer later (30 s timeout); sockets pending an async answer are tracked in `m_PendingAsyncSockets`.
- **Auth enforcement**: every `/api/*` request except `health`, `server/hostname` and `auth/*` requires localhost or a valid `mw_session` cookie; `/api/admin/*` additionally requires localhost (or a host session). 401s are reported to `ConnectionGuard`.

`CertManager` resolves the TLS certificate in priority order: explicit `cert_pem`/`cert_key` from settings (each is *either an env-var name or a file path*), then file scan, then **self-signed generation** for LAN use. A CN-matching check prevents serving a stale cert after the domain changed. `reloadTls()` swaps certs hot after ACME issuance.

## 3.3 Streaming layer

| Class | Role |
|---|---|
| `StreamSession` (`Session.cpp`) | Ephemeral orchestrator for one `/start` request: launches/resumes the app on Sunshine (per-browser `uniqueid`, `s_ActiveUniqueIds` registry decides launch-vs-resume), builds the `StreamConfig`, creates the shim + the relay for the chosen transport, answers the HTTP request with `{ws_url, transport_chain, transport_index, negotiated codec…}`, then self-deletes once streaming runs. AV1 is force-falled-back to H.264 here when needed. |
| `MoonlightShim` | The bridge into `moonlight-common-c` (`LiStartConnection` + decoder/audio/connection callbacks). Receives decoded-protocol H.264/HEVC/AV1 access units and Opus packets on moonlight's threads and hands them to the active relay. `stopConnection()` must always run **before** relay destruction (UAF otherwise). |
| `SignalingServer` | Per-session WebSocket server for SDP/ICE. Only advertises the private LAN ICE candidate to clients detected as local (never leaks the LAN IP to internet peers). |
| `DataChannelRelay` | **webrtc-dc** transport: libdatachannel PeerConnection with video/audio/input DataChannels. Video DC is *ordered* with `maxRetransmits=3`; frames carry a `frameId` (gap detection triggers backend IDR); a 256 KB high-watermark with keyframe-priority avoids blocking; IDR requests are throttled/coalesced (250–500 ms cooldown, exponential backoff under congestion). |
| `MediaTrackRelay` | **webrtc-media** transport: native RTP H.264 track + Opus track (browser jitter buffer, FEC/PLC). H.264-only. Sends from the capture thread, dynamic RTP timestamps, proactive IDR every 250 ms until the first client PLI-equivalent (browsers send no PLI over this path). |
| `StreamRelay` | **wss** transport: everything multiplexed over one WebSocket (worst network conditions, always works). Runs on a dedicated thread. |
| `FrameSender` | Dedicated send thread used by the DC relay so a slow SCTP send never blocks the moonlight callback thread. |
| `EnetControlStream` | Reliable ENet channel to Sunshine for input + control (fire-and-forget START_A/B handshake). |
| `InputEncoder`, `InputCrypto` | Browser JSON input events → binary Limelight input packets → AES-128-GCM encryption (per-session key from the RTSP handshake). |
| `HostAudioSink` | Host-side audio capture normalization (the "Steam Streaming Speakers" sink volume is normalized — a 64% sink caused the infamous "stream at 60% volume"). |
| `ClipboardBridge` | Bidirectional text clipboard sync (gated to host == backend machine). |
| `StreamConfig` | Width/height/fps/bitrate/codec-mask/HDR/4:4:4 config passed to moonlight-common-c. |

Congestion handling (mobile networks): a **degradation ladder** reduces bitrate by −30%, then switches transport family, then caps at 60 fps (floor 2 Mbps), session-only — combined with exponential IDR backoff on both sides. This was the fix for the "4G IDR spiral".

## 3.4 Internet access

`InternetAccessManager` orchestrates the one-click public exposure (state machine with a `phase` field driving the UI loader):

1. `ensureIdentifiers()` — generates the 8-hex-char `unique_id` (rejected if it collides with reserved labels: apex, `www`, `api`, `stats`, `stream`, `ns1/ns2`, `mail`, `_*`), computes `domain`.
2. Public IP detection: STUN first, HTTP fallback (ipify/icanhazip); can be manual (`auto_ip_detection: false`).
3. **Ownership claim**: a `_owner.<uid>` TXT record must match this instance's `owner_token` before the A record is touched — two instances can never clobber each other's subdomain. Changing `unique_id` releases the old subdomain first.
4. A-record create/update via `PdnsClient`; every registration appends a JSONL entry to a **consent audit log** (exact agreement text, timestamp, entry point).
5. **ACME certificate** via the native `AcmeClient` (DNS-01 challenge written through PDNS; ZeroSSL DV90 with EAB env credentials, else Let's Encrypt). Auto-renew below 30 days; `certificateChanged` triggers a hot TLS reload.
6. **UPnP mapping** with *port parity* (external == internal, always): if another device owns the preferred external port, a deterministic FNV-1a fallback port derived from `unique_id` is chosen and the HTTPS listener is extended to it via the rebind callback. Never evicts another device's mapping. CGNAT is detected and surfaced.
7. Periodic checks every 5 min (IP change, DNS resolution ~24h, cert expiry); NAT-hairpin reachability is tested to decide whether the host's own browser can use the public URL.

## 3.5 Startup sequence (`main.cpp`)

1. Qt app + icon, message handler → `Logger`, `CrashHandler::install` (Windows minidumps).
2. `loadEnvFile()` (`.env` next to exe, else project root; supports multi-line PEM values) then `applyEmbeddedEnvDefaults()` (CI-baked `MW_*` fallbacks).
3. CLI parse (`--port`, `--log`, `--ws-port`, `--autostart`).
4. **Force Qt TLS backend to OpenSSL** (Windows Schannel can't import ACME PEM keys → would serve the self-signed cert on the public domain).
5. `AppSettings` + `seedDocumentedDefaults()`; **single-instance `QLockFile`** — a second launch asks the running instance to focus the admin page (`/api/local/focus`) and exits 0.
6. `HttpServer` + domain/cert config; `ComputerManager.init()`; `IdentityManager` (RSA identity); eager OpenSSL init (avoids a libdatachannel DTLS init race).
7. `AuthManager` (+ first-boot certificate token), route registration (health/update/hostname/auth/hosts/start/quit/system).
8. Relay lifecycle wiring: take-over logic, revoked-session kill-switch, deferred start (see [Architecture §2.2](02-Architecture.md)).
9. `server.start()` (port fallback ranges), persist actual ports, `InternetAccessManager` wiring (rebind callback, `Provisioning::applyOnce`, auto-start if enabled).
10. Desktop-shortcut self-heal, `ControlChannel`, `TrayManager`, browser auto-open (`/setup` on first run for macOS/Linux, `/admin` otherwise; suppressed with `--autostart` or headless).

## 3.6 Data on disk

All under `QStandardPaths::AppDataLocation` (e.g. `%APPDATA%\MoonlightWeb\MoonlightWeb\` on Windows):

| File | Content |
|---|---|
| `settings.json` | All server settings — see [Settings Reference](07-Settings-Reference.md) |
| `sessions.json` | Persisted auth sessions (SHA-256 token hashes only) |
| `logs/moonlightweb.log` | Rolling log (all Qt messages captured) |
| `crashes/*.dmp` | Windows minidumps |
| Internet-access audit log (JSONL) | One entry per DNS A-record registration, with consent record |
| ACME artifacts (`letsencrypt/…`) | Issued cert/key files referenced from `settings.json` |
| `moonlightweb.lock` | Single-instance lock |

---

[← Architecture](02-Architecture.md) · [Home](Home.md) · [Next: Frontend →](04-Frontend.md)
