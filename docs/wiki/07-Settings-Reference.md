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
| `transport` | string | `"webrtc"` | Legacy toggle: `"webrtc"` or `"wss"` (StreamRelay diagnostics). |
| `transport_mode` | string | `"auto"` | Transport for streams: `auto` \| `webrtc-media-udp` \| `webrtc-dc-udp` \| `webrtc-media-tcp` \| `webrtc-dc-tcp` \| `wss`. `auto` = fallback chain (see [Transports](05-Streaming-and-Transports.md)). |
| `stun_server` | string | `"stun:stun.l.google.com:19302"` | Used by both libdatachannel and the browser's `RTCPeerConnection`. |
| `upnp_enabled` | bool | `true` | UPnP port mapping for NAT traversal. |

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
| `audio_time_stretch` | bool | `true` | **File-only** (no UI): WSOLA pitch-preserving stretch in the AudioWorklet. |

### Internet Access

| Key | Type | Default | Description |
|---|---|---|---|
| `internet_access_enabled` | bool | `false` | Auto-starts `InternetAccessManager` at boot when true. |
| `unique_id` | string | generated | 8-hex-char subdomain label. Reserved labels rejected. |
| `registered_uid` | string | — | Last subdomain actually registered (used to release the old one on change). |
| `domain` | string | computed | If a valid FQDN is stored, used as-is (**bring-your-own-domain**); otherwise computed as `{unique_id}.{MW_DOMAIN}`. |
| `public_ip` | string | — | Resolved public IP (STUN or manual). |
| `auto_ip_detection` | bool | `true` | STUN/HTTP auto-detect vs manual `public_ip`. |
| `pending_registration` | bool | `false` | Set when registration failed offline; retried at startup. |
| `owner_token` | string | generated | Random token written to the `_owner.<uid>` TXT record (subdomain ownership). |
| `internet_consent` | object | `{}` | `{message, at, source}` — the exact opt-in agreement record. |

### TLS & auth

| Key | Type | Default | Description |
|---|---|---|---|
| `cert_pem` / `cert_key` | string | `"MW_CERT_PEM"` / `"MW_CERT_KEY"` | Certificate/key **source**: an env-var *name* or a *file path*. ACME issuance rewrites these to the issued file paths. For a manual cert set both paths yourself (CN must match `domain`; no auto-renew). |
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
| `MW_PDNS_URL` | Internet Access | Full PowerDNS API base URL. Defaults to `https://api.{MW_DOMAIN}/api/v1/servers/localhost`. |
| `MW_PDNS_TOKEN` | Internet Access | PowerDNS API key (`X-API-Key`). **Secret.** |
| `MW_ACME_DIRECTORY` | optional | ACME directory URL. Defaults to Let's Encrypt, or ZeroSSL DV90 when EAB creds are set. |
| `MW_ZEROSSL_EAB_KID` / `MW_ZEROSSL_EAB_HMAC` | optional | ZeroSSL External Account Binding. **Secrets.** |
| `MW_CERT_PEM` / `MW_CERT_KEY` | optional | Inline PEM cert/key (the default `cert_pem`/`cert_key` settings point at these env-var names). |
| `MW_SERVICE` | service installs | Set by service supervisors: suppresses browser/tray/shortcut behavior and port-mapping take-overs. |
| `MW_FRAME_DUMP` | debug | `1` dumps raw video frames. |

### Build-time embedded fallbacks

CI bakes `MW_DOMAIN`, `MW_PDNS_URL`, `MW_PDNS_TOKEN`, `MW_ZEROSSL_EAB_*` (from repo secrets, via CMake defines) into release binaries. `applyEmbeddedEnvDefaults()` applies them **only when the runtime env/.env did not set them** — so distributed builds work out of the box with the shared domain, while forks/self-hosters override cleanly. The admin UI gates Internet-Access controls on the config being *active*.

## 7.4 Browser-side preferences (`localStorage`)

Per-browser keys managed by `SettingsView` (mirrors of the streaming defaults above, sent with each `/start`), plus notable debug/feature flags:

| Key | Description |
|---|---|
| `mw-lang` | UI language (en/fr/zh). |
| `client_uniqueid` | Per-browser Sunshine identity (session isolation). |
| `mw_jitter_auto` | Adaptive jitterBufferTarget toggle (webrtc-media). |
| `mw_video_worker` | `1` = OffscreenCanvas decode/render worker. |
| `mw_hdr_tonemap` | `1/0` override of the automatic ACES tone-map. |

---

[← Security](06-Security.md) · [Home](Home.md) · [Next: REST API →](08-REST-API.md)
