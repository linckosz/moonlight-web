[← Installers & Packaging](09-Installers-and-Packaging.md) · **PowerDNS Stack** · [Next: Build, CI & Testing →](11-Build-CI-Testing.md)

---

# 10. PowerDNS Stack (`deploy/powerdns/`)

The DNS stack is the infrastructure side of **Internet Access**: an authoritative DNS server with a REST API on a domain you own, so every MoonlightWeb instance can self-register `{uniqueId}.{MW_DOMAIN}` and pass ACME DNS-01 challenges. It runs on a small Linux VM, fully decoupled from the app. The author operates one for the shared domain; anyone can self-host it.

## 10.1 Topology

```
Internet ─:53──────► [dnsdist] ──► pdns:5300          anti-amplification + DNS rate-limit
Internet ─:80/:443─► [caddy] ┬─ api.{domain} ───────► pdns:8081         LEGACY full-access API (0.1.x)
                             ├─ dnsapi.{domain} ────► mw-proxy:8080 ──► pdns:8081  restricted API (0.2.0+)
                             └─ stats.{domain} ─────► umami:3000       analytics dashboard
                     [pdns]     (internal, non-root)  PowerDNS authoritative + REST API
                     [mw-proxy] (internal)            least-privilege filtering gateway
                     [umami+db] (internal)            privacy-friendly web analytics
```

Six containers (one process each — the idiomatic Docker layout), defined in `docker-compose.yml`:

| Container | Image | Exposure | Role |
|---|---|---|---|
| `pdns` | official `powerdns/pdns-auth-49`, **unmodified** | internal only (DNS :5300, API :8081, fixed IP 172.28.0.10) | Authoritative zone (SQLite in the `pdns_data` volume) + REST API. Runs non-root, `cap_drop: ALL`. |
| `mw-proxy` | custom build: small Go gateway (stdlib only) | internal only (:8080) | Least-privilege filter in front of the pdns API: holds the full key, exposes a restricted key that can only manage a client's own A/TXT records. Store in `mwproxy_data`. See §10.8. |
| `dnsdist` | official `powerdns/dnsdist-19` | public :53 UDP+TCP | DNS front: per-client rate limit, ANY→TCP, forwards to pdns. |
| `caddy` | custom build: Caddy + `caddy-ratelimit` (xcaddy) | public :80/:443 | `api.{domain}` → pdns API (legacy); `dnsapi.{domain}` → mw-proxy (restricted); apex/`www`/`stream` → static `website/`; `stats.` → Umami. Auto Let's Encrypt. |
| `umami` | official Umami (postgres flavor) | internal (via caddy) | Cookieless analytics for the landing page. |
| `umami-db` | postgres 16-alpine | internal | Umami storage (`umami_db` volume). |

All containers: `no-new-privileges`, `pids_limit`, `mem_limit`; only dnsdist/caddy keep `NET_BIND_SERVICE`. The compose network uses a **fixed subnet** (172.28.0.0/24) because dnsdist's `newServer` needs a literal backend IP (it does not resolve container names).

## 10.2 PowerDNS configuration

The official image already ships `pdns.conf`, the gsqlite3 schema and the API wiring (rendered from `PDNS_AUTH_API_KEY` by its own startup wrapper). The stack adds only two mounted files:

- **`pdns/zz-mw.conf`** — hardening snippet merged via include-dir: `disable-axfr`, `version-string=anonymous`, default SOA content, internal ports.
- **`pdns/init.sh`** — idempotent zone bootstrap run as entrypoint (then `exec`s the official wrapper):
  - creates the zone if absent with A records for `@`, `www`, `stats`, `stream`, `ns1`, `ns2`, `api`, `dnsapi` → `MW_PUBLIC_IP`, NS records, then `secure-zone` (DNSSEC) + `rectify-zone`, and **prints the DS record** to submit to the registrar;
  - **backfills** missing records on pre-existing zones (`ensure_a` guards — note: a bare re-`add-record` would duplicate, hence the greps);
  - replaces the image's placeholder SOA when found.

`MW_PDNS_API_KEY` from `.env` maps to `PDNS_AUTH_API_KEY`, so the key reaches the official mechanism unchanged. This **full** key now stays server-side: pdns uses it, and `mw-proxy` injects it toward pdns. MoonlightWeb 0.2.0+ instead holds the **restricted** `MW_PDNS_PROXY_KEY` (§10.8). Per-instance subdomains (`{uid}` A records, `_acme-challenge.*` TXT) are managed at runtime by MoonlightWeb through the API — the stack never touches them.

## 10.3 dnsdist configuration (`dnsdist/dnsdist.conf`)

- Listens on :53 UDP+TCP (v4+v6); ACL widened to the whole Internet (authoritative server).
- Backend pinned **up** (`getServer(0):setUp()`) — active health checks probe a name pdns won't serve and would otherwise black-hole the single backend.
- **Anti-amplification**: `MaxQPSIPRule(50, 32, 64)` drops clients over ~50 qps (per /32 v4, /64 v6); `ANY` queries are forced to TCP (`TCAction`), defanging UDP reflection.
- Raised FD limit (16384) — dnsdist pre-allocates sockets for per-IP QPS tracking.

## 10.4 Caddy (`caddy/`)

- **Dockerfile**: xcaddy build adding the `caddy-ratelimit` plugin (this Go build is why the installer adds swap on 1 GiB VMs).
- **`entrypoint.sh`** renders `Caddyfile.tmpl` from env at boot: `@MW_DOMAIN@`, `@TLS_LINE@` (api vhost: empty = auto Let's Encrypt, or `tls /certs/...` when `MW_TLS_CERT`+`MW_TLS_KEY` are both set — user files take priority), `@SITE_TLS_LINE@` (site vhosts always get their own ACME cert, never the api-only files).
- Vhosts: `api.` → `reverse_proxy pdns:8081` (legacy full-access, 60 req/min/IP); `dnsapi.` → `reverse_proxy mw-proxy:8080` (restricted, 60 req/min/IP — §10.7); `stats.` → Umami; apex+`www`+`stream` → static site with `www`/`stream` 301-redirected to the apex (`stream.` is a marketing vanity alias).
- Certificates persist in the `caddy_data` volume.

## 10.5 The installer (`install.sh`)

One idempotent script (`sudo ./install.sh`, re-runnable, keeps an existing `.env`) that takes **any fresh Linux distro** (apt/dnf/yum/pacman/zypper/apk) to a running stack:

1. Detects the package manager; installs **Docker + compose plugin**.
2. Installs host security: **fail2ban** + unattended security updates.
3. Configures the **host firewall** (ufw/firewalld/nftables best-effort): 53 UDP+TCP, 80, 443, SSH.
4. **Frees port 53** — Ubuntu's `systemd-resolved` stub listener holds it.
5. Adds a **swap file on low-RAM hosts** (the xcaddy Go build can OOM at 1 GiB).
6. Interactive `.env`: required `MW_DOMAIN` + `MW_PUBLIC_IP` (auto-detected default); generates `MW_PDNS_API_KEY`, `MW_PDNS_PROXY_KEY`, `MW_PDNS_OWNER_SECRET`, `MW_UMAMI_DB_PASSWORD`, `MW_UMAMI_SECRET`; optional ACME email and own-cert paths.
7. `docker compose up -d --build` (with progress for the slow caddy build).
8. Prints — and saves to **`NEXT-STEPS.txt`** — the checklist of things only the operator can do (§10.6).

`renew-certs.sh` is the post-delegation helper: it **refuses to run until public DNS resolution actually works** (protects Let's Encrypt's ~5 failures/hour budget), then restarts Caddy and tails the logs for `certificate obtained successfully`.

## 10.6 Manual steps (VM / cloud / registrar)

The stack cannot do these for you:

**Cloud/VPS (example: Azure, Standard B2ats v2 + Standard SSD, Ubuntu 24.04):**

1. Make the public IP **static** (it becomes `MW_PUBLIC_IP` and goes to the registrar).
2. Disable any auto-shutdown (a DNS server is 24/7).
3. Open the **cloud firewall / NSG** (in addition to the host firewall): 53/udp, 53/tcp, 80/tcp, 443/tcp, and SSH restricted to your IP. ⚠️ Azure pitfall: check *Effective security rules* — a rule with *Source port ranges = 53* instead of `*` silently breaks TCP-source-port-random resolvers.

**Registrar (where the domain was bought):**

1. **Glue/host records**: `ns1.{MW_DOMAIN}` and `ns2.{MW_DOMAIN}` → `MW_PUBLIC_IP`.
2. **Delegation**: set the domain's NS to `ns1.`/`ns2.{MW_DOMAIN}` (one IP for both works; a second VM on another IP for `ns2` is the cheap redundancy upgrade).
3. **DNSSEC**: submit the **DS record** printed on first boot (`docker compose logs pdns` or `pdnsutil export-zone-ds`).

**After delegation propagates** (`dig +short api.{MW_DOMAIN} @8.8.8.8` returns the VM's IP): run `./renew-certs.sh`. Certificate failures on first boot are **expected** — Let's Encrypt can't validate until delegation + cloud port 53 are live (symptoms: `DNS problem: SERVFAIL`, browser `ERR_SSL_PROTOCOL_ERROR`).

**Umami one-time setup**: log into `https://stats.{domain}` (`admin`/`umami`), change the password, create the website entry, paste the generated UUID into `website/index.html`'s `data-website-id`, `docker compose restart caddy`.

**MoonlightWeb side (0.2.0+)**: set `MW_DOMAIN`, `MW_PDNS_URL=https://dnsapi.{domain}/api/v1/servers/localhost`, and `MW_PDNS_TOKEN=`*the restricted `MW_PDNS_PROXY_KEY`* in the server's `.env` (or CI secrets for release builds). Legacy 0.1.x uses `api.{domain}` with the full key.

## 10.7 Least-privilege API key (`mw-proxy`)

PowerDNS API keys are **all-or-nothing** — the key controls every zone (delete zones, rewrite NS/SOA, disable DNSSEC, dump records). Because the key is baked into distributed binaries, one extracted key would mean full DNS compromise. `mw-proxy` (`deploy/powerdns/mw-proxy/`, ~300 lines of Go, stdlib only) keeps the **full** key server-side and hands the app a **restricted** key that can only manage its own records.

Enforcement, per request (everything else → `403`):

- **Zone + method**: only `GET`/`PATCH` on the one configured zone. Zone create/delete, `/config`, `/cryptokeys`, `/metadata`, other zones — refused.
- **Name + type**: only `{uid}.{zone}` A, `_owner.{uid}.{zone}` TXT, `_acme-challenge.{uid}.{zone}` TXT, where `uid` is 8 hex chars (mirrors `generateUniqueId()`, auto-excludes reserved labels). A disguised NS/SOA rewrite is rejected on type.
- **No zone dumps**: `GET` must carry a valid `rrset_name` filter — no listing the whole zone to harvest subdomains.
- **Server-side ownership (TOFU)**: the first writer of a subdomain presents a per-instance token in the `X-MW-Owner` header; mw-proxy stores `HMAC_S(token)` keyed by `uid` (`mwproxy_data:/data/owners.json`) and requires the matching token on every later write. The token is **never** published in DNS, so it cannot be read (via `dig` or a GET) and replayed. **One subdomain per owner** is enforced; deleting the A record releases it.
- **Bulk-creation limit**: new subdomains are rate-limited per client IP (`MW_PROXY_MAX_NEW_PER_HOUR`, default 20), on top of Caddy's 60 req/min.

Secrets (`.env`, generated by `install.sh`): `MW_PDNS_API_KEY` (full, server-side only), `MW_PDNS_PROXY_KEY` (restricted, = the app's `MW_PDNS_TOKEN`), `MW_PDNS_OWNER_SECRET` (HMAC key for the store — keep stable). App side: `PdnsClient`/`AcmeClient` send `X-MW-Owner` = the persisted per-instance `owner_token`; the legacy public `_owner` TXT is no longer written.

**Migration (two phases).** *Phase 1 (0.2.0)* — both endpoints live: 0.1.x → `api.` (full key), 0.2.0+ → `dnsapi.` (restricted key). *Phase 2 (later)* — the `api.` → pdns route is retired (returns a "please upgrade" message that 0.1.x surfaces via `PdnsClient`'s error path) and `MW_PDNS_API_KEY` is rotated, killing keys baked into old binaries.

## 10.8 Hardening & limits

Built-in: DNS rate-limit + ANY→TCP (dnsdist), API key + 60 req/min/IP (Caddy), least-privilege restricted key (mw-proxy, §10.7), non-root/no-caps containers, resource limits, API port never published, `disable-axfr`, anonymous version string, host fail2ban + auto-updates.

Explicit non-goals: **volumetric DDoS** absorption (needs upstream scrubbing/anycast/managed secondary DNS) and in-container IP banning (fail2ban belongs on the Docker host, watching Caddy's access log; the in-container mitigation is the HTTP 429 rate limit).

## 10.9 Operations cheat-sheet

```bash
docker compose logs -f pdns|mw-proxy|caddy|dnsdist   # logs
docker compose exec pdns pdnsutil --config-dir=/etc/powerdns list-zone <domain>
docker compose restart caddy                      # after editing website/ (bind-mounted)
docker compose up -d --build                      # after editing .env
./renew-certs.sh                                  # reissue certs once DNS is live
```

Persistence: zone DB in `pdns_data`, ownership store in `mwproxy_data`, certs in `caddy_data`, analytics in `umami_db` — rebuilding keeps them; remove the volumes to start fresh. `pdns/init.sh` is idempotent, restarts are safe.

---

[← Installers & Packaging](09-Installers-and-Packaging.md) · [Home](Home.md) · [Next: Build, CI & Testing →](11-Build-CI-Testing.md)
