[← Overview](01-Overview.md) · **Architecture** · [Next: Backend →](03-Backend.md)

---

# 2. Architecture

## 2.1 System diagram

```
   BROWSER (any device)                  MoonlightWeb SERVER (C++/Qt)            Sunshine HOST
 ┌───────────────────────────┐      ┌──────────────────────────────┐      ┌──────────────────┐
 │  Web App (Vanilla JS)     │ REST │  HTTP :80 → HTTPS :443       │HTTPS │  GameStream API  │
 │  Hosts / apps / pairing   │◄────►│  Static files + REST API     │◄────►│  /serverinfo     │
 │  Video : WebCodecs+WebGPU │      │  Proxy to Sunshine           │      │  /applist/launch │
 │  Audio : Opus/AudioWorklet│WebRTC│  ┌────────────────────────┐  │ RTSP │  /pair           │
 │  Input : kbd/mouse/gamepad│◄════►│  │  moonlight-common-c    │  │ RTP  │  GPU encoder     │
 │  Video Enhancement (GPU)  │ (WSS │  │  RTSP/RTP/ENet → Relay │  │◄════►│  (NVENC/AMF/QSV) │
 └───────────────────────────┘ fall)└──────────────────────────────┘ UDP  └──────────────────┘
        ▲ DNS (sub-domain) + TLS
 ┌──────┴────────────────────────────────────────────┐
 │  Self-hosted DNS stack (Docker, separate machine) │  ← maintained by the author,
 │  dnsdist :53 · PowerDNS (API) · Caddy :80/:443    │    or host your own
 └───────────────────────────────────────────────────┘
```

The MoonlightWeb server plays three roles at once:

1. **Web server** — serves the frontend (static files) and a REST API over HTTPS.
2. **GameStream client** — embeds `moonlight-common-c` and speaks NvHTTP/RTSP/RTP/ENet to Sunshine, exactly like moonlight-qt would.
3. **Streaming bridge** — re-encapsulates the decoded-protocol media (H.264/HEVC/AV1 frames, Opus packets) and input into browser-reachable transports: WebRTC DataChannels, WebRTC RTP media tracks, or a WSS relay.

The **DNS stack is decoupled**: it runs on a separate machine and only provides subdomain registration + the marketing site. MoonlightWeb talks to it through a REST API (`MW_PDNS_URL`/`MW_PDNS_TOKEN`).

## 2.2 The three-party exchange in detail

### Browser ↔ MoonlightWeb

| Channel | Purpose |
|---|---|
| **HTTPS REST** (`/api/...`) | Hosts/apps/pairing, settings, admin, auth, internet access. Full reference in [REST API](08-REST-API.md). |
| **WSS `/ws` (signaling)** | WebRTC SDP/ICE exchange, proxied by the HTTPS server to an internal signaling WebSocket server (default port 48001). The WS URL is always anchored on `window.location.host` (the page origin), never on a backend-computed host — this is what keeps non-default external ports working. |
| **WebRTC PeerConnection** | The stream itself: video/audio/input DataChannels, or RTP media tracks (see [Streaming & Transports](05-Streaming-and-Transports.md)). |
| **WSS `/ws/stream`** | Legacy/fallback full-stream relay (video+audio+input over one WebSocket) when WebRTC cannot connect. |
| **WSS `/ws/control`** | Tiny control channel every open tab keeps: used for single-tab dedup (a second app launch redirects an existing tab to `/admin` instead of opening a duplicate). |

All WebSocket surfaces share the single HTTPS port: the HTTP server detects the `Upgrade` header and proxies the socket to the right internal WS server. One public port (443 by default) carries everything.

### MoonlightWeb ↔ Sunshine

| Channel | Purpose |
|---|---|
| **NvHTTP** (HTTP :47989 / HTTPS :47984) | `serverinfo`, `applist`, `launch`/`resume`/`cancel`, pairing (challenge-response, client TLS cert). Implemented in `backend/src/backend/NvHTTP.cpp` with fully **async** APIs — one HTTPS request per host at a time, no nested event loops (a historical crash source). |
| **RTSP** (:48010) | Stream negotiation after launch, driven by `moonlight-common-c` inside `MoonlightShim`. |
| **RTP** (UDP 47998/47999/48000) | Video/audio payload from Sunshine's GPU encoder. |
| **ENet control** (:47999) | Encrypted input channel (AES-128-GCM) + control messages (IDR requests, connection status). |
| **mDNS :5353** | LAN discovery. The server never binds 5353 permanently (it would conflict with Sunshine's own mDNS on the same machine); scans are ephemeral. |
| **Sunshine REST API** (:47990) | Used only by the setup wizard/installer to create credentials and send the pairing PIN (`/api/pin`). |

### End-to-end launch sequence

```
Browser                        MoonlightWeb                        Sunshine
   │  POST /api/hosts/:id/start     │                                  │
   │───────────────────────────────►│  (take-over of any live relay)   │
   │                                │  NvHTTP /launch or /resume       │
   │                                │─────────────────────────────────►│
   │                                │  RTSP SETUP/PLAY (moonlight-c)   │
   │                                │◄────────────────────────────────►│
   │   200 {ws_url, transport_chain,│                                  │
   │        transport_index, ...}   │                                  │
   │◄───────────────────────────────│                                  │
   │  WSS /ws  (SDP offer/answer,   │                                  │
   │   ICE candidates)              │                                  │
   │◄──────────────────────────────►│                                  │
   │  WebRTC DTLS/SCTP or RTP  ◄════╪══► relay ◄═══ RTP/ENet ═════════►│
   │  (video, audio, input)         │                                  │
```

Key invariants (hard-won, do not regress):

- **One stream at a time.** `moonlight-common-c` is a process-global singleton and the signaling port is fixed. A second browser launching does a **take-over**: the live client is notified (`{"type":"takeover"}`), its relay is torn down *without* cancelling the Sunshine session, and the newcomer `/resume`s it.
- **The response to `/start` is sent before ICE connects.** Connection failures are only observable client-side, so the **browser drives the transport fallback chain** by relaunching with `transport_index + 1`.
- **Teardown is serialized.** A new session's `start()` is deferred until the previous relay graph is fully destroyed (`QObject::destroyed`), because the signaling port and the moonlight singleton are only freed then.
- Sessions are keyed per-browser by a `client_uniqueid` (hex, localStorage) so one browser's quit/relaunch never cancels another client's Sunshine session, and a page reload `/resume`s its own session.

## 2.3 Technology stack & rationale

| Choice | Rationale |
|---|---|
| **C++17 + Qt 6.11** (backend) | `moonlight-common-c` is C; Qt provides the cross-platform event loop, networking (QSslSocket), JSON, tray icon, and mature TLS handling on all three OSes with a single codebase. Qt 6.11 is the tested baseline; the **OpenSSL TLS backend is forced** on Windows (Schannel cannot load ACME PEM keys). |
| **`moonlight-common-c`** (submodule) | The canonical, battle-tested GameStream protocol core used by every Moonlight client. Reimplementing RTSP/RTP/ENet/FEC would be folly. |
| **`libdatachannel`** (submodule) | Lightweight C++ WebRTC implementation (DataChannels *and* RTP media tracks) without pulling the enormous libwebrtc. Built statically via CMake `add_subdirectory`. |
| **`qmdnsengine`**, **`miniupnpc`** (submodules) | mDNS discovery and UPnP port mapping, both small and embeddable. |
| **OpenSSL 3** | Pairing crypto (AES/RSA per GameStream), input encryption (AES-128-GCM), ACME JOSE signing. Bundled on Windows (`backend/libs/windows/`). |
| **Vanilla JS, no framework, no build step** (frontend) | The app is served by an embedded C++ web server: zero build tooling means the server ships plain files and contributors need only a browser. ES6 modules give structure; Prettier+ESLint+Vitest+tsc(advisory) give quality without a bundler. |
| **WebCodecs + WebGPU/Canvas** (video) | WebCodecs exposes the browser's hardware H.264/HEVC/AV1 decoders with frame-level control (latency!); rendering to canvas allows the WebGPU enhancement pipeline. A `<video>`-sink alternative exists for HDR (see [Transports](05-Streaming-and-Transports.md)). |
| **AudioWorklet + WebCodecs AudioDecoder** (audio) | Opus decode on the native decoder, playback on the real-time audio thread with an adaptive jitter buffer and WSOLA time-stretch. |
| **CMake** (single build system) | qmake was removed 2026-06-28. One `CMakeLists.txt` covers Windows x64/ARM64 (MSVC/Ninja), Linux, macOS, plus tests, coverage and `compile_commands.json`. |
| **PowerDNS + dnsdist + Caddy in Docker** (DNS) | Official images, one process per container, REST API for record management — the smallest self-hostable authoritative-DNS-with-API stack. See [PowerDNS Stack](10-PowerDNS-Stack.md). |
| **Server-side settings** (`settings.json`) | The server is the single source of truth (no accounts/multi-user); per-browser *streaming* preferences live in `localStorage`, everything else server-side. |

## 2.4 Repository layout

```
moonlight-web/
├── backend/                    # C++/Qt server
│   ├── CMakeLists.txt          # single canonical build (also embeds MW_* CI secrets)
│   ├── src/
│   │   ├── main.cpp            # composition root: startup, routes, session lifecycle
│   │   ├── backend/            # GameStream client: NvHTTP, pairing, discovery
│   │   ├── server/             # HTTP/HTTPS server, REST router, auth, settings, certs
│   │   │   └── routes/         # AuthRoutes, HostRoutes, SystemRoutes
│   │   ├── streaming/          # Session, relays (DC/media/WSS), shim, input, signaling
│   │   ├── network/            # InternetAccess: PDNS, STUN, UPnP, ACME, GeoIP, updates
│   │   └── common/             # Logger, CrashHandler, shared types
│   ├── tests/                  # Qt Test suites + coverage scripts
│   ├── third_party/            # git submodules (moonlight-common-c, libdatachannel, …)
│   ├── installer/              # Windows Inno Setup + macOS .pkg plugin
│   ├── packaging/              # systemd/launchd/Windows-service, linux .deb/.rpm
│   └── libs/windows/           # vendored OpenSSL 3 (headers + libs + DLLs)
├── frontend/                   # Vanilla JS web app (no build step)
│   ├── index.html              # single page: header + #main-content + footer
│   ├── js/                     # app.js + api/ audio/ i18n/ models/ stream/ ui/ util/
│   ├── css/                    # design tokens + per-view stylesheets
│   ├── locales/                # en/fr/zh runtime i18n catalogs (Tolgee-compatible)
│   └── test/                   # Vitest unit tests (jsdom)
├── deploy/powerdns/            # self-hosted DNS stack (Docker) + installer
├── website/                    # static landing page served by the DNS box's Caddy
├── scripts/                    # run-tests.sh (full TNR gate), build_stream_image.py
├── docs/                       # design docs, audits, screenshots, this wiki
└── .github/workflows/          # ci.yml, release.yml, build-asan.yml
```

## 2.5 Code architecture principles

- **Composition root in `main.cpp`** — all wiring (routes, relay lifecycle tracking, tray, internet access, control channel) happens there with lambdas capturing `QPointer`s; classes stay decoupled.
- **Event-driven, no nested event loops** — HTTP dispatch uses sync or **async routes with a `ResponseCallback`**; pairing and NvHTTP are fully asynchronous. A nested `QEventLoop` in the HTTP dispatch path historically caused a use-after-free crash and is banned.
- **Relay threading** — each relay runs its stream pumping off the main thread (dedicated relay thread); teardown ownership lives in `qApp`-context lambdas in `main.cpp` (a relay whose teardown lived elsewhere was never destroyed, producing 504s).
- **Frontend: one main view + overlays** — `hosts` is the single history-backed view; `admin`, `settings` and `streaming` are overlays with guard `pushState` (see `frontend/js/app.js` header comment).
- **Transports are pluggable** — a shared relay contract (`RelayBase.h`) and a frontend renderer abstraction (`VideoRenderer` + `createRenderer`) keep the 5 transport modes and the 3 render sinks orthogonal.

---

[← Overview](01-Overview.md) · [Home](Home.md) · [Next: Backend →](03-Backend.md)
