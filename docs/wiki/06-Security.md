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

Losing the host session (revoked, expired, cookies cleared) leaves the host machine looking like any other visitor under the public domain — a PIN prompt, on the very machine that owns the server. Nothing in the request can safely say otherwise, so the way back is to go through the one address that proves where the browser runs: the login page offers a link to `https://localhost`, which loads with local privilege and bounces straight back to the domain carrying a fresh `?mwk=` (`app.js _maybeRedirectToDomain`). The tray icon and the Desktop shortcut do the same thing, for the same reason.

Requests from the machine itself bypass authentication entirely and are, besides host sessions and the remote admin password below, the only way to use `/api/admin/*`. "From the machine itself" takes two signals, never one: the peer address must be loopback or one of our own interfaces (`HttpServer::isLocalRequest`), **and** the `Host` header must be a name only reachable from here — loopback, a private/link-local address, or `.local` (`RequestGuard::isLocalHostName`). The public domain is excluded from that second list on purpose: a TLS-terminating tunnel runs on this machine and forwards every Internet visitor from loopback under exactly that name, so the pair would otherwise describe the whole Internet. The host machine reaching itself through the domain proves it with the host key instead (`?mwk=`, below), which the parity redirect carries automatically.

### 6.2.1 Remote admin password (LAN only)

Admin access is otherwise granted by *address*: the host machine has it, nobody else ever does. That leaves the operator stuck when the host is a headless box, a machine they are not sitting at, or simply a PC in another room. The remote admin password is the second door.

Three states, in `AppSettings`: **unset** (remote admin on, no digest stored — the door is advertised but opens for nobody), **set** (a digest stored), **disabled** (`remote_admin_enabled: false`, nothing is accepted).

- **There is no built-in default password.** A documented default is public knowledge, and while the unlock only ever *promotes an already-authenticated LAN session*, that still means "whoever can stream can also administer" on every install whose owner never changed it. So the door ships shut and `validateAdminPassword` matches nothing until a password exists. Setting one takes the host, which is exactly what the two entry points cover:
  - **with a desktop**, from the admin page, which the owner can always open locally;
  - **headless**, from the terminal — `moonlightweb --set-admin-password`, which the installer prompts for during a headless install, and which `--status` reports as unset until then.
- **Changed from the admin page or the CLI** (`POST /api/admin/password`, admin-gated). Stored as **PBKDF2-SHA256, 210 000 iterations, 16-byte random salt** — the plaintext is never written, so a forgotten password is *reset*, never recovered. Minimum 8 characters (400 `too_short`).
- **Spent by an already-authenticated LAN device** (`POST /api/auth/admin-unlock`). It **promotes an existing session** rather than creating one: a device still has to pass the PIN first. The password raises what an admitted device may do; it never admits anyone.
- Two independent gates keep it off the Internet, because either one alone is forgeable in a deployment we support:
  - the peer address must be LAN (`AuthManager::isLanAddress` — loopback, RFC 1918, link-local, `fc00::/7`), **and**
  - the `Host` header must name this machine (`RequestGuard::isTrustedHost`). A TLS-terminating tunnel makes every visitor look like `127.0.0.1`, which the peer check alone would accept; a hairpin-NAT LAN client arrives under the public domain, which the Host check alone would accept for Internet visitors too. Both together admit exactly the LAN.
- Failed attempts use **their own rate-limit bucket** (`admin|<ip>`), on the same 3/5/10 → 30 s/2 min/10 min tiers, so hammering the password cannot lock a legitimate user out of PIN login (or vice versa).
- **Changing the password, or disabling remote administration, revokes every unlock the old one bought** (`demoteAdminSessions`) — except the caller's own when they are the remote admin doing the reset. Sessions survive; only the admin flag is dropped. Disabling and re-enabling restores the operator's password: the digest is kept. A persisted `is_admin` is discarded at load whenever the door is shut — remote administration off, or no password set.
- An unlocked LAN admin gets `isLocal` (full admin), but **not** `isHostMachine`: it never receives the host key and never sees the first-run setup wizard, both of which belong to the machine with the local desktop.

## 6.3 Sessions

- A successful auth issues a random token in the `mw_session` cookie. **Only its SHA-256 (base64url) is stored** — in memory and in `sessions.json` — so a stolen sessions file cannot be replayed.
- **Sliding expiration**: 90 days of inactivity (`SESSION_TTL_SECS`); any authenticated request bumps `lastSeen`. Expired sessions are purged periodically; persisted sessions older than 24 h without a live process are discarded on load.
- Sessions carry IP, machine name (renameable), geolocation (async `GeoIpService` lookup, refreshed when the IP changes), a `streaming` flag (single active stream), `is_host`, and `is_admin` (unlocked with the remote admin password — see §6.2.1).
- **Revocation is immediate**: destroying a session that is actively streaming emits `streamingSessionRevoked`, which tears the live relay down and cancels the browser's Sunshine session (kill-switch in `main.cpp`).
- Regenerating the PIN invalidates **all** sessions.
- HMAC signing key: generated once, persisted (`hmac_key` in settings) so sessions survive restarts.

## 6.4 Brute-force & flood mitigation

Two cooperating layers:

1. **`AuthManager` secret rate limiting** — per rate-limit bucket (raw IPv4, or /64 prefix for IPv6 — a client trivially owns a whole /64): 3 failures → escalating lockouts (30 s → 2 min → 10 min), remaining attempts surfaced to the UI. Constant-time comparisons throughout. The PIN and the remote admin password each get their own bucket per address.
2. **`ConnectionGuard`** — in-process fail2ban equivalent (fail2ban itself is Linux-only), checked at `accept()` time so banned IPs cost no TLS handshake:
   - Connection flood: > 200 new TCP connections / 10 s / IP (generous because the server is `Connection: close` — one page load bursts dozens of connections).
   - Auth-failure flood: > 10 rejected (401) requests / 60 s / IP.
   - Both arm a **10-minute temporary ban**; entries idle 5 min are purged.
   - **Loopback and private addresses (RFC 1918 / ULA / link-local) are fully exempt** — LAN clients are trusted.

## 6.5 TLS

- **LAN**: a self-signed certificate is generated on first run (browser shows a one-time warning — inherent to self-signed TLS).
- **Internet Access**: a real certificate is issued via the native **ACMEv2 client** (`AcmeClient`) with the **DNS-01 challenge** through the PowerDNS API — ZeroSSL DV90 when EAB credentials are configured, Let's Encrypt otherwise. Renewal below 30 days remaining; `certificateChanged` performs a **hot TLS reload** (no restart, new connections get the new cert).
- **Bring your own**: set `domain` to your FQDN and point `cert_pem`/`cert_key` at your PEM files (or drop them in the data dir's `cert/` folder). A certificate is accepted when it covers the domain by **CN or SAN**, wildcards included; renewal and lifecycle are the user's, and a certificate close to expiry is kept, never downgraded to self-signed. Full procedure, with the DNS/router side: [Settings Reference §7.5](07-Settings-Reference.md#75-bring-your-own-domain--certificate).
- Qt's TLS backend is forced to **OpenSSL** (Windows Schannel cannot import ACME PEM keys — it would silently fall back to the self-signed cert and break the public domain).
- Historical ACME pitfalls fixed and guarded: finalize-URL handling, self-signed↔ACME key collision, `loadCertFiles` ordering, hot reload.

## 6.6 DNS subdomain ownership

Two instances (or a malicious actor) must never overwrite each other's A record. Ownership is enforced **server-side** by the `mw-proxy` gateway (0.2.0+; see [PowerDNS Stack §10.7](10-PowerDNS-Stack.md)) rather than cooperatively:

- Each instance holds a random per-instance `owner_token`, sent as the **`X-MW-Owner` header** on every write (`PdnsClient`/`AcmeClient`). It is **never published in DNS**, so it cannot be read via `dig` and replayed.
- The first writer of a `uid` claims it (Trust-On-First-Use); `mw-proxy` stores `HMAC(owner_token)` keyed by `uid` and rejects (`403`) any later write whose header does not match. **One subdomain per owner** is enforced, and the restricted key can only touch `{uid}` A / `_owner` / `_acme-challenge` records (never NS/SOA/DNSSEC/other zones).
- Reserved labels (`www`, `api`, `dnsapi`, `stats`, `stream`, `ns1/ns2`, `mail`, apex, anything starting `_`) are rejected as `unique_id` values — backend-side (`isReservedSubdomain`), by a boot-time guard against `settings.json` edits, and again by mw-proxy's 8-hex `uid` rule.
- Changing `unique_id` releases the previous subdomain (deleting its A record frees the ownership entry) so one owner never holds two live subdomains.

## 6.7 Internet-access consent & audit

Enabling Internet Access is an **explicit opt-in** (checkbox unchecked everywhere: installer, setup wizard, admin page). The exact agreement text, timestamp (ISO-8601 UTC) and entry point (`admin` | `setup` | `installer`) are persisted (`internet_consent` in settings), and **every A-record registration request appends a JSONL audit entry** referencing that consent — legal traceability for exposing a user's machine publicly.

## 6.8 Other hardening

- `/api/admin/*` → 403 unless localhost/host-session; the internet/setup/system/local routes re-check the same `req.isLocal` predicate inside their handlers.
- Input to Sunshine is encrypted (AES-128-GCM, per-session key) as per GameStream.
- Private LAN ICE candidates are advertised **only to clients detected as local** — internet peers never learn the LAN IP.
- Per-browser `client_uniqueid` values are sanitized to hex (max 32 chars) before reaching launch URLs.
- Session cookies are the only client-held secret; settings/keys/certs are server-side only.
- Frontend escapes all interpolated HTML (`escapeHtml.js`).
- CI embeds DNS/ACME secrets (`MW_*`) into the binary at build time — installers ship **no editable secrets** on disk; runtime env/.env still override.
- **Distribution integrity**: Windows binaries and the installer are Authenticode-signed via SignPath; the APT repository is trusted through a signed `InRelease`, the DNF one through both a signed `repomd.xml` and per-package `rpmsign` signatures; the Homebrew cask and AUR package pin a sha256 of the exact release asset. The signing key lives only in the `GPG_PRIVATE_KEY` CI secret and the maintainer's keyring — `*.asc` is gitignored so an exported half can never be staged. Details in [Installers & Packaging §9.1–9.3](09-Installers-and-Packaging.md).
- The DNS stack has its own hardening (rate limits, non-root containers, no published API port) — see [PowerDNS Stack](10-PowerDNS-Stack.md).
- Backend security-focused tests exist (`backend/tests/security_main.cpp`, `test_auth_manager.cpp`, `test_connection_guard.cpp`, `test_input_crypto.cpp`).

---

[← Streaming & Transports](05-Streaming-and-Transports.md) · [Home](Home.md) · [Next: Settings Reference →](07-Settings-Reference.md)
