[← Streaming & Transports](05-Streaming-and-Transports.md) · **Security** · [Next: Settings Reference →](07-Settings-Reference.md)

---

# 6. Security

MoonlightWeb exposes a streaming server to the LAN and, optionally, to the public Internet. The security model is layered: transport security (TLS), authentication (PIN / certificate file / host key), session management, abuse mitigation, and legal traceability.

## 6.1 Threat model in one paragraph

The server may be reachable from the Internet on `https://{uniqueId}.{MW_DOMAIN}`. Attackers can: scan/guess the PIN, flood connections, replay stolen artifacts, or try to hijack another instance's subdomain. Trusted parties: the local machine (localhost is always admin), LAN clients (exempt from bans, still need a PIN when remote-auth applies), the DNS box operator. Out of scope: volumetric DDoS (see [PowerDNS Stack §10.7](10-PowerDNS-Stack.md)), a compromised host OS.

## 6.2 Authentication

Three ways to obtain a session, all implemented in `AuthManager` + `AuthRoutes`:

| Method | Flow | Properties |
|---|---|---|
| **PIN** | Admin generates an 8-char PIN on the admin page → remote user enters it once (`POST /api/auth/validate`) | Single-use in practice: a consumed PIN **auto-regenerates** and the admin UI shows `--------` until a new one is explicitly generated. Rate-limited per IP. |
| **Certificate file** | Admin downloads a token file (`/api/admin/certificate/download`, 64+ random bytes Base64) → remote user uploads it instead of typing a PIN | Optional (`cert_auth_enabled`), revocable by regeneration. Compared in constant time. |
| **Host key** (`?mwk=`) | The host machine's own entry points (tray, Desktop shortcut, startup open) embed a long random key in the URL. Redeeming it (`POST /api/auth/host-key`) proves the browser runs *on the host* even when reached via the public domain (peer address is the router, not loopback) | Grants a **host session** (localhost-equivalent, admin-capable). **Single-use**: each redemption rotates the key and rewrites every entry point, so a leaked URL cannot be replayed. |

`localhost` requests (loopback peer address) bypass authentication entirely and are the only way (besides host sessions) to use `/api/admin/*`.

## 6.3 Sessions

- A successful auth issues a random token in the `mw_session` cookie. **Only its SHA-256 (base64url) is stored** — in memory and in `sessions.json` — so a stolen sessions file cannot be replayed.
- **Sliding expiration**: 90 days of inactivity (`SESSION_TTL_SECS`); any authenticated request bumps `lastSeen`. Expired sessions are purged periodically; persisted sessions older than 24 h without a live process are discarded on load.
- Sessions carry IP, machine name (renameable), geolocation (async `GeoIpService` lookup, refreshed when the IP changes), a `streaming` flag (single active stream), and `is_host`.
- **Revocation is immediate**: destroying a session that is actively streaming emits `streamingSessionRevoked`, which tears the live relay down and cancels the browser's Sunshine session (kill-switch in `main.cpp`).
- Regenerating the PIN invalidates **all** sessions.
- HMAC signing key: generated once, persisted (`hmac_key` in settings) so sessions survive restarts.

## 6.4 Brute-force & flood mitigation

Two cooperating layers:

1. **`AuthManager` PIN rate limiting** — per rate-limit bucket (raw IPv4, or /64 prefix for IPv6 — a client trivially owns a whole /64): 3 failures → escalating lockouts (30 s → 2 min → 10 min), remaining attempts surfaced to the UI. Constant-time comparisons throughout.
2. **`ConnectionGuard`** — in-process fail2ban equivalent (fail2ban itself is Linux-only), checked at `accept()` time so banned IPs cost no TLS handshake:
   - Connection flood: > 200 new TCP connections / 10 s / IP (generous because the server is `Connection: close` — one page load bursts dozens of connections).
   - Auth-failure flood: > 10 rejected (401) requests / 60 s / IP.
   - Both arm a **10-minute temporary ban**; entries idle 5 min are purged.
   - **Loopback and private addresses (RFC 1918 / ULA / link-local) are fully exempt** — LAN clients are trusted.

## 6.5 TLS

- **LAN**: a self-signed certificate is generated on first run (browser shows a one-time warning — inherent to self-signed TLS).
- **Internet Access**: a real certificate is issued via the native **ACMEv2 client** (`AcmeClient`) with the **DNS-01 challenge** through the PowerDNS API — ZeroSSL DV90 when EAB credentials are configured, Let's Encrypt otherwise. Renewal below 30 days remaining; `certificateChanged` performs a **hot TLS reload** (no restart, new connections get the new cert).
- **Bring your own**: `domain` + `cert_pem`/`cert_key` in `settings.json` (file paths or env-var names); the CN must match the domain, lifecycle is the user's.
- Qt's TLS backend is forced to **OpenSSL** (Windows Schannel cannot import ACME PEM keys — it would silently fall back to the self-signed cert and break the public domain).
- Historical ACME pitfalls fixed and guarded: finalize-URL handling, self-signed↔ACME key collision, `loadCertFiles` ordering, hot reload.

## 6.6 DNS subdomain ownership

Two instances (or a malicious actor) must never overwrite each other's A record:

- Each instance holds a random `owner_token`; a **`_owner.<uid>` TXT record** on the DNS zone must match (or be absent, then claimed) before the A record is created/updated.
- Reserved labels (`www`, `api`, `stats`, `stream`, `ns1/ns2`, `mail`, apex, anything starting `_`) are rejected as `unique_id` values — both backend-side (`isReservedSubdomain`) and by a boot-time guard against `settings.json` edits.
- Changing `unique_id` releases the previous subdomain (delete A + `_owner` TXT, only after verifying ownership) so one owner never holds two live subdomains.

## 6.7 Internet-access consent & audit

Enabling Internet Access is an **explicit opt-in** (checkbox unchecked everywhere: installer, setup wizard, admin page). The exact agreement text, timestamp (ISO-8601 UTC) and entry point (`admin` | `setup` | `installer`) are persisted (`internet_consent` in settings), and **every A-record registration request appends a JSONL audit entry** referencing that consent — legal traceability for exposing a user's machine publicly.

## 6.8 Other hardening

- `/api/admin/*` → 403 unless localhost/host-session; `/api/local/focus` → loopback only (`req.isLocal`).
- Input to Sunshine is encrypted (AES-128-GCM, per-session key) as per GameStream.
- Private LAN ICE candidates are advertised **only to clients detected as local** — internet peers never learn the LAN IP.
- Per-browser `client_uniqueid` values are sanitized to hex (max 32 chars) before reaching launch URLs.
- Session cookies are the only client-held secret; settings/keys/certs are server-side only.
- Frontend escapes all interpolated HTML (`escapeHtml.js`).
- CI embeds DNS/ACME secrets (`MW_*`) into the binary at build time — installers ship **no editable secrets** on disk; runtime env/.env still override.
- The DNS stack has its own hardening (rate limits, non-root containers, no published API port) — see [PowerDNS Stack](10-PowerDNS-Stack.md).
- Backend security-focused tests exist (`backend/tests/security_main.cpp`, `test_auth_manager.cpp`, `test_connection_guard.cpp`, `test_input_crypto.cpp`).

---

[← Streaming & Transports](05-Streaming-and-Transports.md) · [Home](Home.md) · [Next: Settings Reference →](07-Settings-Reference.md)
