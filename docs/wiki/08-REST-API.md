[← Settings Reference](07-Settings-Reference.md) · **REST API** · [Next: Installers & Packaging →](09-Installers-and-Packaging.md)

---

# 8. REST API & WebSocket surfaces

Everything the frontend (or any client) can call. Routes are registered in `backend/src/main.cpp` and `backend/src/server/routes/{Auth,Host,System}Routes.cpp` on the `RestRouter` (exact-match + `:param` path segments; sync handlers return an `HttpResponse`, async handlers receive a `ResponseCallback`, 30 s timeout).

**Access levels**

- 🌐 *public* — no auth (also exempted from the 401 guard: `health`, `server/hostname`, `auth/*`).
- 🔑 *session* — localhost, or a valid `mw_session` cookie.
- 🏠 *admin* — localhost or a **host session** only (`/api/admin/*` returns 403 otherwise; same for setup/internet/system routes which are admin-gated in their handlers).
- 🔒 *loopback* — strictly `req.isLocal`.

## 8.1 Health & server info

| Method & path | Access | Description |
|---|---|---|
| `GET /api/health` | 🌐 | `{status, version}` liveness probe. |
| `GET /api/server/hostname` | 🌐 | Server hostname + OS (used for display and self-stream detection). |
| `GET /api/server/status` | 🔑 | Version + actual HTTP/HTTPS ports. |
| `GET /api/update/check` | 🔑 | Cached GitHub-Releases check: `{current, latest, update_available, download_url}` for this OS/arch; stale cache refreshes in background. |
| `POST /api/local/focus` | 🔒 | Second-launch IPC: redirect a connected tab to `/admin` via the control channel, else report `delivered:false` (caller opens a browser). |

## 8.2 Authentication (`AuthRoutes.cpp`)

| Method & path | Access | Description |
|---|---|---|
| `POST /api/auth/validate` | 🌐 | Body `{pin}` or certificate upload → sets `mw_session` cookie. Rate-limited (remaining attempts / lockout seconds in the response). Consumed PIN auto-regenerates. |
| `GET /api/auth/status` | 🌐 | Session validity, `is_host`, whether a PIN exists, cert-auth enabled. |
| `POST /api/auth/regenerate` | 🏠 | New PIN + invalidate all sessions. |
| `POST /api/admin/pin/generate` / `POST /api/admin/pin/clear` | 🏠 | Manage the PIN. |
| `GET /api/auth/sessions` | 🏠 | Sessions table (opaque token hash ids, IP, geo, machine name, streaming flag). |
| `POST /api/auth/sessions/revoke` | 🏠 | Revoke by opaque id; a streaming session's relay is torn down immediately. |
| `GET /api/admin/certificate/download` / `POST /api/admin/certificate/regenerate` | 🏠 | Certificate-file auth token. |
| `POST /api/auth/host-key` | 🌐 | Redeem `?mwk=` host key → host session; rotates the key and rewrites entry points. |

## 8.3 Hosts & streaming (`HostRoutes.cpp` + `main.cpp`)

| Method & path | Access | Description |
|---|---|---|
| `GET /api/hosts` | 🔑 | Known hosts with state, pair status, apps. |
| `POST /api/hosts/scan` | 🔑 | Trigger an ephemeral mDNS scan. |
| `POST /api/hosts/manual` | 🔑 | Add a host by IP/hostname. |
| `DELETE /api/hosts/:id` | 🔑 | Forget a host. |
| `POST /api/hosts/:id/wol` | 🔑 | Wake-on-LAN. |
| `POST /api/hosts/:id/pair` (async) / `GET /api/hosts/:id/pair` | 🔑 | Start pairing (returns the PIN to type into Sunshine) / poll status. Fully event-driven (no nested loop). |
| `GET /api/hosts/:id/apps` (async) | 🔑 | App list (with box-art fetched in background). |
| `GET /api/hosts/:id/appasset` | 🔑 | Box-art image. |
| `POST /api/hosts/:id/start` (async) | 🔑 | **The launch route.** Body: `appId` + per-browser overrides (`video_codec`, `stream_bitrate/height/fps/aspect`, `hdr_enabled`, `chroma_444_enabled`, `gaming_mode`, `mute_host_audio`, `video_enhancement`, `low_audio`, `client_uniqueid`, `transport_mode`, `transport_index`). Performs take-over, launch/resume on Sunshine, relay creation. Response: `{ws_url, transport_chain, transport_index, negotiated codec, codecOverridden…}`. |
| `POST /api/hosts/:id/quit` (async) | 🔑 | Stop the stream (ownership-guarded by `client_uniqueid`) + quit the app on Sunshine. |

## 8.4 Admin, settings, internet, setup, system (`SystemRoutes.cpp`)

| Method & path | Access | Description |
|---|---|---|
| `GET /api/admin/settings` / `POST /api/admin/settings` | 🏠 | Read/write server settings (ports, transport mode, internet toggle…). Port changes rebind live. |
| `GET /api/settings/streaming` / `POST /api/settings/streaming` | 🔑 | Server-side streaming defaults (the browser seeds its localStorage from these). |
| `GET /api/internet/status` | 🔑 | Full Internet-Access state: phase, domain, public IP, external ports, hairpin, cert expiry, last error. |
| `POST /api/internet/enable` | 🏠 | Opt-in (records consent `{message, source}`) + start the manager. |
| `POST /api/internet/disable` / `POST /api/internet/refresh` / `POST /api/internet/renew-cert` | 🏠 | Stop / force IP+DNS re-check / force ACME renewal. |
| `GET /api/setup/status` / `POST /api/setup/apply` | 🏠 | First-run wizard state + apply (internet consent, Sunshine install/pair) with live checklist. |
| `POST /api/system/open-screen-recording` | 🏠 | macOS: open the Screen-Recording TCC pane (Sunshine permission). |
| `POST /api/system/start-sunshine` / `POST /api/system/stop-sunshine` | 🏠 | Control the local Sunshine (liveness probed on port 47989). |

## 8.5 WebSocket surfaces

All are reached through the **single HTTPS port** — `HttpServer` recognizes the `Upgrade` handshake and raw-proxies to internal WS servers:

| Path | Internal port (default) | Purpose |
|---|---|---|
| `/ws` | 48001 (`--ws-port`) | **Signaling** for WebRTC: JSON SDP offers/answers + ICE candidates, per-session. |
| `/ws/stream` | 48002 | **Legacy WSS transport**: binary multiplexed video/audio/input when WebRTC can't connect. |
| `/ws/control` | 48003 | **Control channel**: every open tab holds one; used to redirect a tab to `/admin` on a second app launch (single-tab dedup). |

The frontend always builds WS URLs from `window.location.host` (page origin) — required for non-default external ports (port parity / multi-instance NAT).

## 8.6 Static file serving

Anything not matching `/api/*` or a WS upgrade is served by `StaticFileHandler` from the bundled `frontend/` directory (MIME-typed, `Cache-Control: no-cache` on text assets so `VersionGuard` + reload always pulls fresh code). SPA paths (`/admin`, `/settings`, `/setup`) serve `index.html`.

---

[← Settings Reference](07-Settings-Reference.md) · [Home](Home.md) · [Next: Installers & Packaging →](09-Installers-and-Packaging.md)
