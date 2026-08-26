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
| `transport_mode` | string | `"auto"` | Transport for streams: `auto` \| `webrtc-media-udp` \| `webrtc-dc-udp` \| `webrtc-media-tcp` \| `webrtc-dc-tcp` \| `wss`. `auto` = fallback chain (see [Transports](05-Streaming-and-Transports.md)). Written by `POST /api/internet/enable` (the admin page's transport selector). |
| `stun_server` | string | `"stun:stun.l.google.com:19302"` | Used by both libdatachannel and the browser's `RTCPeerConnection`. |
| `upnp_enabled` | bool | `true` | UPnP port mapping for NAT traversal. |
| `stream_worker_enabled` | bool | `true` | **File-only** (no UI), seeded at startup. Each session runs in a `--stream-worker` child process → two concurrent stream slots + crash isolation (see [Streaming §5.6](05-Streaming-and-Transports.md#56-session-lifecycle--teardown-discipline)). `false` = legacy in-process mode, single stream. |
| `update_relay_enabled` | bool | `true` | **File-only** (no UI), seeded at startup. Routes the 6-hourly update check through `https://updates.{MW_DOMAIN}` instead of `api.github.com`; the relay returns the same release JSON and counts version/OS/arch in aggregate (see [PowerDNS Stack §10.8](10-PowerDNS-Stack.md#108-update-relay--installed-version-census)). `false` — or `MW_NO_TELEMETRY` set in the environment — checks GitHub directly and reports nothing. Self-built binaries never use the relay: it needs the `MW_PDNS_TOKEN` only official builds carry. |
| `session_metrics_enabled` | bool | `true` | **File-only** (no UI), seeded at startup. Reports the SHAPE of a streaming session to `https://metrics.{MW_DOMAIN}` when it starts and again when it ends — resolution, frame rate, negotiated codec, HDR/4:4:4, bitrate band, backend, transport, LAN-or-internet, device class, owner-or-player, duration (see [PowerDNS Stack §10.9](10-PowerDNS-Stack.md#109-session-census)). Never the host name, the account, the pairing identity or the application launched. `false` — or `MW_NO_TELEMETRY` in the environment — reports nothing; streaming is identical either way, and self-built binaries never report at all. |

### Streaming defaults (overridable per-request by the browser)

| Key | Type | Default | Description |
|---|---|---|---|
| `video_codec` | string | `"auto"` | `auto` \| `h264` \| `hevc` \| `av1`. `auto` resolves to HEVC if the host supports it, else H.264. |
| `stream_bitrate` | int (kbps) | `20000` | 5 000–150 000. |
| `stream_height` | int | `1080` | 720/1080/1440/2160, or `0` = *Native Host* (largest reported display mode). Width is derived. |
| `stream_aspect` | string | `"auto"` | `auto` (the browser takes the host's real format from the shape it encodes, or failing that measures it from the bars Sunshine pads with, then relaunches at it) or explicit `16:9` / `16:10` / `21:9` / `4:3` / `3:2` / `32:9` / `5:4`. Width is derived from the height. |
| `stream_fps` | int | `60` | 15–240. |
| `hdr_enabled` | bool | `false` | Request HDR10 encode (see [HDR limitations](05-Streaming-and-Transports.md#53-hdr--support-and-limitations)). |
| `chroma_444_enabled` | bool | `false` | YUV 4:4:4 (needs bandwidth + a browser decoding the 4:4:4 profile). |
| `mute_host_audio` | bool | `true` | GameStream `localAudioPlayMode`: mute the host PC speakers while streaming. |
| `gaming_mode` | bool | `false` | Pointer-lock mouse (relative) vs absolute tracking. |
| `show_performance_stats` | bool | `false` | Stats overlay default. |
| `video_enhancement` | string | `"on"` | `on`/`off` — WebGPU upscale/sharpen. When on, transport negotiation deprioritizes webrtc-media (canvas required). |
| `video_enhancement_algo` | string | `"auto"` | `auto` \| `sgsr` \| `fsr1` \| `force2d`. |
| `audio_time_stretch` | bool | `true` | **File-only** (no UI), seeded at startup: WSOLA pitch-preserving stretch in the AudioWorklet. |

The browser also stores per-device keys the server deliberately ignores when the Settings overlay POSTs them back (`touch_sensitivity`, `touch_screen`, `tearing_enabled`, `video_worker`, `power_save`, `power_save_backup`, `seamless_switching`) — see [§7.4](#74-browser-side-preferences-localstorage).

### Internet Access

The consent is the master switch: `internet_access_enabled` now gates the per-session media-port mapping too, and disabling it removes the router mappings immediately. The DNS half of the table only applies to a **legacy instance** — one that registered a `{unique_id}.{MW_DOMAIN}` subdomain before the mechanism was retired (`registered_uid` set). The shared DNS service behind those subdomains shuts down in **February 2027**.

| Key | Type | Default | Description |
|---|---|---|---|
| `internet_access_enabled` | bool | `false` | The user's consent to be reachable from the internet. Gates the per-session UPnP media mapping, auto-starts `InternetAccessManager` at boot, and (legacy only) the 80/443/47999 forwards. |
| `unique_id` | string | generated | 8-hex-char instance id. Internal only on a fresh install: it seeds the deterministic UPnP fallback port so two instances on one LAN never collide. On a legacy instance it is also the subdomain label (reserved labels rejected). |
| `registered_uid` | string | — | **The legacy marker.** Set on the first successful subdomain registration, never cleared; its presence is what keeps the DNS/ACME machinery running (and lets a `unique_id` change release the old record). Absent on every fresh install. |
| `domain` | string | `"MW_DOMAIN"` | The sentinel means "computed" — `{unique_id}.{MW_DOMAIN}`, which only exists on a legacy instance (a fresh install has no public name). Replace it with **your own FQDN** to serve the app under it — the value is then kept verbatim and Internet Access switches to its network-only mode ([§7.5](#75-bring-your-own-domain--certificate)). Anything that is not a valid FQDN falls back to the computed name. |
| `public_ip` | string | — | Resolved public IP (STUN or manual). Also feeds the NAT-hairpin test and CGNAT detection. |
| `auto_ip_detection` | bool | `true` | STUN/HTTP auto-detect vs manual `public_ip`. |
| `pending_registration` | bool | `false` | Legacy only: set when the subdomain registration failed offline; retried at startup. |
| `owner_token` | string | generated | Legacy only: random token sent as the `X-MW-Owner` header on every DNS write — the HMAC of it is what the proxy stores as proof of subdomain ownership. Never published in DNS. |
| `internet_consent` | object | `{}` | `{message, at, source, version, mechanism}` — the exact opt-in agreement record. A record without `version` predates the retirement and was worded for the DNS mechanism (version 1): a non-legacy instance then waits in phase `consent_required` until the user agrees to the current wording (version 2). |

### TLS & auth

| Key | Type | Default | Description |
|---|---|---|---|
| `cert_pem` / `cert_key` | string | `"MW_CERT_PEM"` / `"MW_CERT_KEY"` | Certificate/key **source**: an env-var *name* or a *file path*. ACME issuance rewrites these to the issued file paths. The certificate must cover `domain` (**CN or SAN**, wildcards included) or it is skipped in favour of the `cert/` directory scan. |
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
| `MW_DOMAIN` | Legacy Internet Access | Parent domain hosted on the PowerDNS box. Fallback default: `moonlightweb.top`. Only consulted by an instance that still holds a legacy subdomain. |
| `MW_PDNS_URL` | Legacy Internet Access | DNS-registration API base URL. 0.2.0+ points at the restricted gateway: `https://dnsapi.{MW_DOMAIN}/api/v1/servers/localhost`. Defaults to `https://api.{MW_DOMAIN}/api/v1/servers/localhost` (legacy direct PowerDNS) when unset. |
| `MW_PDNS_TOKEN` | Legacy Internet Access | API key (`X-API-Key`) for the `dnsapi.` gateway: the **restricted** `MW_PDNS_PROXY_KEY`, which manages only this instance's own records. **Secret.** |
| `MW_ACME_DIRECTORY` | optional | ACME directory URL. Defaults to Let's Encrypt, or ZeroSSL DV90 when EAB creds are set. |
| `MW_ZEROSSL_EAB_KID` / `MW_ZEROSSL_EAB_HMAC` | optional | ZeroSSL External Account Binding. **Secrets.** |
| `MW_CERT_PEM` / `MW_CERT_KEY` | optional | Inline PEM cert/key (the default `cert_pem`/`cert_key` settings point at these env-var names). |
| `MW_SERVICE` | service installs | Set by service supervisors: suppresses browser/tray/shortcut behavior and port-mapping take-overs. |
| `MW_MEDIA_QUEUED_VIDEO` | debug | `1` rolls `MediaTrackRelay` back to the old queued-video send path (default: send from the capture thread). |

### Build-time embedded fallbacks

CI bakes `MW_DOMAIN`, `MW_PDNS_URL`, `MW_PDNS_TOKEN`, `MW_ZEROSSL_EAB_*` (from repo secrets, via CMake defines) into release binaries. `applyEmbeddedEnvDefaults()` applies them **only when the runtime env/.env did not set them** — so distributed builds work out of the box with the shared domain, while forks/self-hosters override cleanly. The admin UI gates Internet-Access controls on the config being *active*.

## 7.4 Browser-side preferences (`localStorage`)

| Key | Description |
|---|---|
| `mw-streaming-settings` | The whole Settings overlay state as one JSON object (`SettingsView`): mirrors of the streaming defaults above, sent with each `/start`, **plus** per-device-only fields — `touch_sensitivity`, `touch_screen` (absolute touch instead of the trackpad model — both exposed on mobile/tablet only, a touchscreen laptop keeps the mouse/trackpad path), `tearing_enabled` (off = VSync pacing; Chromium desktop only), `video_worker` (`auto`\|`on`\|`off` — OffscreenCanvas decode/render worker, `auto` by default), `power_save` + `power_save_backup` (mobile light pipeline), `seamless_switching`. |
| `mw-lang` | UI language (en/fr/zh). |
| `mw_client_uniqueid` | Per-browser Sunshine identity (session isolation); the standby stream slot derives its own id from it. |
| `mw_jitter_auto` | Adaptive jitterBufferTarget toggle (webrtc-media). |
| `mw_pacing` | Adaptive presentation reserve (DataChannel/WSS paths — the `FramePacer` de-jitter clock). Off by default (freshest-frame-first: on real Wi-Fi the reserve legitimately sits near its 25ms cap, and latency wins over smoothness); set to `1` to opt in. The reserve is shown live in the latency breakdown ("jitter reserve" — the row only exists while the pacer runs), and is read once per stream start — set it, then relaunch the stream. |
| `mw_hdr_tonemap` | `1/0` override of the automatic ACES tone-map; `mw_hdr_curve` / `mw_hdr_exposure` / `mw_hdr_refwhite` tune it. |
| `mw_force_2d` | Force the Canvas2D renderer (skip the WebGPU probe). |
| `mw_setup_dismissed` / `mw_update_dismissed` | Banner dismissals (setup nudge, update banner). |

## 7.5 Bring your own domain & certificate

Bringing your own domain is **the** way to put the web interface itself on the internet now that the shared-subdomain automation is retired: set `domain` in `settings.json` and the manager runs in a **network-only mode** — public-IP detection and the NAT-hairpin test still run, DNS registration and ACME issuance never do (the zone and the certificate are yours), and the 443 forward on your router is yours to create.

**1. DNS — A record on your public IP.** In `mywebsite.com`'s zone create `stream` → **your router's public IPv4** (the WAN address, *not* the machine's `192.168.x.x`). Check it with `curl https://api.ipify.org` on the host, or read *Public IP* on the admin page. IPv6-only reachability needs an AAAA record instead. A dynamic ISP address needs a DDNS updater; a **CGNAT** address (`100.64.0.0/10`) can never be port-forwarded — the admin page detects and reports that case.

**2. Router — port forwarding with port parity.** External port **must equal** internal port: the browser builds every REST/WebSocket URL from the origin it was served on, so a rewritten port breaks the signaling URL.

| Forward | To | Why |
|---|---|---|
| **TCP 443** (mandatory) | `<LAN IP of the MoonlightWeb machine>`:443 | Page, REST API, WSS signaling and the `wss` fallback transport all ride the single HTTPS port. |
| TCP 80 (optional) | same host:80 | HTTP→HTTPS redirect; also needed if you renew with an HTTP-01 ACME client on that machine. |
| UDP 48010–48014 (recommended) | same host, same ports | Direct WebRTC media (`SignalingServer` maps the first free port of that range). Without it the fallback chain still works over ICE-TCP, then `wss`, at a worse latency profile. |

With `upnp_enabled: true` **and** Internet Access on, the per-session media ports (UDP 48010-48014) are still mapped for you; the TCP 443 (and optional 80) rules above are always manual with a custom domain.

**3. Certificate.** Issue one for `stream.mywebsite.com` (certbot / win-acme / lego with your DNS provider, or any commercial CA). You need two **unencrypted PEM** files: the full chain and the private key (RSA or EC). A wildcard (`*.mywebsite.com`) works too.

**4. Declare the domain and install the certificate.** Stop the server, edit `settings.json` (path in [§7.1](#71-settingsjson-location)), then restart:

```jsonc
{
  "domain": "stream.mywebsite.com",
  // optional — absolute paths to your PEM files; omit to use the cert/ folder
  "cert_pem": "C:/certs/fullchain.pem",
  "cert_key": "C:/certs/privkey.pem"
}
```

Without `cert_pem`/`cert_key`, drop both files (`*.pem`, same folder) into the data dir's `cert/` subfolder — `%APPDATA%\MoonlightWeb\MoonlightWeb\cert\` on Windows, `~/Library/Application Support/MoonlightWeb/MoonlightWeb/cert/` on macOS, `~/.local/share/MoonlightWeb/MoonlightWeb/cert/` on Linux. `CertManager` scans recursively for a certificate that **covers the domain (CN or SAN, wildcards included)** plus a private key beside it.

Confirm in `logs/moonlightweb.log`: `SSL certificate loaded: CN=stream.mywebsite.com`. **Renewal is yours**: replace the files and restart. A certificate near expiry is kept and logged as *renew it soon* — it is never silently downgraded to a self-signed one.

**5. Internet Access — optional.** With a custom domain it registers nothing and requests no certificate; it re-detects the public IP, tests NAT hairpin, allows the per-session media mapping, and points the tray/shortcut entry URLs at your domain — but only when that hairpin test succeeds. Without it the host's own entry points stay on `https://localhost:<port>`, which is the address that works from the machine itself; every other device still uses the domain. Turn it off to manage everything by hand. `GET /api/internet/status` reports `custom_domain: true` in that mode.

Authentication is unchanged: remote devices still need the admin PIN or the certificate-auth token file (see [Security](06-Security.md)).

**Caveats**

- Reaching `stream.mywebsite.com` from *inside* your own LAN requires NAT-hairpin support on the router; otherwise use `https://<LAN IP>` at home (self-signed warning) or add a split-horizon DNS entry. The host machine itself needs nothing: the tray, the shortcut and the auto-open fall back to loopback on their own.
- `unique_id` keeps existing (it is this instance's identity) but is never registered while a custom domain is set. Restoring `"domain": "MW_DOMAIN"` brings the computed name back — on a legacy instance only; a fresh install then simply has no public name again.
- The `stream` label is only reserved on the project's shared domain, never on yours.

---

[← Security](06-Security.md) · [Home](Home.md) · [Next: REST API →](08-REST-API.md)
