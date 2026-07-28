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
| `http_port` | int | `80` | HTTP listener (redirects to HTTPS). CLI `--port` overrides. The *actually bound* port is persisted back after fallback. |
| `https_port` | int | `443` | HTTPS listener preference; fallback ranges are tried, the bound port is persisted back. |
| `transport_mode` | string | `"auto"` | Transport for streams: `auto` \| `webrtc-media-udp` \| `webrtc-dc-udp` \| `webrtc-media-tcp` \| `webrtc-dc-tcp` \| `wss`. `auto` = fallback chain (see [Transports](05-Streaming-and-Transports.md)). Written by `POST /api/internet/enable` (the admin page's transport selector). |
| `stun_server` | string | `"stun:stun.l.google.com:19302"` | Used by both libdatachannel and the browser's `RTCPeerConnection`. |
| `upnp_enabled` | bool | `true` | UPnP port mapping for NAT traversal. |
| `stream_worker_enabled` | bool | `true` | **File-only** (no UI), seeded at startup. Each session runs in a `--stream-worker` child process → two concurrent stream slots + crash isolation (see [Streaming §5.6](05-Streaming-and-Transports.md#56-session-lifecycle--teardown-discipline)). `false` = legacy in-process mode, single stream. |

### Streaming defaults (overridable per-request by the browser)

| Key | Type | Default | Description |
|---|---|---|---|
| `video_codec` | string | `"auto"` | `auto` \| `h264` \| `hevc` \| `av1`. `auto` resolves to HEVC if the host supports it, else H.264. |
| `stream_bitrate` | int (kbps) | `20000` | 5 000–150 000. |
| `stream_height` | int | `1080` | 720/1080/1440/2160, or `0` = *Native Host* (largest reported display mode). Width is derived. |
| `stream_aspect` | string | `"auto"` | `auto` (host's native format) or explicit `16:9` / `21:9` / `32:9`. |
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

| Key | Type | Default | Description |
|---|---|---|---|
| `internet_access_enabled` | bool | `false` | Auto-starts `InternetAccessManager` at boot when true. |
| `unique_id` | string | generated | 8-hex-char subdomain label. Reserved labels rejected. |
| `registered_uid` | string | — | Last subdomain actually registered (used to release the old one on change). |
| `domain` | string | `"MW_DOMAIN"` | The sentinel means "computed": `{unique_id}.{MW_DOMAIN}`. Replace it with **your own FQDN** to serve the app under it — the value is then kept verbatim and Internet Access switches to its network-only mode ([§7.5](#75-bring-your-own-domain--certificate)). Anything that is not a valid FQDN falls back to the computed name. |
| `public_ip` | string | — | Resolved public IP (STUN or manual). |
| `auto_ip_detection` | bool | `true` | STUN/HTTP auto-detect vs manual `public_ip`. |
| `pending_registration` | bool | `false` | Set when registration failed offline; retried at startup. |
| `owner_token` | string | generated | Random token written to the `_owner.<uid>` TXT record (subdomain ownership). |
| `internet_consent` | object | `{}` | `{message, at, source}` — the exact opt-in agreement record. |

### TLS & auth

| Key | Type | Default | Description |
|---|---|---|---|
| `cert_pem` / `cert_key` | string | `"MW_CERT_PEM"` / `"MW_CERT_KEY"` | Certificate/key **source**: an env-var *name* or a *file path*. ACME issuance rewrites these to the issued file paths. The certificate must cover `domain` (**CN or SAN**, wildcards included) or it is skipped in favour of the `cert/` directory scan. |
| `hmac_key` | string (Base64) | generated | Session-token HMAC key, persisted so sessions survive restarts. |
| `certificate_token` | string | generated first boot | The downloadable auth token file content. |
| `cert_auth_enabled` | bool | `false` | Enable certificate-file authentication. |
| *(host key)* | string | generated | Single-use host-machine key embedded as `?mwk=` in host-side entry URLs; rotates on redemption. |

### Lifecycle

| Key | Type | Default | Description |
|---|---|---|---|
| `setup_completed` | bool | `false` | First-run wizard done. Windows: set by the installer's provisioning flow; macOS/Linux: by the in-app `/setup` wizard. While false on a GUI launch, the browser opens `/setup`. |

## 7.3 `.env` — environment configuration

Loaded at startup by `loadEnvFile()` (`.env` next to the executable, else the project root; values quoted or multi-line PEM blocks supported). Reference: `.env.example` at the repo root. All values are optional — without them the server runs LAN-only with a self-signed cert.

| Variable | Required for | Description |
|---|---|---|
| `MW_DOMAIN` | Internet Access | Parent domain hosted on the PowerDNS box. Fallback default: `moonlightweb.top`. |
| `MW_PDNS_URL` | Internet Access | DNS-registration API base URL. 0.2.0+ points at the restricted gateway: `https://dnsapi.{MW_DOMAIN}/api/v1/servers/localhost`. Defaults to `https://api.{MW_DOMAIN}/api/v1/servers/localhost` (legacy direct PowerDNS) when unset. |
| `MW_PDNS_TOKEN` | Internet Access | API key (`X-API-Key`) for the `dnsapi.` gateway: the **restricted** `MW_PDNS_PROXY_KEY`, which manages only this instance's own records. **Secret.** |
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
| `mw-streaming-settings` | The whole Settings overlay state as one JSON object (`SettingsView`): mirrors of the streaming defaults above, sent with each `/start`, **plus** per-device-only fields — `touch_sensitivity`, `touch_screen` (absolute touch instead of the trackpad model), `tearing_enabled` (off = VSync pacing; Chromium desktop only), `video_worker` (`auto`\|`on`\|`off` — OffscreenCanvas decode/render worker, `auto` by default), `power_save` + `power_save_backup` (mobile light pipeline), `seamless_switching`. |
| `mw-lang` | UI language (en/fr/zh). |
| `mw_client_uniqueid` | Per-browser Sunshine identity (session isolation); the standby stream slot derives its own id from it. |
| `mw_jitter_auto` | Adaptive jitterBufferTarget toggle (webrtc-media). |
| `mw_hdr_tonemap` | `1/0` override of the automatic ACES tone-map; `mw_hdr_curve` / `mw_hdr_exposure` / `mw_hdr_refwhite` tune it. |
| `mw_force_2d` | Force the Canvas2D renderer (skip the WebGPU probe). |
| `mw_setup_dismissed` / `mw_update_dismissed` | Banner dismissals (setup nudge, update banner). |

## 7.5 Bring your own domain & certificate

The one-click **Internet Access** flow is the *shared-domain* automation (subdomain on the project's PowerDNS + ACME DNS-01 + UPnP). If you own a domain, publish the server as `https://stream.mywebsite.com` with your own certificate instead: set `domain` in `settings.json`, and the manager switches to a **network-only mode** — public-IP detection, UPnP port mapping and NAT-hairpin test still run, DNS registration and ACME issuance never do (the zone and the certificate are yours).

**1. DNS — A record on your public IP.** In `mywebsite.com`'s zone create `stream` → **your router's public IPv4** (the WAN address, *not* the machine's `192.168.x.x`). Check it with `curl https://api.ipify.org` on the host, or read *Public IP* on the admin page. IPv6-only reachability needs an AAAA record instead. A dynamic ISP address needs a DDNS updater; a **CGNAT** address (`100.64.0.0/10`) can never be port-forwarded — the admin page detects and reports that case.

**2. Router — port forwarding with port parity.** External port **must equal** internal port: the browser builds every REST/WebSocket URL from the origin it was served on, so a rewritten port breaks the signaling URL.

| Forward | To | Why |
|---|---|---|
| **TCP 443** (mandatory) | `<LAN IP of the MoonlightWeb machine>`:443 | Page, REST API, WSS signaling and the `wss` fallback transport all ride the single HTTPS port. |
| TCP 80 (optional) | same host:80 | HTTP→HTTPS redirect; also needed if you renew with an HTTP-01 ACME client on that machine. |
| UDP 48010–48014 (recommended) | same host, same ports | Direct WebRTC media (`SignalingServer` maps the first free port of that range). Without it the fallback chain still works over ICE-TCP, then `wss`, at a worse latency profile. |

If the router speaks UPnP, keeping `upnp_enabled: true` **and** Internet Access on maps 80/443/UDP for you even with a custom domain (step 5); otherwise every rule above is manual.

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

**5. Internet Access — optional, and safe to leave on.** With a custom domain it registers nothing and requests no certificate; it only keeps the router mapping fresh (UPnP), re-detects the public IP, tests NAT hairpin, and points the tray/shortcut entry URLs at your domain. Turn it off to manage the forwarding entirely by hand. `GET /api/internet/status` reports `custom_domain: true` in that mode.

Authentication is unchanged: remote devices still need the admin PIN or the certificate-auth token file (see [Security](06-Security.md)).

**Caveats**

- Reaching `stream.mywebsite.com` from *inside* your own LAN requires NAT-hairpin support on the router; otherwise use `https://<LAN IP>` at home (self-signed warning) or add a split-horizon DNS entry.
- `unique_id` keeps existing (it is this instance's identity) but is never registered while a custom domain is set. Restoring `"domain": "MW_DOMAIN"` brings the shared-domain behaviour back.
- The `stream` label is only reserved on the project's shared domain, never on yours.

---

[← Security](06-Security.md) · [Home](Home.md) · [Next: REST API →](08-REST-API.md)
