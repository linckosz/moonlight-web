[← Settings Reference](07-Settings-Reference.md) · **REST API** · [Next: Installers & Packaging →](09-Installers-and-Packaging.md)

---

# 8. REST API & WebSocket surfaces

Everything the frontend (or any client) can call. Routes are registered in `backend/src/main.cpp` and `backend/src/server/routes/{Auth,Host,Share,System,Log}Routes.cpp` on the `RestRouter` (exact-match + `:param` path segments; sync handlers return an `HttpResponse`, async handlers receive a `ResponseCallback`, 30 s timeout).

**Access levels**

- 🌐 *public* — no auth (also exempted from the 401 guard: `health`, `server/hostname`, `auth/*`).
- 🔑 *session* — localhost, or a valid `mw_session` cookie.
- 🏠 *admin* — `req.isLocal`, i.e. a **loopback peer, a host-key session** (the host's own browser, arriving through the entry link with a single-use key) **or a session that unlocked the remote admin password** — on the LAN, or from the internet when the owner allows it ([Security §6.2.1](06-Security.md#621-remote-admin-password-lan-internet-on-request)) — **and** the admin key on writes (below). `/api/admin/*` returns 403 otherwise; the setup/internet/system/local routes apply the same check inside their handlers.
- 🖥️ *host machine* — `req.isHostMachine`: 🏠 minus the password unlock. Reserved for what needs the local desktop or identifies the host itself (`local_key`, the first-run wizard).
- 🍪 *invited player* — the `mw_player` cookie bound to one live share activation ([§8.3b](#83b-session-sharing-shareroutescpp)). Not a session: it opens that player's own slot and nothing else, and is exempt from the 401 guard like 🌐.

**Request requirements** (enforced in `RequestGuard` + `HttpServer::processRequest`, before routing)

The address-based trust above is something a malicious page can borrow — it makes the victim's own browser issue the request, which then arrives from 127.0.0.1 like the admin UI's would. Every `/api/*` request is therefore screened first:

| Rule | Failure | Why |
|---|---|---|
| `Sec-Fetch-Site` must be `same-origin`/`none` (falling back to `Origin` vs `Host` on engines that lack it). Absent on both = non-browser client, allowed. | 403 `cross_site_request_blocked` | A page cannot forge either header, so this is what separates our frontend from any other site. |
| A non-empty body must be `Content-Type: application/json`. | 415 `unsupported_media_type` | Not CORS-safelisted, so a cross-origin write needs a preflight we never answer. |
| `Host` must be loopback, a private/LAN address, `*.local`, or a configured own-domain FQDN — **otherwise the request keeps working but loses 🔑/🏠 privileges**. | — | DNS rebinding makes the attacker same-origin with us (able to *read* the PIN, `local_key`, TLS key) but cannot change the name in `Host`. A request off the tunnel is judged by its arrival, not its `Host`, and never obtains the local exemption at all. |
| Writes (`POST`/`DELETE`) to 🏠 routes must carry `X-MW-Admin-Key`. Reads do not. | route's own 403 | A custom header cannot cross origins without a preflight. `GET /api/admin/token` serves the key to 🏠 callers only; it is regenerated at every server start, and `BackendClient` refetches + replays once on a 403. |

WebSocket upgrades are screened the same way on `Origin` (they bypass CORS entirely, and `/ws` carries input events).

## 8.1 Health & server info

| Method & path | Access | Description |
|---|---|---|
| `GET /api/health` | 🌐 | `{status, version, name, debug, verbose}` liveness probe (the instance's name, and the diagnostic modes a page follows). |
| `GET /api/server/hostname` | 🌐 | Server hostname + OS (used for display and self-stream detection). |
| `GET /api/server/status` | 🔑 | Version + actual HTTP/HTTPS ports. |
| `GET /api/update/check` | 🔑 | Cached GitHub-Releases check: `{current, latest, update_available, download_url}` for this OS/arch; stale cache refreshes in background. Plus `self_update: {supported, method, requires_host_confirmation}` — what this host can do about it unattended. |
| `POST /api/update/start` | 🔑 | Download + apply the update on the host, then relaunch. 409 when no update is available or one is already running. Deliberately **not** localhost-gated: the point is to update the host from wherever the user is. |
| `GET /api/update/status` | 🔑 | `{state, percent, version, requires_host_confirmation, error}`, `state ∈ idle\|downloading\|installing\|restarting\|failed`. Stops answering once the installer takes the server down — the client then waits for the new build on `/api/health`. |
| `POST /api/local/focus` | 🏠 | Second-launch IPC: redirect a connected tab to `/admin` via the control channel, else report `delivered:false` (caller opens a browser). |

## 8.2 Authentication (`AuthRoutes.cpp`)

| Method & path | Access | Description |
|---|---|---|
| `POST /api/auth/validate` | 🌐 | Body `{pin}` or certificate upload → sets `mw_session` cookie. Optional `remember` (default `true`): `false` gives a session-scoped cookie (no `Max-Age`) and an ephemeral session — 8 h idle TTL, never persisted (§6.3). Rate-limited (remaining attempts / lockout seconds in the response). Consumed PIN auto-regenerates. |
| `POST /api/auth/logout` | 🌐 | Destroys the caller's *own* session (raw cookie token only, so it can reach no other) and expires the cookie. Answers `{status, had_session}` — `had_session: false` for a stale or absent cookie, which is cleared anyway. A streaming session's relay is torn down as with any revocation. |
| `GET /api/auth/status` | 🌐 | Session validity, `has_session` (this browser holds a valid `mw_session` cookie — true even on localhost, where `authenticated` is granted by address), `is_localhost` (has admin), `is_host_machine`, `admin_unlock_available`, `remote_admin_enabled` + `remote_admin_internet` + `admin_password_set` (admin only), whether a PIN exists, cert-auth enabled. Refreshes the cookie, keeping an ephemeral session session-scoped. |
| `POST /api/auth/regenerate` | 🏠 | New PIN + invalidate all sessions. |
| `POST /api/admin/pin/generate` / `POST /api/admin/pin/clear` | 🏠 | Manage the PIN. |
| `POST /api/auth/admin-unlock` | 🔑 LAN | Body `{password}` → promotes this session to admin. Requires a LAN peer **and** a trusted `Host` (on the tunnel, the LAN address ICE settled on) — or, when `remote_admin_internet` is on, any peer that came through the rendezvous tunnel — plus an existing session, remote administration enabled **and** a password actually set (there is no built-in default); own rate-limit bucket, and one shared by every attempt from outside the LAN (401 `invalid_password`, 429 `rate_limited`, 403 `lan_only`/`not_configured`). Answers `ok` without spending an attempt when the caller is already an admin. |
| `POST /api/admin/password` | 🏠 | Body `{password}`, `{clear}`, `{enabled}` and/or `{internet}` — changes the remote admin password (min 8 chars, 400 `too_short`) or removes it (`{clear: true}`), turns remote administration on or off, and opens or closes its internet side. Also reachable from a terminal as `moonlightweb --set-admin-password` and `--clear-admin-password`. Any change revokes every session unlocked with the old password, except the caller's while the door still admits it from where it stands. |
| `GET /api/auth/sessions` | 🏠 | Sessions table (opaque token hash ids, IP, geo, machine name, streaming flag, `ephemeral`), newest first. `current` marks the caller's own session. |
| `POST /api/auth/sessions/revoke` | 🏠 | Revoke by opaque id; a streaming session's relay is torn down immediately. Refuses the caller's own session (409 `cannot_revoke_self`). |
| `GET /api/admin/certificate/download` / `POST /api/admin/certificate/regenerate` | 🏠 | Certificate-file auth token. |
| `POST /api/auth/host-key` | 🌐 | Redeem the host key (`#k=` from the owner's entry link, or `?mwk=` on a local address) → host session; rotates the key. |

## 8.3 Hosts & streaming (`HostRoutes.cpp` + `main.cpp`)

| Method & path | Access | Description |
|---|---|---|
| `GET /api/hosts` | 🔑 | Known hosts with state, pair status, apps. **No addresses**: `activeAddress`, `localAddress`, `remoteAddress`, `manualAddress` and `macAddress` are deliberately not serialized (`NvComputer::toJson`) — the browser only ever talks to this server, routes are keyed by `uuid`, and Wake-on-LAN is sent host-side, so the LAN topology has no reason to reach the page. Waking is advertised as the boolean `wakeSupported`. Persistence is unaffected (it uses `serialize(QSettings&)`). |
| `POST /api/hosts/scan` | 🔑 | Trigger an ephemeral mDNS scan. |
| `POST /api/hosts/manual` | 🔑 | Add a host by IP/hostname. |
| `DELETE /api/hosts/:id` | 🔑 | Forget a host. |
| `POST /api/hosts/:id/wol` | 🔑 | Wake-on-LAN. |
| `POST /api/hosts/:id/pair/start` / `POST /api/hosts/:id/pair` (async) | 🔑 | Start pairing (returns the PIN to type into Sunshine) / poll status. Fully event-driven (no nested loop). Starting is a POST because it acts on the host: a GET gets fetched by link-preview bots, prefetchers and URL-following antivirus, and those arrive as ordinary navigations that no cross-site check can tell apart from the user. |
| `GET /api/hosts/:id/apps` (async) | 🔑 | App list (with box-art fetched in background). |
| `GET /api/hosts/:id/appasset` | 🔑 | Box-art image. |
| `POST /api/hosts/:id/start` (async) | 🔑 | **The launch route.** Body: `appId` + per-browser overrides (`video_codec`, `stream_bitrate/height/fps/aspect`, `hdr_enabled`, `chroma_444_enabled`, `gaming_mode`, `mute_host_audio`, `video_enhancement`, `low_audio`, `mic_enabled` (the viewer's microphone — the response then says `mic:true`, the audio section offered sendrecv, or `mic:false` with `mic_why`: [§5.4](05-Streaming-and-Transports.md#54-audio-path)), `client_uniqueid`, `transport_mode`, `transport_index`; `ultra_codec:"pyrowave"`, sent beside `video_codec:"hevc"` by a browser whose user chose « PyroWave (Wired LAN only) » and that decodes it, asks a native Windows host that codes it (`nativeDisplay.pyrowave` on its card) for PyroWave on the DataChannel transport, SDR only — anything else streams the `video_codec`; the response's `ultra_codec:"pyrowave"` says the encoder that runs is PyroWave's) + the dual-stream pair `standby` / `session_slot` (a `standby:true` launch adds the second slot instead of taking over). Performs take-over, launch/resume on Sunshine, worker spawn + relay creation. Response: `{signalingUrl\|wsUrl, transport_chain, transport_index, transport_dropped, negotiated codec, codecOverridden, dual_supported…}` (`transport_dropped` names a transport that was requested by name but removed from the chain — today only `wss`, for a browser on the rendezvous; absent when the request was honoured), or `{status:"dual_unavailable"}` when the host refuses a second concurrent session. On a host whose app outlives the stream (backend capability `resumableApps`: Sunshine, Apollo), the session asks the host which app runs: the same app is resumed, nothing running is launched, another app answers **`409 {code:"app_running", runningAppId}`** — the browser then offers to quit it and launch, or to resume it. `quit_app_on_stop:true` (the Settings switch) makes a worker that ends on its own close the app as before. |
| `POST /api/hosts/:id/quit` (async) | 🔑 | Stop the stream (ownership-guarded by `client_uniqueid`). Body: `session_slot` targets one slot only (absent = every slot this `client_uniqueid` owns); `keep_host_session:true` **retires** the leg without the Sunshine `/cancel` — mandatory whenever a twin stream is still playing. On a host whose app outlives the stream (`resumableApps`), this is a **disconnect**: the app keeps running and guests keep playing, unless `quit_app:true` (sent when the viewer ticked *Quit the app when the stream stops*). Elsewhere it also quits the app on the host. |
| `GET /api/hosts/:id/running-app` (async) | 🔑 | `{currentGameId}` asked of the host now (0 = nothing runs), not read from the last poll, which pauses while a stream runs. The browser asks it before `/start` on a `resumableApps` host: another app running is a question for the viewer, and `/start`'s take-over would already have ended someone else's stream. `502` when the host does not answer. |
| `POST /api/hosts/:id/stop-session` (async) | 🔑 | Escape hatch, and the card's **Quit** button: drop every slot and cancel every live Sunshine session on this host, unscoped. For when no browser holds a stream view any more (tab closed, another device, players keeping the app alive). Answers `{status:"stopped", currentGameId}`, read back from the host once the cancels returned, so the card updates at once (non-zero: an app Sunshine could not close). |

## 8.3b Session sharing (`ShareRoutes.cpp`)

Owner routes need a normal session (any authenticated user — deliberately not admin-only). Player routes are exempt from the session gate and authenticate the device from the `mw_player` cookie the PIN buys. Everything answers **404** when `ShareManager::kSessionSharingEnabled` is false.

| Method & path | Access | Description |
|---|---|---|
| `GET /api/share/status` | 🔑 | The three player rows: `{slot, state ∈ off\|shared\|binded, name, permissions, access_level ∈ viewer\|gamer\|desktop\|full, streaming, ttl_secs, expires_at, devices[], last_refused?}` + `streaming` count + `remote_reachable`. `expires_at` is **null** on an unlimited invitation, never 0. `remote_reachable` is whether the rendezvous line is up **at this instant** — it never changes a link, it only lets the board say the machine is not answering right now. `?host=` names the host the board is about (the running stream's when absent), for three more fields: `feed_height` ∈ 720\|1080\|1440, the guests' picture the owner picked; `owner_height`, true when that host's guests watch it (the native host) rather than choosing their own; `shared_feed`, true when they watch one shared feed (the switch on, Windows). And the address menu: `link_addresses[]` (`{id: auto}`, then `{id, kind ∈ lan\|tailscale\|other, address, adapter}`), `link_address`, that host's pick (`auto` by default), and `link_address_missing`, true when the pick names a network this PC no longer has. |
| `POST /api/share/feed` | 🔑 | `{height ∈ 720\|1080\|1440}` — the guests' picture on a native host, one for every row, kept in `share.json` (1080 by default). A feed with guests on it is rebuilt at the new height under them; they follow within a second or two. **400** for any other height. |
| `POST /api/share/link-address` | 🔑 | `{host_uuid, link_address}` — the address that host's guest links carry: `auto`, `lan`, `tailscale` or `iface:<adapter>`, kept per host in `share.json`. A live share keeps its token and PIN; the board fetches its credentials again for the new link. **400** for no host, or an id the menu does not offer right now. |
| `POST /api/share/slots/:n/activate` | 🔑 | Mint a link + PIN, revoking the slot's previous pair. Body `{ttl_secs}`, plus `{host_uuid, app_id}` when no stream is running (the board opened cold from a host's ⋯ menu) — **400** for an unlisted lifetime or a missing host, **404** for an unknown host/app. With a stream up, the running host and app win and the body's are ignored. Returns `{url, pin, reach, local_only, …}`. |
| `POST /api/share/slots/:n/credentials` | 🔑 | The same `{url, pin, reach, local_only}` again, for an owner reopening the row on a live share. `{available:false}` when the process restarted since — only the digests are persisted. POST, not GET: it hands out a credential, so it goes through the CSRF/origin gate. |
| `POST /api/share/slots/:n/permissions` | 🔑 | `{gamepad, keyboardMouse, hid}`, accepted **at any time**, mid-stream included — the new policy is pushed down to the live worker. `hid` (wheels and radios sent as themselves) only counts with `gamepad`; granting or taking it mid-stream plugs the guest's devices in or pulls them out. **409** only when the slot has no activation. |
| `POST /api/share/slots/:n/ttl` | 🔑 | `{ttl_secs}` ∈ 3600 / 14400 / 28800 / 86400 / 172800 / 0 (unlimited). **400** otherwise. Shortening below the time already served expires the invitation on the spot. |
| `POST /api/share/slots/:n/name` | 🔑 | `{name}` — the row's label, 32 chars, empty restores "Player N". Player slots only: **404** for an owner slot. |
| `POST /api/share/slots/:n/deactivate` | 🔑 | Disconnect the player and revoke link, PIN and cookies. |
| `POST /api/share/player/pin` | 🌐 | `{token, pin}` → sets `mw_player` (HttpOnly, Secure, SameSite=Strict, the invitation's own lease). Rate-limited; 10 failures on one activation destroy it. **409** `already_bound` when the PIN is right but the invitation already has its one device — this does *not* count as a failure, and is surfaced to the owner on the board. |
| `GET /api/share/player/info?token=` | 🍪 | `{needs_pin:true}` without the cookie, but only while the invitation is still unbound; once it has spent its PIN on a device, a request without that device's cookie gets a **404** `bound_elsewhere`, with no PIN form, because every PIN it could offer would be answered `already_bound` anyway. The page says the invitation is already used and asks to reopen it in the browser that entered the PIN. With the cookie: `{machine_name, state, access_level, expires_at, app_name, cold_start, shared_feed}`, plus `feed_height` where the owner picks the guests' picture (a native host): the page then states it instead of offering a choice. `cold_start` says the guest's arrival would *launch* `app_name` rather than join something already up, resolved on the host the invitation is bound to. `shared_feed` says the guests watch one feed, so a browser that decodes no HEVC joins with `codec: "h264"` at once. **404** for a dead link. |
| `POST /api/share/player/join` | 🍪 | `{token, height ∈ 720\|1080\|1440, codec?: "h264"}` → starts the player's worker and answers like `/api/hosts/:id/start`. On a native host `height` is ignored: its guests watch the owner's `feed_height`. `codec: "h264"` is a browser that decodes no HEVC — on the shared feed, it turns the whole feed H.264, and the guests already on it come back in H.264 (`feedcodec` notice). Nothing running on the bound host is no longer fatal: an invitation opened cold **launches** the app it names — on *MoonlightWeb Virtual Display*, turned on first, at the guests' picture size. **409** `stream_in_progress` / `session_ended` / `app_unavailable` (the app was removed from the host since the link was sent). |
| `POST /api/share/player/leave` | 🍪 | Frees the slot; the invitation stays valid. |
| `POST /api/share/player/log` | 🍪 | The guest page's console, in debug mode, into the client log. |

**The shape of `url`.** Recomposed on every read, so an invitation minted before an
address changed hands out the current form without being revoked.

- Reachable: `https://stream.{MW_DOMAIN}/{rendezvous-id}#t={token}`. The path
  names the machine, which the introduction server has to see to route the call.
  The token rides in the **fragment**, which a browser never sends to the server
  it fetched the page from, so it never reaches that server in a path, a query or
  a referrer. Nothing names the guest page: a token is only ever for that page,
  so the bootstrap infers it. The bootstrap carries the token across the handover
  and the guest's page ends up at `/p#{id}&t={token}`. A link minted before this
  and spelling the page out (`#p=/p&t={token}`) is still honoured.
- Otherwise, with a `domain` of your own in `settings.json` ([Settings §7.5](07-Settings-Reference.md#75-bring-your-own-domain--certificate)):
  `https://{domain}/p/{token}`, no port (the 443 forward is part of serving it),
  `reach: "domain"`.
- Otherwise, the address the owner opened the board on: the `Host` their
  browser sent, or `X-Forwarded-Host` from a reverse proxy on this PC or its
  LAN — `https://{that name}[:port]/p/{token}`. Never loopback, never over the
  rendezvous tunnel. An IP of this PC keeps its network's `reach` (`lan`,
  `tailscale`, `other`); a private address that is not one of its networks is
  skipped; a `.local`, `.lan`, `.home.arpa` or single-label name is `lan`, a
  `.ts.net` name `tailscale`, anything else (a public name or IP) `direct`.
- Otherwise: `https://{the host's default-route LAN IPv4}[:port]/p/{token}`, and `local_only` is **true**.
  Self-signed, same network only. Never loopback — the guest is on another PC.

That is Automatic. The owner can pin a host's links to one address on its board
(`POST /api/share/link-address`): their domain (`domain`, offered first when set),
or `https://{that address}[:port]/p/{token}`, the
LAN even with a line up, Tailscale's 100.x, or another card's address. The
networks that exist only inside the PC (Hyper-V's Default Switch, WSL,
VirtualBox, VMware, Docker, libvirt) are never offered. `reach` says where the
link opens from: `public`, `domain`, `direct`, `lan`, `tailscale` or `other`;
the board says nothing about `domain` and `direct`, which it cannot test. A pinned network that
is gone (Tailscale disconnected) gives the Automatic link with
`link_address_missing: true`, and the pick is kept.

Only those forms are ever minted. A share link lives for weeks in somebody
else's chat window, so it is deliberately **not** gated on the line being up at
the moment it is created: an invitation is opened tonight or tomorrow, and
refusing to mint one over a thirty-second blip would be absurd. `remote_reachable`
on the board is what says the machine is not answering right now; the link itself
stays valid and stays the same characters.

## 8.4 Admin, settings, internet, setup, system (`SystemRoutes.cpp`)

| Method & path | Access | Description |
|---|---|---|
| `GET /api/admin/token` | 🏠 | `{token}` — the per-run admin key required on 🏠 writes (`X-MW-Admin-Key`). Handled in `processRequest`, not the router, so it can never be reached ahead of the screening rules above. |
| `GET /api/admin/settings` | 🏠 | `{http_port, https_port, cert_auth_enabled}` + `local_key` for 🖥️ callers only (the single-use host key that lets the host's own browser carry its admin session through the entry link). |
| `GET /api/server/remote-link` | 🏠 | `{url, pending}` — the entry link with that host key in its **fragment**, plus an optional `?p=` naming a page. Localhost-only: it hands out a live credential. The path is validated against the shape the bootstrap accepts rather than passed through, so this route can never put an arbitrary string into a link the machine hands its owner. Empty when the line is down — a single-use link that cannot work is worse than none, because the user spends it. `pending` is true while that empty url only means "not yet": the internet way in is wanted but the line has not come up and been checked since it was last wanted — the process starting, or the switch being turned on while it runs (bounded to 20 s from either). The Windows installer and the setup wizard's last button keep asking while it is set, instead of settling for loopback. |
| `POST /api/admin/settings` | 🏠 | Accepts **`https_port`** (rebinds live, deferred), **`cert_auth_enabled`**, `stream_notifications`, `packet_marking`, `virtual_display_exclusive`, `debug_mode`, `verbose_logs`, `autostart_enabled` and `instance_name`; anything else → 400. |
| `GET /api/settings/streaming` / `POST /api/settings/streaming` | 🔑 / 🏠 | Server-side streaming defaults, `transport_mode` included (moved here from `/api/internet/enable` — it is a streaming setting). The browser seeds its localStorage from these; POST is localhost-only and silently ignores per-device keys it doesn't know. `native_video_pipeline` (`auto` \| `d3d11` \| `d3d12` \| `vaapi` \| `vulkan`, [Settings](07-Settings-Reference.md)) is the exception: anything else answers **400** before any key of the request is written. GET adds the read-only `native_video_pipeline_supported` — a Windows host whose GPU encoder has a D3D12 route, or a Linux host with a GPU and the Vulkan chain built in: the condition for the admin's **Advanced** selector — and `native_video_pipeline_options`, the values this OS offers (`["auto","d3d11","d3d12"]` on Windows, `["auto","vaapi","vulkan"]` elsewhere). |
| `GET /api/host-apps` / `POST /api/host-apps` | 🏠 | The native host's apps (`host_apps`, [Settings](07-Settings-Reference.md)). Every answer is the section's whole state: `{supported, max, screen, apps, detected}` — `screen` is where the apps open (`gamescope` \| `virtual` \| `primary`), `detected` the launchers found on this PC (`{source, name, listed, dismissed}`). POST takes `{apps: [{id?, name, command}]}`, the whole list: an entry with a known `id` keeps its card id, source and image, one without gets new ones; a broken rule answers **400** and writes nothing. The administrator's only, GET included: a command runs as the desktop's user. |
| `POST /api/host-apps/restore` | 🏠 | `{source}`: a launcher found on this PC whose card was removed, added back with its icon card. **404** when this PC does not have it. |
| `POST /api/host-apps/:id/image` / `DELETE /api/host-apps/:id/image` | 🏠 | `{png: <base64>}`: a PNG of at most 3 MB and 4096 px a side, cropped to cover the card's 3:4 and stored at 600×800 (the admin page converts JPEG and WebP first). Served by `/api/hosts/:id/appasset`, the hash in the card's `device.art` — the URL's `&v=` — making each version cacheable for good. |
| `GET /api/internet/status` | 🔑 | Full Internet-Access state: `lan_only` (true on a LAN-only instance, `MW_LAN_ONLY`), phase, `custom_domain` (user-owned FQDN → network-only mode), `consent_version`, public IP, external ports, hairpin, transport mode, last error, and **`rendezvous: {url, online}`** — the machine's address and whether its line is up. Those are two separate answers on purpose: an address that exists but is not being held is a link that would fail, and showing it as if it worked is worse than showing nothing. 🔑-but-remote callers get `local_ip(s)`, `public_ip`, `unique_id`, cert material **and the whole `rendezvous` block** redacted — that block carries this instance's permanent identifier, and a permanent identifier is a tracking handle. A remote admin who needs the address already has it in their address bar. |
| `POST /api/internet/enable` | 🏠 | Opt-in (records the versioned consent `{message, source, version, mechanism}`) + start the manager **and** the rendezvous line. Also the write route for `unique_id` (only while unset), `public_ip`, `auto_ip_detection`, `upnp_enabled`. On a LAN-only instance (`MW_LAN_ONLY`) a request to enable answers **409** with the reason and writes nothing; disabling still works. |
| `POST /api/internet/disable` / `POST /api/internet/refresh` | 🏠 | Stop (line closed; the router ports already mapped stay until exit) / force a public-IP and reachability re-check, and renew every router hole held (**409** on a LAN-only instance). |
| `GET /api/internet/upnp-probe` | 🏠 | On-demand IGD discovery → `{available, gateway, lan_ip, external_ip}`. `upnp_available` in `/api/internet/status` is only meaningful once the manager has run a discovery, so a LAN-only install always reads false there. **Blocks ~2 s** on the main thread (M-SEARCH) — an explicit operator action, never polled. Reuses a live IGD rather than re-discovering under an active session. **409** on a LAN-only instance, which never maps a port. |
| `GET /api/setup/status` / `POST /api/setup/apply` | 🏠 | First-run wizard state + apply (internet consent, Sunshine install/pair) with live checklist. `internet.rendezvous` carries `{url, online}` — the address the machine is reached at, which the wizard's done screen names (it is usually still empty in `apply`'s own response, since the claim is a round trip that cannot run while that handler holds the event loop: the wizard picks it up from `status`); `apply` throws the same Internet-access switch as `/api/internet/enable`, rendezvous line included. `headless: true` when no desktop session — the wizard then drops the Sunshine step entirely (nothing to capture, no GPU, and `pkexec` has no agent to ask). On a LAN-only instance the Internet step is refused: the checklist marks it failed and the response carries the reason as `internet_error`, which sends the wizard back to its Internet page. |
| `POST /api/system/open-screen-recording` | 🏠 | macOS: open the Screen-Recording TCC pane, for the native host as well as for a local Sunshine (§9.2). |
| `POST /api/system/start-sunshine` / `POST /api/system/stop-sunshine` | 🏠 | Control the local Sunshine (liveness probed on port 47989). |
| `GET /api/native/status` | 🔓 / 🏠 | Whether this machine can stream itself, and on what. Availability + the enum `reason` go to **any** caller (it explains why the "MoonlightWeb Host" card is or isn't in their list); the machine detail — `displays[]` with `gpu`/`gpu_driver`/`encoder`/`codecs`/`hdr_active`, `capture` — only to a caller on the host itself, same trimming as `/internet/status`. `gpu_driver` is the encoding GPU's driver version as Windows writes it (`32.0.101.7088`), absent where the platform does not say. On Windows and Linux that detail also carries `video_pipeline` (the `native_video_pipeline` setting) and, per display, `video_pipeline_auto` — what `auto` picks for the GPU that would encode it (on Windows `d3d12` for an Intel GPU, `d3d11` for NVIDIA and AMD; on Linux `vulkan` for an AMD GPU, `vaapi` for the others, `vulkan` being taken only on its pixel proof). A driver the D3D12 route stays off reads `d3d11` there, with `video_pipeline_excluded` saying why; the list of such drivers is empty today. `remote_session` true means the engine runs in the **console session** (this process is a service, [ch. 9 §15](../design/native-capture-encoder.md)); `user_present` false with it is "nobody logged in yet", not "unsupported". |
| `GET /api/native/virtual-display` | 🔓 | **MoonlightWeb Virtual Display** on this machine: `supported`, `installed` (Windows: our device node exists; macOS: always), `enabled` (on), `active` (in the engine's list right now), `can_manage` + `method` (`task` = the installer's elevated task, `elevated` = this process already is, `inprocess` = macOS creates it itself), `name`, and the `job` in flight (`{state: idle\|elevating\|applying\|configuring\|refreshing\|done\|failed, action, error?}`). Nothing adds or removes it over REST: the installer does, and a `/start` on its app card (id 1000) turns it on. |
| `POST /api/logs/client` | 🔑 | A page's console lines, in debug mode, into `logs/moonlightweb-client.log`. |
| `GET` / `POST /api/logs/archive`, `GET /api/logs/archive/download` | 🏠 | **Download logs**: build the logs archive with what is secret or personal taken out (`LogScrubber`), follow it, then download it. |

## 8.5 WebSocket surfaces

All are reached through the **single HTTPS port** — `HttpServer` recognizes the `Upgrade` handshake and raw-proxies to internal WS servers:

| Path | Internal port (default) | Purpose |
|---|---|---|
| `/ws` | 48001 (`--ws-port`) | **Signaling** for WebRTC: JSON SDP offers/answers + ICE candidates, per-session (stream slot 0). |
| `/ws/stream` | 48002 | **Fallback WSS transport**: binary multiplexed video/audio/input when WebRTC can't connect (slot 0). |
| `/ws1`, `/ws1/stream` | 48011, 48012 | The same two surfaces for **stream slot 1** — the standby leg of seamless quality switching. Both slots' worker children can listen at once. |
| `/ws2`…`/ws4` (+ `/stream`) | 48013/48014, 48015/48016, 48017/48018 | One pair per **invited player** (slot *n* ≥ 1 → 48011 + 2 × (*n* − 1), relay one above). Gated on the `mw_player` cookie of *that* slot's live share activation — a session cookie and even a loopback peer are refused here. |
| `/ws/control` | 48003 | **Control channel**: every open tab holds one; used to redirect a tab to `/admin` on a second app launch (single-tab dedup). |

The frontend always builds WS URLs from `window.location.host` (page origin) — required for non-default external ports (port parity / multi-instance NAT).

## 8.6 Static file serving

Anything not matching `/api/*` or a WS upgrade is served by `StaticFileHandler` from the bundled `frontend/` directory (MIME-typed, `Cache-Control: no-cache` on text assets so `VersionGuard` + reload always pulls fresh code). SPA paths (`/admin`, `/settings`, `/setup`) serve `index.html`.

---

[← Settings Reference](07-Settings-Reference.md) · [Home](Home.md) · [Next: Installers & Packaging →](09-Installers-and-Packaging.md)
