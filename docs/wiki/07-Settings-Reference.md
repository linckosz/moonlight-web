[← Security](06-Security.md) · **Settings Reference** · [Next: REST API →](08-REST-API.md)

---

# 7. Settings Reference

All server configuration lives in a single JSON file managed by `backend/src/server/AppSettings.{h,cpp}`. There is no registry, no multi-user store; per-browser *streaming* preferences additionally live in the browser's `localStorage` and override server defaults per session.

## 7.1 `settings.json` location

| OS | Path |
|---|---|
| **Windows** | `%APPDATA%\MoonlightWeb\MoonlightWeb\settings.json` |
| **macOS** | `~/Library/Application Support/MoonlightWeb/MoonlightWeb/settings.json` |
| **Linux** | `~/.local/share/MoonlightWeb/MoonlightWeb/settings.json` |

Access is single-threaded, synchronous I/O. **Restart the server after a manual edit** (most keys are read at startup or on demand; the file is not watched). Documented file-only keys are seeded into the file at startup (`seedDocumentedDefaults()`) so they are discoverable.

## 7.2 Key reference

### Server & network

| Key | Type | Default | Description |
|---|---|---|---|
| `http_port` | int | `80` | HTTP listener (redirects to HTTPS). CLI `--port` overrides. When 80 is denied (unprivileged install) the fallback ladder is `8080`, `18080`, `28080`, then a scan from 49080. The *actually bound* port is persisted back. |
| `https_port` | int | `443` | HTTPS listener preference. When 443 is denied (unprivileged install — only the systemd unit gets `CAP_NET_BIND_SERVICE`) the fallback ladder is `8443`, `18443`, `28443`, then the high ranges. The bound port is persisted back and preferred on the next boot, which is why the ladder stays below 32768: ports above it are handed out to outgoing connections by the OS, so the local address would drift on every restart. |
| `transport_mode` | string | `"auto"` | Transport for streams: `auto` \| `all` \| `webrtc-media-udp` \| `webrtc-dc-udp` \| `webrtc-media-tcp` \| `webrtc-dc-tcp`. `auto` = fallback chain (see [Transports](05-Streaming-and-Transports.md)). Written by `POST /api/internet/enable` (the admin page's transport selector). `wss` is also accepted but is not offered in the selector outside debug builds — it needs a browser opened on the host's LAN IP, and is dropped from the chain for anyone arriving through the rendezvous. `all` runs the same chain as `auto` and only puts `wss` back in the selector — for a browser that reaches the host directly: its LAN IP, or a domain of your own (A record to the router's public IP, TCP 443 forwarded, Let's Encrypt/ZeroSSL certificate). |
| `stun_server` | string | `"stun:stream.{MW_DOMAIN}:3478"` | Used by both libdatachannel and the browser's `RTCPeerConnection`. Derived from `MW_DOMAIN` when the key is absent, so an instance under its own domain reaches its own operator. A STUN binding tells whoever answers the public address of whoever asked, which is why the default is the server the Internet-access consent already names rather than a public one; `StunClient::defaultServers()` keeps the public servers (Google, Cloudflare, Nextcloud, metered.ca) behind it for the backend's own detection, because a machine that cannot learn its own address cannot be reached at all. A **LAN** session is told to use no STUN server at all — the browser gets an empty `ice-config` list rather than its built-in fallback. |
| `upnp_enabled` | bool | `true` | UPnP port mapping for NAT traversal — the control tunnel's holes and each stream slot's, all claimed by `RouterPortAllocator`, which skips any entry the router says belongs to another machine on the LAN (candidate lists in `backend/src/network/RouterPortPools.h`). Only with Internet Access on; read live, so the toggle applies to the next claim. |
| `router_ports` | object | *(absent)* | **File-only**, written by the app, never seeded. The router ports this host obtained last time — `{lan_ip, tunnel: [3478, …], media: {"0": 48550, …}}` — asked for first at the next start, so the first host installed on a LAN keeps the corporate-friendly `3478` whatever order the machines boot in. A port is forgotten the moment the router says it belongs to another machine. Delete the key to start the walk from the top again. |
| `stream_worker_enabled` | bool | `true` | **File-only** (no UI), seeded at startup. Each session runs in a `--stream-worker` child process → two concurrent stream slots + crash isolation (see [Streaming §5.6](05-Streaming-and-Transports.md#56-session-lifecycle--teardown-discipline)). `false` runs the session in-process instead, single stream. |
| `native_host_enabled` | bool | `true` | **File-only** (no UI), seeded at startup. Whether this machine offers **itself** as a host — the `<hostname> — MoonlightWeb Host` card. `false` hides it from the host list and answers `/api/native/status` with `{available: false, enabled: false, reason: "Disabled"}`; the setup wizard then treats the machine as unable to host itself (`native.possible` false) and offers Sunshine. It gates **visibility, not installation**: the engine stays installed and untouched, and no probe is spawned at all while it is off (as a service, that means no console-session probe process). For someone running MoonlightWeb as a pure front end for other hosts — Sunshine, Wolf, a private cloud-gaming rig — where a card pointing at the machine that merely serves the page would be an invitation to stream the wrong thing. Read at startup: restart the server after editing. |
| `virtual_display` | object | *(absent)* | **File-only**, written by the app, never seeded. Present while *MoonlightWeb Virtual Display* is **on** — `{active, previous_primary, activated_at}` — so the display that was primary before it can get its role back when the last stream ends, and so a server restarted mid-stream knows to turn the display off. Cleared when it goes off. |
| `update_relay_enabled` | bool | `true` | **File-only** (no UI), seeded at startup. The switch on **Settings → Privacy** writes this key (and the other one) — no consent gates it. Routes the 6-hourly update check through `https://updates.{MW_DOMAIN}` instead of `api.github.com`; the relay returns the same release JSON and counts version/OS/arch in aggregate (see [Infrastructure §10.10](10-Infrastructure-Stack.md#1010-update-relay--installed-version-census)). `false` checks `api.github.com` directly and reports nothing. `MW_NO_TELEMETRY` in the environment goes further and stops the check happening at all — it used to merely redirect it to GitHub, which is still a six-hourly request to a third party and not what that variable is asking for. Self-built binaries never use the relay: it needs the `MW_PDNS_TOKEN` only official builds carry. |
| `session_metrics_enabled` | bool | `true` | **File-only** (no UI), seeded at startup. The switch on **Settings → Privacy** writes this key (and the other one) — no consent gates it. Reports the SHAPE of a streaming session to `https://metrics.{MW_DOMAIN}` when it starts and again when it ends — resolution, frame rate, negotiated codec, HDR/4:4:4, bitrate band, backend, transport, LAN-or-internet, device class, owner-or-player, duration (see [Infrastructure §10.11](10-Infrastructure-Stack.md#1011-session-census)). Never the host name, the account, the pairing identity or the application launched. `false` — or `MW_NO_TELEMETRY` in the environment — reports nothing; streaming is identical either way, and self-built binaries never report at all. |
| `session_location_enabled` | bool | `false` | **File-only** (no UI), seeded at startup. Resolves a session's city and country through `ipwho.is` so the admin sessions list can label it. **Off by default, and deliberately**: the address sent to that third party is the *visitor's*, not the owner's, so an invited player's public IP would reach a company they have never heard of in order to show the person who invited them a city name — and neither of them was asked. Private and cached addresses never leave the machine whatever this says, and the flag is read on every lookup, so switching it off takes effect without a restart. |
| `metrics_consent` | object | *(absent)* | **File-only**, written by the app, never seeded. **Dormant: nothing reads it.** The consent machinery, kept whole for the day a field genuinely needs an answer before it may travel — `{decision: "granted"|"denied", message, at, source, version}`, where `message` is the exact wording that was displayed and the version rule refuses to let an answer given to a narrower question cover a wider one. A record left by a version that did ask decides nothing in either direction: the two switches above are the whole rule. Neither census carries a field that identifies anyone, so what is owed is the disclosure and the means to refuse, both on **Settings → Privacy**. |

### Streaming defaults (overridable per-request by the browser)

| Key | Type | Default | Description |
|---|---|---|---|
| `video_codec` | string | `"auto"` | `auto` \| `h264` \| `hevc` \| `av1`. `auto` resolves to HEVC if the host supports it, else H.264. |
| `stream_bitrate` | int (kbps) | `20000` | 5 000–150 000. |
| `stream_resolution` | string | `"auto"` | How the streamed size is chosen. `auto` = on the native host, the host display's own size when it fits the box, else the host's picture scaled down on the host, its shape kept, to fit it — never upscaled. On a computer the box is this screen (a 2560×1440 display on a 2000×1600 laptop streams 2000×1125), and the stream follows the window moving to another monitor (relaunched at the new size). On a phone or a tablet, where the picture is pinched and zoomed, the box is 1440 lines by any width, the same whichever way the device is held: a host of up to 1440 lines streams as it is (a 2560×1440 PC on an iPhone streams 2560×1440, shown reduced, sharp when zoomed), a 4K host comes down to 1440, nothing is reduced below 1080. A host in portrait (1080×1920) keeps its shape: 810×1440 on a phone. Sunshine, Apollo and Wolf cannot report their display's size and stream 1080p under it, the width from the measured ratio. `device` = *Match my screen*: this screen's size in physical pixels, pixel for pixel — the native host puts its display in that mode for the session when its driver lists one (Windows only so far; the exact size, else the largest listed mode of the client's shape that fits) and otherwise streams exactly as `auto` would. On **"MoonlightWeb Virtual Display"** the mode is made rather than looked for: the size travels with the activation, the driver's mode list is rewritten with it ahead of the rest (macOS creates the display at that size outright), so a phone streams a 2532×1170 desktop no monitor has; other hosts get the explicit size with GameStream's optimal-settings flag, which lets them switch the host display to it. Follows this screen too. `custom` = the pair below; `fixed` = the rung in `stream_height`, width derived from the host's shape. Every choice but `fixed` fixes the shape itself: the native host fits its display's shape inside the requested box (a 16:9 display asked for 1920×1200 streams 1920×1080, shown 1:1 with bars on the client). |
| `stream_custom_width`, `stream_custom_height` | int | `1920`, `1080` | The `custom` pair, each pinned to 360–4096 and made even at launch. |
| `stream_bitrate_auto` | bool | `true` | The bitrate is the estimate's (resolution × frame rate × HDR/4:4:4, **halved on a phone or a tablet** — hand-sized screen, weakest decoder, mobile data rather than the LAN a desktop streams over) and follows the frame the host really streams — the launch's resolved size, then the native host's answer and its mode changes, moved on the host between two frames (`clientbitrate`). `false` once the slider is moved off the estimate: the value is the user's and stays whatever the resolution becomes; putting the slider back on the estimate makes it automatic again. |
| `stream_height` | int | `1080` | 720/1080/1440/2160 — the `fixed` rung, kept while another choice is active. The browser sends `0` to a native host under `auto` when it cannot read its own screen (its display's own size). |
| `stream_aspect` | string | `"auto"` | Kept for tests: since 0.3.1 the ratio is `auto` for everyone (the browser takes the host's real format from the shape it encodes, or failing that measures it from the bars Sunshine pads with, then relaunches at it) and the dropdown exists in debug builds only. A ratio a user forced before then (`4:3` …) is ignored at launch unless a debug build vouched for it (`stream_aspect_forced` in the browser's storage). Explicit values: `16:9` / `16:10` / `21:9` / `4:3` / `3:2` / `32:9` / `5:4`. |
| `stream_fps` | int | `0` | `0` = **Auto**, the default: the rate this very screen refreshes at, measured in the browser by requestAnimationFrame (`util/RefreshRate.js`) and rounded to whole frames — a 165 Hz panel asks for 165, a 60 Hz laptop for 60, so one streamed frame lands on one refresh: no frame encoded for nothing, no refresh showing the previous one again. The launch resolves it, and the same measurement (`client_refresh_mhz`) is what **"MoonlightWeb Virtual Display"** is created at, so the host's display, the encoder and the screen all run at one cadence. A client that measured none — an older frontend, a tab hidden at launch — streams at 60. Otherwise a rate to hold: 15–240. |
| `hdr_enabled` | bool | `false` | Request HDR10 encode (see [HDR limitations](05-Streaming-and-Transports.md#53-hdr--support-and-limitations)). |
| `chroma_444_enabled` | bool | `false` | YUV 4:4:4 (needs bandwidth + a browser decoding the 4:4:4 profile). SDR only: 10-bit 4:4:4 renders green on Chrome/Windows ([ch. 15 §15.6](15-Client-Presentation-Benchmarks.md#156-decision-5--444-chroma)). The native host grants it per codec (H.264/HEVC on NVENC, never AV1, nothing on AMF/oneVPL) and falls back to 4:2:0 with a log line. |
| `mute_host_audio` | bool | `true` | Mute the host PC speakers while streaming. GameStream hosts receive it as `localAudioPlayMode`; the native Windows host mutes its default output when the device mutes in hardware, routes to a virtual sink otherwise, and keeps playing (with a log line) when it can do neither. |
| `gaming_mode` | bool | `false` | Pointer-lock mouse (relative) vs absolute tracking. |
| `show_performance_stats` | bool | `false` | Stats overlay default. |
| `video_enhancement` | string | `"off"` (frontend seed; the backend answers `on` for a key it never stored) | `on`/`off` — upscale/sharpen on WebGL2 (SDR) or WebGPU (HDR). Off by default since 09/2026: the plain Canvas2D path is the fastest first impression ([ch. 15](15-Client-Presentation-Benchmarks.md)). When on, transport negotiation deprioritizes webrtc-media (canvas required). |
| `video_enhancement_algo` | string | `"auto"` | Always `auto` in production (desktop → FSR1 WebGL2, mobile/tablet → SGSR1 WebGL2, HDR → FSR1 WebGPU). Debug builds expose the full menu: `canvas2d` \| `video` \| `smooth2d` \| `gl-sgsr` \| `gl-nis` \| `gl-fsr1` \| `sgsr` \| `nis` \| `fsr1`. |
| `audio_time_stretch` | bool | `true` | **File-only** (no UI), seeded at startup: WSOLA pitch-preserving stretch in the AudioWorklet. |
| `keyboard_layout_fidelity` | bool | `false` | **File-only.** `true` makes the host type the character **your** layout produced, whatever layout the host runs (`azerty` typed on an AZERTY board shows `azerty` on a QWERTY host; `Ctrl+A` stays `Ctrl+A`). Keys where both layouts agree are untouched, and every corrected key is still a real key press, so games keep reading it. The **MoonlightWeb host** corrects every key in every direction, since it compares against its own layout. A **Sunshine Windows host** corrects letters only; digits and punctuation stay positional. **Sunshine macOS, Linux and Wolf hosts** stay positional (text injection there drops or garbles keys). `false` sends positions for everything, as every Moonlight client does. |
| `keyboard_debug` | bool | `false` | **File-only, never seeded** — add it by hand, restart, and remove it when done. Logs one line per printable key press with two verdicts: **Notepad** (the character a text field shows) and **Game** (the physical key a raw-keyboard title sees, by its US label); a `KO` is logged as a warning. On the MoonlightWeb host a second line gives the key its own layout actually resolved, read back through that layout. The lines are in the **worker** log (`moonlightweb-worker-<pid>.log`). |

The browser also stores per-device keys the server deliberately ignores when the Settings overlay POSTs them back (`touch_sensitivity`, `touch_screen`, `tearing_enabled`, `video_worker`, `power_save`, `power_save_backup`, `seamless_switching`) — see [§7.4](#74-browser-side-preferences-localstorage).

### Internet Access

The consent is the master switch: `internet_access_enabled` gates the per-session media-port mapping **and** the held line to the introduction server, and disabling it closes both immediately.

| Key | Type | Default | Description |
|---|---|---|---|
| `internet_access_enabled` | bool | `false` | The user's consent to be reachable from the internet. Gates the per-session UPnP media mapping and the rendezvous line, and auto-starts `InternetAccessManager` at boot. |
| `rendezvous_id` | string | generated | **The machine's address.** 26 Crockford base32 characters — the `{id}` in `https://stream.{MW_DOMAIN}/{id}`. A locator, not a secret: holding it grants nothing. Drawn once and never rotated, because an address that changes cannot be bookmarked. |
| `rendezvous_token` | string | generated | Ownership credential for the identifier above, presented as `X-MW-Owner`; the introduction server stores only its HMAC. **Secret.** Deliberately its own value, so it can be rotated or retired without touching anything else. |
| `rendezvous_claimed` | bool | `false` | Whether the introduction server has accepted the identifier. Until it has, the instance holds an id nothing knows about, and every entry point reports *no address* rather than one that answers to nobody. |
| `unique_id` | string | generated | 8-hex-char instance id, **internal only**: it seeds the deterministic UPnP fallback port so two instances on one LAN never collide. It is not an address and appears in no URL. |
| `domain` | string | `"MW_DOMAIN"` | The sentinel means "none" — an instance has no public DNS name and needs none. Replace it with **your own FQDN** to serve the app under it: the value is then kept verbatim and Internet Access switches to its network-only mode ([§7.5](#75-bring-your-own-domain--certificate)). Anything that is not a valid FQDN is ignored. |
| `public_ip` | string | — | Resolved public IP (STUN or manual). Also feeds the NAT-hairpin test and CGNAT detection. |
| `auto_ip_detection` | bool | `true` | STUN/HTTP auto-detect vs manual `public_ip`. |
| `internet_consent` | object | `{}` | `{message, at, source, version, mechanism}` — the exact opt-in agreement record, keeping the text the user was actually shown. A record whose wording predates the current mechanism authorises nothing: the manager waits in phase `consent_required` until the user agrees again ([Security §6.7](06-Security.md#67-internet-access-consent--audit)). |

### TLS & auth

| Key | Type | Default | Description |
|---|---|---|---|
| `cert_pem` / `cert_key` | string | `"MW_CERT_PEM"` / `"MW_CERT_KEY"` | Certificate/key **source**: an env-var *name* or a *file path*. The certificate must cover `domain` (**CN or SAN**, wildcards included) or it is skipped in favour of the `cert/` directory scan, then the self-signed fallback. Only relevant when serving under your own name ([§7.5](#75-bring-your-own-domain--certificate)) — a remote browser arriving through the entry link meets DTLS, not this. |
| `hmac_key` | string (Base64) | generated | Session-token HMAC key, persisted so sessions survive restarts. |
| `certificate_token` | string | generated first boot | The downloadable auth token file content. |
| `cert_auth_enabled` | bool | `false` | Enable certificate-file authentication. |
| `admin_password` | string | absent | PBKDF2-SHA256 digest (`pbkdf2-sha256$iters$salt$key`) of the remote admin password — the one a LAN machine spends to open the admin page. **Absent means no password has been set**, so the LAN unlock door is shut and the admin page says so — there is no built-in default. Set from the admin page, or with `moonlightweb --set-admin-password`. See [Security §6.2.1](06-Security.md#621-remote-admin-password-lan-only). |
| `remote_admin_enabled` | bool | `true` | Whether the LAN may unlock admin access at all. Set to `false` for host-only administration; the stored digest is kept, so re-enabling restores your password rather than the default. |
| *(host key)* | string | generated | Single-use host-machine key embedded as `?mwk=` in host-side entry URLs; rotates on redemption. |

### Lifecycle

| Key | Type | Default | Description |
|---|---|---|---|
| `setup_completed` | bool | `false` | First-run wizard done. Windows: set by the installer's provisioning flow; macOS/Linux: by the in-app `/setup` wizard. While false on a GUI launch, the browser opens `/setup`. |

## 7.3 `.env` — environment configuration

Loaded at startup by `loadEnvFile()` (`.env` next to the executable, else the project root; values quoted or multi-line PEM blocks supported). Reference: `.env.example` at the repo root. All values are optional — without them the server runs LAN-only with a self-signed cert.

| Variable | Required for | Description |
|---|---|---|
| `MW_DOMAIN` | Internet Access | The project domain everything else is derived from: the introduction server at `stream.{MW_DOMAIN}`, STUN at `stream.{MW_DOMAIN}:3478`, the update relay at `updates.{MW_DOMAIN}`. Fallback default: `moonlightweb.top` — **except for a DEV identity** (the `MoonlightWebDev` build every untagged CI run produces, or any build under `--dev`), where an unset value defaults to `dev.moonlightweb.top`, the staging rendezvous ([Infrastructure §10.14](10-Infrastructure-Stack.md#1014-production-and-staging-on-one-box)). This beats the domain CI embeds, so one secret serves both editions. Set `MW_DOMAIN=moonlightweb.top` explicitly to point a DEV identity at production. A fork sets this and needs nothing else. |
| `MW_RENDEZVOUS_URL` | optional | Overrides the derivation above outright when the introduction server lives somewhere else (trailing slashes trimmed) — a lab box, say. It also suppresses the DEV staging default. |
| `MW_PDNS_TOKEN` | update relay + census | The **restricted** key an instance presents to `updates.` and `metrics.` ([Infrastructure §10.9](10-Infrastructure-Stack.md#109-least-privilege-api-key-mw-proxy)). **Secret.** Absent → the update check goes straight to GitHub and nothing is counted, which is exactly what a self-built binary does. |
| `MW_NO_TELEMETRY` | optional | Set to anything and neither census is contacted, whatever the settings say. |
| `MW_LAN_ONLY` | forks developing on a LAN | `1` / `true` / `yes` / `on` makes the instance **LAN-only** ([§11.1bis](11-Build-CI-Testing.md#111bis-lan-only-development-environment-for-a-fork)); anything else, empty included, leaves it off. `internet_access_enabled` then reads `false` whatever `settings.json` says, the rendezvous line never opens and no rendezvous address is handed out, neither end is given a STUN server, no UPnP discovery or mapping runs, and neither the update check nor the census is made. Asking for more is refused with the reason: `POST /api/internet/enable` (to enable), `/api/internet/refresh` and `/api/internet/upnp-probe` answer **409**, `/api/setup/apply` returns it as `internet_error`, and `--enable-internet` exits 1. `--dev` skips its staging default. `/api/internet/status` reports `lan_only`. Set in the environment CMake is configured from, it is also embedded in the binary (below). |
| `MW_CERT_PEM` / `MW_CERT_KEY` | optional | Inline PEM cert/key (the default `cert_pem`/`cert_key` settings point at these env-var names). |
| `MW_SERVICE` | service installs | Set by service supervisors: suppresses browser/tray/shortcut behavior and port-mapping take-overs. |
| `MW_MEDIA_QUEUED_VIDEO` | debug | `1` rolls `MediaTrackRelay` back to the old queued-video send path (default: send from the capture thread). |

### Build-time embedded fallbacks

CI bakes `MW_DOMAIN` and `MW_PDNS_TOKEN` (from repo secrets, via CMake defines) into release binaries. `applyEmbeddedEnvDefaults()` applies them **only when the runtime env/.env did not set them** — so distributed builds work out of the box against the shared infrastructure, while forks and self-hosters override cleanly. A build compiled without them is not crippled: it still reaches the introduction server through the `MW_DOMAIN` fallback default, and only the update relay and the censuses (which need the restricted key) fall back to talking to GitHub directly and counting nothing.

`MW_LAN_ONLY` rides a similar path: present in the environment `backend/CMakeLists.txt` is configured from, it is compiled in, and `mw::edition::lanOnly()` (`backend/src/common/Edition.cpp`) falls back to it when the runtime environment has no value — a LAN-only binary stays LAN-only when copied away from the checkout whose `.env` said so. A runtime value still wins. Unlike the two above, nothing in CI sets it.

## 7.4 Browser-side preferences (`localStorage`)

| Key | Description |
|---|---|
| `mw-streaming-settings` | The whole Settings overlay state as one JSON object (`SettingsView`): mirrors of the streaming defaults above, sent with each `/start`, **plus** per-device-only fields — `touch_sensitivity`, `touch_screen` (absolute touch instead of the trackpad model — both exposed on mobile/tablet only, a touchscreen laptop keeps the mouse/trackpad path), `tearing_enabled` + `tearing_default_v2` (on by default, Chromium desktop only; off = VSync pacing — the marker tells a stored `false` apart from the earlier OFF default), `video_worker` (`auto`\|`on`\|`off` — OffscreenCanvas decode/render worker, `auto` by default), `power_save` + `power_save_backup` (mobile light pipeline), `seamless_switching`. |
| `mw-gamepad-mappings` | The user's own controller layouts (`util/gamepadMappingsStore.js`), `{ [padKey]: {name, bindings, updatedAt} }` — `padKey` is `usb:vid:pid`, or `name:<name>` where the browser gives no ids (Android, iOS). Written by the remap dialog (Settings → **Controllers**, the stream toasts, a share guest's join page and header button); kept apart from `mw-streaming-settings` so it is never pushed to the server as a default. See [design §7](../design/gamepad-virtual.md#7-détection). |
| `mw_player_prefs` | A share guest's join choices: `gaming`, `touchScreen`, and `padKey` — the controller picked when several are plugged in (one controller per invitation). |
| `mw-lang` | UI language (en/fr/zh). |
| `mw_client_uniqueid` | Per-browser Sunshine identity (session isolation); the standby stream slot derives its own id from it. |
| `mw_jitter_auto` | Adaptive jitterBufferTarget toggle (webrtc-media). |
| `mw_pacing` | Adaptive presentation reserve (DataChannel/WSS paths — the `FramePacer` de-jitter clock). Off by default (freshest-frame-first: on real Wi-Fi the reserve legitimately sits near its 25ms cap, and latency wins over smoothness); set to `1` to opt in. The reserve is shown live in the latency breakdown ("jitter reserve" — the row only exists while the pacer runs), and is read once per stream start — set it, then relaunch the stream. |
| `mw_hdr_tonemap` | `1` forces the ACES tone-map on an HDR display (to compare with the `linear` path), `0` disables the raw-plane readback altogether; `mw_hdr_curve` / `mw_hdr_exposure` / `mw_hdr_refwhite` tune the tone-map ([ch. 5 §5.3](05-Streaming-and-Transports.md#53-hdr--support-and-limitations)). |
| `mw_hdr_ext_tonemap` | `1` forces the ACES tone-map fed by `importExternalTexture` (no readback, works on HEVC), `0` disables it. On by default on WebKit only, the one engine that does not tone-map on import ([ch. 5 §5.3](05-Streaming-and-Transports.md#53-hdr--support-and-limitations)) — forcing it on a browser that *does* would apply the PQ EOTF twice and crush the picture. |
| `mw_hdr_request` | `1` offers the HDR checkbox even when `hdrClientCapability()` says the client cannot present HDR (experiments only). |
| `mw_force_2d` | `1` forces the Canvas2D renderer whatever the Enhancer says (comparison runs). |
| `mw_force_2d` | Force the Canvas2D renderer (skip the WebGPU probe). |
| `mw_setup_dismissed` / `mw_update_dismissed` | Banner dismissals (setup nudge, update banner). |

## 7.5 Bring your own domain & certificate

Bringing your own domain is how you serve the web interface under a name of your own, as an alternative to (or alongside) the entry link: set `domain` in `settings.json` and the manager runs in a **network-only mode** — public-IP detection and the NAT-hairpin test still run, the zone and the certificate are yours, and the 443 forward on your router is yours to create. This is the one configuration in which a browser opens a TLS socket to your machine directly, so it is also the one in which a certificate matters.

**1. DNS — A record on your public IP.** In `example.com`'s zone create `stream` → **your router's public IPv4** (the WAN address, *not* the machine's `192.168.x.x`). Check it with `curl https://api.ipify.org` on the host, or read *Public IP* on the admin page. IPv6-only reachability needs an AAAA record instead. A dynamic ISP address needs a DDNS updater; a **CGNAT** address (`100.64.0.0/10`) can never be port-forwarded — the admin page detects and reports that case.

**2. Router — port forwarding.**

| Forward | To | Why |
|---|---|---|
| **TCP 443** (mandatory) | `<LAN IP>`:*the bound HTTPS port* | Page, REST API, WSS signalling and the `wss` fallback transport all ride the single HTTPS port. |
| UDP 48550-48573, one per slot (recommended) | same host, **same ports** | Direct WebRTC media (`kMediaBasePort = 48550`, then `48550 + slot`). Without it the fallback chain still works over ICE-TCP, then `wss`, at a worse latency profile. |
| TCP 80 | — | **Leave it closed.** See below. |

**The TCP rule may translate the port; the UDP rules may not.** The web side survives `443 → 8443` because the frontend keeps only the *path* from the backend's signalling URL and rebuilds it on `window.location.host` (`_streamWsUrl` in `app.js`), so every REST and WebSocket call follows the origin the page was served on. The media rules are the opposite case: the port travels inside the ICE candidates, so a rewritten one points the browser at a port nothing is listening on.

**Why 80 is better left shut.** It buys only the HTTP→HTTPS redirect, and that redirect is built from the *internal* port (`m_ActiveHttpsPort`, `HttpServer.cpp`): a machine listening on 8443 behind an external 443 would redirect visitors to `https://stream.example.com:8443`, which is not open. It is also an unauthenticated surface for the sake of saving five characters of typing. Open it only if you renew with an HTTP-01 client on that same machine.

With `upnp_enabled: true` **and** Internet Access on, the per-session media ports are still mapped for you; the TCP 443 rule is always manual with a custom domain.

**3. Certificate.** Issue one for `stream.example.com` (certbot / win-acme / lego with your DNS provider, or any commercial CA). You need two **unencrypted PEM** files: the full chain and the private key (RSA or EC). A wildcard (`*.example.com`) works too.

**4. Declare the domain and install the certificate.** Stop the server, edit `settings.json` (path in [§7.1](#71-settingsjson-location)), then restart:

```jsonc
{
  "domain": "stream.example.com",
  // optional — absolute paths to your PEM files; omit to use the cert/ folder
  "cert_pem": "C:/certs/fullchain.pem",
  "cert_key": "C:/certs/privkey.pem"
}
```

Without `cert_pem`/`cert_key`, drop both files (`*.pem`, same folder) into the data dir's `cert/` subfolder — `%APPDATA%\MoonlightWeb\MoonlightWeb\cert\` on Windows, `~/Library/Application Support/MoonlightWeb/MoonlightWeb/cert/` on macOS, `~/.local/share/MoonlightWeb/MoonlightWeb/cert/` on Linux. `CertManager` scans recursively for a certificate that **covers the domain (CN or SAN, wildcards included)** plus a private key beside it.

Confirm in `logs/moonlightweb.log`: `SSL certificate loaded: CN=stream.example.com`. **Renewal is yours**: replace the files and restart. A certificate near expiry is kept and logged as *renew it soon* — it is never silently downgraded to a self-signed one.

**5. Internet Access — optional.** With a custom domain it registers nothing and requests no certificate; it re-detects the public IP, tests NAT hairpin, and allows the per-slot media mapping (`RouterPortAllocator::enabled()` = `upnp_enabled && internet_access_enabled`). Turn it off and the 48550+ mappings become manual too. `GET /api/internet/status` reports `custom_domain: true` in that mode.

The host machine's **own** entry points — tray menu, Desktop shortcut, browser auto-open — stay on `https://localhost:<port>` whatever you configure here. They are not built from `domain` at all: the tray resolves its address at the click and prefers the entry link with a single-use host key, everything else is written down ahead of the click and stays on loopback. Your domain is for every *other* device.

Authentication is unchanged: remote devices still need the admin PIN or the certificate-auth token file (see [Security](06-Security.md)).

**Caveats**

- Reaching `stream.example.com` from *inside* your own LAN requires NAT-hairpin support on the router; otherwise use `https://<LAN IP>` at home (self-signed warning) or add a split-horizon DNS entry. The host machine itself needs nothing — see the note in step 5.
- **Check the bound HTTPS port before writing the router rule.** An unprivileged install cannot take 443 and falls back to 8443 (then 18443, 28443…); the *bound* port is persisted back into `https_port`, and that is the internal side of the forward. Externally you still publish 443, so visitors type the bare name — but if you also want to reach it without a forward at all, the URL carries the port.
- Everything on this page is about the **certificate** and **the machine's own name**. The certificate is not needed for remote access as such: the entry link is served under the introduction server's certificate and the tunnel is bound to the machine's own key, so an instance with no domain and no PEM file is already reachable from anywhere ([Security §6.5](06-Security.md#65-tls)).
- `unique_id` and `rendezvous_id` are untouched: the entry link keeps working alongside your own name, and restoring `"domain": "MW_DOMAIN"` simply leaves the instance with no public DNS name again.
- The `stream` label is only reserved on the project's shared domain, never on yours.

---

[← Security](06-Security.md) · [Home](Home.md) · [Next: REST API →](08-REST-API.md)
