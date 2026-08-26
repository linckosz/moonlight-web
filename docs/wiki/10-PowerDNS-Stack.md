[← Installers & Packaging](09-Installers-and-Packaging.md) · **PowerDNS Stack** · [Next: Build, CI & Testing →](11-Build-CI-Testing.md)

---

# 10. PowerDNS Stack (`deploy/powerdns/`)

The DNS stack is the infrastructure side of the **legacy Internet Access** mechanism: an authoritative DNS server with a REST API on a domain you own, letting a MoonlightWeb instance self-register `{uniqueId}.{MW_DOMAIN}` and pass ACME DNS-01 challenges. It runs on a small Linux VM, fully decoupled from the app.

> ⚠️ **Retirement.** New installs no longer register subdomains; only instances that registered one before the retirement still talk to this stack. The author's shared instance keeps serving those until **February 2027**, then shuts down. The stack remains available for a fork that wants to run the mechanism for its own users — and this VM also hosts the marketing site, the stats and the release relay, which outlive the DNS role.

## 10.1 Topology

```
Internet ─:53──────► [dnsdist] ──► pdns:5300          anti-amplification + DNS rate-limit
Internet ─:80/:443─► [caddy] ┬─ api.{domain} ───────► pdns:8081         direct PowerDNS API (compatibility)
                             ├─ dnsapi.{domain} ────► mw-proxy:8080 ──► pdns:8081  restricted API (0.2.0+)
                             ├─ updates.{domain} ───► mw-proxy:8080 ──► GitHub     release relay + census (0.3.0+)
                             ├─ metrics.{domain} ───► mw-proxy:8080 ──► umami:3000 session census (0.3.0+)
                             └─ stats.{domain} ─────► umami:3000       analytics dashboard
                     [pdns]     (internal, non-root)  PowerDNS authoritative + REST API
                     [mw-proxy] (internal)            least-privilege filtering gateway
                     [umami+db] (internal)            privacy-friendly web analytics
```

Six containers (one process each — the idiomatic Docker layout), defined in `docker-compose.yml`:

| Container | Image | Exposure | Role |
|---|---|---|---|
| `pdns` | official `powerdns/pdns-auth-49`, **unmodified** | internal only (DNS :5300, API :8081, fixed IP 172.28.0.10) | Authoritative zone (SQLite in the `pdns_data` volume) + REST API. Runs non-root, `cap_drop: ALL`. |
| `mw-proxy` | custom build: small Go gateway (stdlib only) | internal only (:8080) | Least-privilege filter in front of the pdns API: holds the full key, exposes a restricted key that can only manage a client's own A/TXT records. Store in `mwproxy_data`. See §10.7. Also serves the update relay on `/v1/update` (§10.8) and the session census on `/v1/session` (§10.9). |
| `dnsdist` | official `powerdns/dnsdist-19` | public :53 UDP+TCP | DNS front: per-client rate limit, ANY→TCP, forwards to pdns. |
| `caddy` | custom build: Caddy + `caddy-ratelimit` (xcaddy) | public :80/:443 | `api.{domain}` → pdns API (compatibility); `dnsapi.{domain}` → mw-proxy (restricted); `updates.{domain}` → mw-proxy (release relay); `metrics.{domain}` → mw-proxy (session census); apex/`www`/`stream` → static `website/`; `stats.` → Umami. Auto Let's Encrypt. |
| `umami` | official Umami (postgres flavor) | internal (via caddy) | Cookieless analytics for the landing page. |
| `umami-db` | postgres 16-alpine | internal | Umami storage (`umami_db` volume). |

All containers: `no-new-privileges`, `pids_limit`, `mem_limit`; only dnsdist/caddy keep `NET_BIND_SERVICE`. The compose network uses a **fixed subnet** (172.28.0.0/24) because dnsdist's `newServer` needs a literal backend IP (it does not resolve container names).

## 10.2 PowerDNS configuration

The official image already ships `pdns.conf`, the gsqlite3 schema and the API wiring (rendered from `PDNS_AUTH_API_KEY` by its own startup wrapper). The stack adds only two mounted files:

- **`pdns/zz-mw.conf`** — hardening snippet merged via include-dir: `disable-axfr`, `version-string=anonymous`, default SOA content, internal ports.
- **`pdns/init.sh`** — idempotent zone bootstrap run as entrypoint (then `exec`s the official wrapper):
  - creates the zone if absent with A records for `@`, `www`, `stats`, `stream`, `ns1`, `ns2`, `api`, `dnsapi`, `updates`, `metrics` → `MW_PUBLIC_IP`, NS records, then `secure-zone` (DNSSEC) + `rectify-zone`, and **prints the DS record** to submit to the registrar;
  - **backfills** missing records on pre-existing zones (`ensure_a` guards — note: a bare re-`add-record` would duplicate, hence the greps);
  - replaces the image's placeholder SOA when found.

`MW_PDNS_API_KEY` from `.env` maps to `PDNS_AUTH_API_KEY`, so the key reaches the official mechanism unchanged. This **full** key now stays server-side: pdns uses it, and `mw-proxy` injects it toward pdns. MoonlightWeb 0.2.0+ instead holds the **restricted** `MW_PDNS_PROXY_KEY` (§10.7). Per-instance subdomains (`{uid}` A records, `_acme-challenge.*` TXT) are managed at runtime by MoonlightWeb through the API — the stack never touches them.

## 10.3 dnsdist configuration (`dnsdist/dnsdist.conf`)

- Listens on :53 UDP+TCP (v4+v6); ACL widened to the whole Internet (authoritative server).
- Backend pinned **up** (`getServer(0):setUp()`) — active health checks probe a name pdns won't serve and would otherwise black-hole the single backend.
- **Anti-amplification**: `MaxQPSIPRule(50, 32, 64)` drops clients over ~50 qps (per /32 v4, /64 v6); `ANY` queries are forced to TCP (`TCAction`), defanging UDP reflection.
- Raised FD limit (16384) — dnsdist pre-allocates sockets for per-IP QPS tracking.

## 10.4 Caddy (`caddy/`)

- **Dockerfile**: xcaddy build adding the `caddy-ratelimit` plugin (this Go build is why the installer adds swap on 1 GiB VMs).
- **`entrypoint.sh`** renders `Caddyfile.tmpl` from env at boot: `@MW_DOMAIN@`, `@TLS_LINE@` (api vhost: empty = auto Let's Encrypt, or `tls /certs/...` when `MW_TLS_CERT`+`MW_TLS_KEY` are both set — user files take priority), `@SITE_TLS_LINE@` (site vhosts always get their own ACME cert, never the api-only files).
- Vhosts: `api.` → `reverse_proxy pdns:8081` (compatibility, 60 req/min/IP); `dnsapi.` → `reverse_proxy mw-proxy:8080` (restricted, 60 req/min/IP — §10.7); `updates.` → the same mw-proxy, serving the release relay (§10.8); `metrics.` → the same mw-proxy again, serving the session census (§10.9, 60 req/min/IP); `stats.` → Umami; apex+`www`+`stream` → static site with `www`/`stream` 301-redirected to the apex (`stream.` is a marketing vanity alias).
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

**MoonlightWeb side (0.2.0+)**: set `MW_DOMAIN`, `MW_PDNS_URL=https://dnsapi.{domain}/api/v1/servers/localhost`, and `MW_PDNS_TOKEN=`*the restricted `MW_PDNS_PROXY_KEY`* in the server's `.env` (or CI secrets for release builds).

## 10.7 Least-privilege API key (`mw-proxy`)

MoonlightWeb instances are granted DNS access on a least-privilege basis: they never hold full PowerDNS credentials. `mw-proxy` (`deploy/powerdns/mw-proxy/`, ~300 lines of Go, stdlib only) sits in front of the pdns API — it keeps the **full** key server-side and hands each instance a **restricted** key scoped to its own records. By design a restricted key cannot delete zones, rewrite NS/SOA, disable DNSSEC, dump records, or reach any other zone.

Enforcement, per request (everything else → `403`):

- **Zone + method**: only `GET`/`PATCH` on the one configured zone. Zone create/delete, `/config`, `/cryptokeys`, `/metadata`, other zones — refused.
- **Name + type**: only `{uid}.{zone}` A, `_owner.{uid}.{zone}` TXT, `_acme-challenge.{uid}.{zone}` TXT, where `uid` is 8 hex chars (mirrors `generateUniqueId()`, auto-excludes reserved labels). A disguised NS/SOA rewrite is rejected on type.
- **No zone dumps**: `GET` must carry a valid `rrset_name` filter — no listing the whole zone to harvest subdomains.
- **Server-side ownership (TOFU)**: the first writer of a subdomain presents a per-instance token in the `X-MW-Owner` header; mw-proxy stores `HMAC_S(token)` keyed by `uid` (`mwproxy_data:/data/owners.json`) and requires the matching token on every later write. The token is **never** published in DNS, so it cannot be read (via `dig` or a GET) and replayed. **One subdomain per owner** is enforced; deleting the A record releases it.
- **Bulk-creation limit**: new subdomains are rate-limited per client IP (`MW_PROXY_MAX_NEW_PER_HOUR`, default 20), on top of Caddy's 60 req/min.

Secrets (`.env`, generated by `install.sh`): `MW_PDNS_API_KEY` (full, server-side only), `MW_PDNS_PROXY_KEY` (restricted, = the app's `MW_PDNS_TOKEN`), `MW_PDNS_OWNER_SECRET` (HMAC key for the store — keep stable). App side: `PdnsClient`/`AcmeClient` send `X-MW-Owner` = the persisted per-instance `owner_token`, which is kept out of DNS.

**Endpoints.** MoonlightWeb registers through the restricted `dnsapi.` gateway. A direct `api.` → pdns route is also present for backward compatibility and is retired in a later release.

## 10.8 Update relay & installed-version census

Version-migration decisions ("can 0.2.x be dropped?") need to know what is still running. GitHub download counts cannot answer that: cumulative, downloads ≠ installs, silent about who upgraded since.

`updates.{domain}` (`mw-proxy/update.go`, path `/v1/update`, same container as §10.7) answers it by taking over a request the app already makes. MoonlightWeb 0.3.0+ sends its 6-hourly update check here instead of to `api.github.com`, with `?v=&os=&arch=`, authenticating with the same restricted key.

- **Serves**: the *verbatim* GitHub release JSON — the client parser is untouched — from a fleet-wide cache (`MW_UPDATE_CACHE_SECONDS`, default 600). One upstream fetch per window for every instance on Earth, versus GitHub's 60 req/h unauthenticated budget.
- **Records**: version/OS/arch as an Umami event, nothing else — no identifier, no account, no per-machine history. Each value is matched against an allowlist and collapsed to `unknown`/`other` otherwise, so a forged client cannot inject strings into the dashboard or blow up its cardinality.
- **Never load-bearing**: on any failure (unreachable, 502, unusable body) the client immediately retries `api.github.com`; the relay itself serves a *stale* cached release rather than an error when GitHub is down. An outage here costs a round trip, never an update.
- **Consent first**: the instance reports nothing until the person running it has been asked, at first launch, and has said yes (`metrics_consent`; [Settings §7](07-Settings-Reference.md)). Refusing — or never answering — sends the check straight to GitHub, exactly as an instance without the relay does. Withdrawn at any time from the app's **Settings → Privacy** section, effective on the spot.
- **Opt-out**: `update_relay_enabled: false` or `MW_NO_TELEMETRY` in the app. Self-built binaries never reach it — the relay requires the `MW_PDNS_TOKEN` only official builds carry.
- **Cannot escalate into a binary**: the client refuses a relayed payload whose `download_url` is not on a GitHub host and re-asks `api.github.com` (`UpdateChecker::isGitHubUrl`). `SelfUpdater` *executes* what that URL names, and the same payload supplies the digest it is checked against — so this pin is what stops "controls the DNS VM" from becoming "installs code on every host". A compromised relay is left able only to lie about which release exists.

**Dashboard.** Set `MW_UMAMI_TELEMETRY_WEBSITE_ID` to a *second* Umami website (suggested `MoonlightWeb clients` / `updates.{domain}`) so fleet data stays out of the marketing stats; leave it empty and the relay serves updates while counting nothing. Checks are recorded as **pageviews** at `/uc/{version}?os={os}&arch={arch}`, which makes Umami's built-in **Pages** panel the census with zero configuration: its *Path* tab gives one row per version (the histogram), its *URL* tab keeps the platforms apart. The payload deliberately carries no `name` — Umami files a named payload as a custom event, which lands under **Events** and never feeds Visitors/Views/Pages.

Two properties of that number to keep in mind: Umami's visitor hash uses a **daily-rotating salt**, so "unique visitors / 30 days" approximates machine count rather than counting distinct machines; and since the version is part of the hash input, an upgraded machine moves to its new version's bucket the same day — which is exactly the signal a migration wants.

## 10.9 Session census

The version census says which builds are alive; it says nothing about how they are used. `metrics.{domain}` (`mw-proxy/session.go`, path `/v1/session`, the same container again) is where instances report the **shape** of a session — so "is 720p still worth supporting?", "did AV1 take off?", "how long does a session last?" stop being guesses.

MoonlightWeb 0.3.0+ POSTs a small JSON body, authenticated with the same restricted key, at two moments: when a stream first carries a picture, and when it ends. A launch that never got there is reported too.

- **Records**: resolution, frame rate, negotiated codec, HDR and 4:4:4 flags, a bitrate *band*, backend family, the transport that won, LAN-or-internet, a coarse device class, owner-or-player, and — at the end — a duration *bucket*.
- **Never records**: host name or UUID, account, session token, pairing identity, **the application launched**, or any raw address. Every field is matched against an allowlist or collapsed into a bucket before it goes anywhere, so a forged client can neither inject strings into the dashboard nor blow up its cardinality; there is no free-text field at all.
- **Never load-bearing**: the endpoint answers `204` whether or not it counts anything, the instance drops a failed report without retrying, and nothing about a stream depends on the answer.
- **Consent first**: same single answer as §10.8 — nothing is reported until the machine's owner has been asked and has agreed, and withdrawing in **Settings → Privacy** stops it immediately.
- **Opt-out**: `session_metrics_enabled: false` or `MW_NO_TELEMETRY` in the app ([Settings §7](07-Settings-Reference.md)). Self-built binaries never reach it — it needs the `MW_PDNS_TOKEN` only official builds carry. Fleet-wide, deleting the `metrics` A record switches the whole census off without touching the update path, which is why it is its own host rather than a path under `updates.`.

**Dashboard.** Set `MW_UMAMI_SESSIONS_WEBSITE_ID` to a *third* Umami website (suggested `MoonlightWeb sessions` / `metrics.{domain}`). Its own website on purpose: there, "views" means "sessions started" and nothing else is mixed in. With that, the built-in reports need no configuration:

| Umami report | Reads as |
|---|---|
| Overview → **Views** | sessions started per day |
| Overview → **Visitors** | distinct instances that streamed (daily hash) |
| Pages → **Path** (`/s/1080p60`) | the resolution / frame-rate histogram |
| Pages → **URL** | the same, split by codec / bitrate band / backend / transport / network / device / owner-or-player |
| Events → **dur-…** | how long sessions lasted |
| Events → **launch-failed** | attempts that never reached a picture |

Three things that number is not. A *standby* leg (the second stream the quality ladder opens to switch seamlessly) is deliberately not counted, since it is the same viewer continuing an already-counted session — which also means a duration is the length of the *first* leg, not of the whole evening when the ladder moved. A transport fallback that fails before the picture arrives shows up under `launch-failed`, so that count is attempts, not people. And a transport that reached the host but then failed to establish the browser connection counts as its own very short session: a spike in `dur-0-1m` reads as "the first transport does not work there", not as "people leave immediately".

## 10.10 Hardening & limits

Built-in: DNS rate-limit + ANY→TCP (dnsdist), API key + 60 req/min/IP (Caddy), least-privilege restricted key (mw-proxy, §10.7), non-root/no-caps containers, resource limits, API port never published, `disable-axfr`, anonymous version string, host fail2ban + auto-updates.

Explicit non-goals: **volumetric DDoS** absorption (needs upstream scrubbing/anycast/managed secondary DNS) and in-container IP banning (fail2ban belongs on the Docker host, watching Caddy's access log; the in-container mitigation is the HTTP 429 rate limit).

## 10.11 Operations cheat-sheet

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
