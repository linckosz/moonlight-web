[← Installers & Packaging](09-Installers-and-Packaging.md) · **Infrastructure Stack** · [Next: Build, CI & Testing →](11-Build-CI-Testing.md)

---

# 10. Infrastructure stack (`deploy/powerdns/`)

One small Linux VM with a public IP, running everything this project needs on the public internet — and deliberately nothing that a stream depends on. It carries:

| Role | Answers on | Section |
|---|---|---|
| **Introduction server** — puts a browser in touch with a machine, then gets out of the way | `stream.{domain}/v1/` | [§10.7](#107-the-introduction-server-mw-rendezvous) |
| **Bootstrap entry page** — the few kilobytes a browser loads before the tunnel exists | `stream.{domain}/{id}` | [§10.8](#108-the-bootstrap-entry-page) |
| **STUN** — "what address did this packet come from?" | `stream.{domain}:3478` | [§10.1](#101-topology) |
| **Authoritative DNS** for the project's own names | `:53`, `ns1`/`ns2.{domain}` | [§10.2](#102-powerdns-configuration) |
| **Release relay + version census** | `updates.{domain}` | [§10.10](#1010-update-relay--installed-version-census) |
| **Session census** | `metrics.{domain}` | [§10.11](#1011-session-census) |
| **Marketing site + analytics** | apex, `www.`, `stats.{domain}` | [§10.4](#104-caddy-caddy) |

**No media crosses this box, and neither does any credential.** The introduction server copies WebRTC signalling between two peers until they have a direct path; from that point video, audio and input flow between the browser and the user's own machine, and this VM is not in the path. What it *is* trusted with — and what that trust cannot be designed away from — is stated in [§10.7](#107-the-introduction-server-mw-rendezvous).

A fork can run its own: nothing in the app hard-codes the author's box beyond the `MW_DOMAIN` / `MW_RENDEZVOUS_URL` defaults ([Settings §7.3](07-Settings-Reference.md#73-env--environment-configuration)).

## 10.1 Topology

```
Internet ─:53──────► [dnsdist] ──► pdns:5300               anti-amplification + DNS rate-limit
Internet ─:3478────► [coturn]                              STUN only, host network, no relay
Internet ─:80/:443─► [caddy] ┬─ stream.{domain}/v1/ ─────► mw-rendezvous:8090  held lines + signalling
                             ├─ stream.{domain}/{id} ────► /srv/bootstrap      the entry page
                             ├─ updates.{domain} ───────► mw-proxy:8080 ──► GitHub     release relay + census
                             ├─ metrics.{domain} ───────► mw-proxy:8080 ──► umami:3000 session census
                             ├─ dnsapi.{domain} ────────► mw-proxy:8080 ──► pdns:8081  restricted DNS API
                             ├─ stats.{domain} ─────────► umami:3000          analytics dashboard
                             └─ apex + www.{domain} ────► /srv/site           marketing site
                     [pdns]          (internal, non-root)  PowerDNS authoritative + REST API
                     [mw-proxy]      (internal)            least-privilege gateway + relay/census
                     [mw-rendezvous] (internal)            the introduction server
                     [umami+db]      (internal)            privacy-friendly web analytics
```

Eight containers (one process each — the idiomatic Docker layout), defined in `docker-compose.yml`:

| Container | Image | Exposure | Role |
|---|---|---|---|
| `mw-rendezvous` | custom build: small Go server (stdlib only) | internal only (:8090) | **The introduction server.** One held WebSocket per online instance; copies signalling to the browsers that ask for that instance by identifier. Claim store in `mwrdv_data`. See §10.7. |
| `coturn` | official `coturn/coturn:4.6-alpine` | public :3478, **host network** | STUN and only STUN (`--stun-only`): no relay, so it never carries a byte of anybody's stream and cannot be turned into an amplifier. Host networking is not incidental — Docker's userland proxy rewrites the source address, and a STUN server behind it would confidently answer every client with the bridge gateway's address. |
| `pdns` | official `powerdns/pdns-auth-49`, **unmodified** | internal only (DNS :5300, API :8081, fixed IP 172.28.0.10) | Authoritative zone (SQLite in the `pdns_data` volume) + REST API. Runs non-root, `cap_drop: ALL`. |
| `mw-proxy` | custom build: small Go gateway (stdlib only) | internal only (:8080) | Least-privilege gateway in front of the pdns API (§10.9); the same process serves the update relay on `/v1/update` (§10.10) and the session census on `/v1/session` (§10.11). Store in `mwproxy_data`. |
| `dnsdist` | official `powerdns/dnsdist-19` | public :53 UDP+TCP | DNS front: per-client rate limit, ANY→TCP, forwards to pdns. |
| `caddy` | custom build: Caddy + `caddy-ratelimit` (xcaddy) | public :80/:443 | Vhosts per §10.4. Bind-mounts `bootstrap/` and `website/` from the repo root. Auto Let's Encrypt. |
| `umami` | official Umami (postgres flavor) | internal (via caddy) | Cookieless analytics for the landing page and the two censuses. |
| `umami-db` | postgres 16-alpine | internal | Umami storage (`umami_db` volume). |

All containers: `no-new-privileges`, `pids_limit`, `mem_limit`; only dnsdist, caddy and coturn keep `NET_BIND_SERVICE` — for coturn not because of the port (3478 is unprivileged) but because `turnserver` carries that capability as a *file* capability, and the kernel refuses to exec such a binary when the capability is missing from the bounding set. Dropping `ALL` alone kills the container before it prints a line. The compose network uses a **fixed subnet** (172.28.0.0/24) because dnsdist's `newServer` needs a literal backend IP (it does not resolve container names).

## 10.2 PowerDNS configuration

The official image already ships `pdns.conf`, the gsqlite3 schema and the API wiring (rendered from `PDNS_AUTH_API_KEY` by its own startup wrapper). The stack adds only two mounted files:

- **`pdns/zz-mw.conf`** — hardening snippet merged via include-dir: `disable-axfr`, `version-string=anonymous`, default SOA content, internal ports.
- **`pdns/init.sh`** — idempotent zone bootstrap run as entrypoint (then `exec`s the official wrapper):
  - creates the zone if absent with A records for `@`, `www`, `stats`, `stream`, `ns1`, `ns2`, `api`, `dnsapi`, `updates`, `metrics` → `MW_PUBLIC_IP`, NS records, then `secure-zone` (DNSSEC) + `rectify-zone`, and **prints the DS record** to submit to the registrar;
  - **backfills** missing records on pre-existing zones (`ensure_a` guards — note: a bare re-`add-record` would duplicate, hence the greps);
  - replaces the image's placeholder SOA when found.

The zone holds the stack's **own** names and nothing else: no instance is named in it, and no record is created at runtime on anyone's behalf. An instance is found through the introduction server, not through DNS — which is why enabling Internet Access publishes nothing and issues no certificate ([Overview §1.2](01-Overview.md#internet-access)).

`MW_PDNS_API_KEY` from `.env` maps to `PDNS_AUTH_API_KEY`, so the key reaches the official mechanism unchanged. This **full** key stays server-side: pdns uses it, and `mw-proxy` injects it toward pdns; clients only ever hold the **restricted** `MW_PDNS_PROXY_KEY` (§10.9).

## 10.3 dnsdist configuration (`dnsdist/dnsdist.conf`)

- Listens on :53 UDP+TCP (v4+v6); ACL widened to the whole Internet (authoritative server).
- Backend pinned **up** (`getServer(0):setUp()`) — active health checks probe a name pdns won't serve and would otherwise black-hole the single backend.
- **Anti-amplification**: `MaxQPSIPRule(50, 32, 64)` drops clients over ~50 qps (per /32 v4, /64 v6); `ANY` queries are forced to TCP (`TCAction`), defanging UDP reflection.
- Raised FD limit (16384) — dnsdist pre-allocates sockets for per-IP QPS tracking.

## 10.4 Caddy (`caddy/`)

- **Dockerfile**: xcaddy build adding the `caddy-ratelimit` plugin (this Go build is why the installer adds swap on 1 GiB VMs).
- **`entrypoint.sh`** renders `Caddyfile.tmpl` from env at boot: `@MW_DOMAIN@`, `@TLS_LINE@` (api vhost: empty = auto Let's Encrypt, or `tls /certs/...` when `MW_TLS_CERT`+`MW_TLS_KEY` are both set — user files take priority), `@SITE_TLS_LINE@` (site vhosts always get their own ACME cert, never the api-only files).
- Certificates persist in the `caddy_data` volume.

| Vhost | Serves |
|---|---|
| `stream.` | **The entry host.** `/v1/*` reverse-proxies to `mw-rendezvous` (rate-limited on connection *attempts*: a held line is one request that then stays open for days, so a host is charged once per reconnect). Everything else is the bootstrap (§10.8), with `try_files {path} /index.html` so every `/{id}` serves the same page and a real asset still wins over the fallback. `Referrer-Policy: no-referrer`, because the identifier is in the path and would otherwise ride along in the `Referer` of every outbound link. |
| `updates.` / `metrics.` / `dnsapi.` | `mw-proxy` (60 req/min/IP), respectively the release relay, the session census and the restricted DNS API. |
| `stats.` | Umami. |
| apex + `www.` | The static marketing site (`website/`), `www` 301-redirected to the apex. |

`stream.` is a **real host, deliberately separate from the apex**, and its Caddy block is self-contained on purpose: the apex is the marketing site and stays where it is, while this name can be repointed at another machine — or several — the day signalling outgrows one box. That separation is only worth something if nothing about the entry host depends on being co-located with the site. It used to 301 everything to the apex, back when it was a vanity name with nothing behind it; nothing redirects any more, and the address a user is shown is the address they stay on.

The bootstrap block also sets a `Content-Security-Policy` header kept **in step with the `<meta>` CSP inside `index.html`** — the meta tag is what protects the reference copy on GitHub Pages, which serves no headers of its own. The two are enforced independently and intersect, so a directive relaxed in one place and not the other silently becomes the strictest of the pair. Change both together.

## 10.5 The installer (`install.sh`)

One idempotent script (`sudo ./install.sh`, re-runnable, keeps an existing `.env`) that takes **any fresh Linux distro** (apt/dnf/yum/pacman/zypper/apk) to a running stack:

1. Detects the package manager; installs **Docker + compose plugin**.
2. Installs host security: **fail2ban** + unattended security updates.
3. Configures the **host firewall** (ufw/firewalld/nftables best-effort): 53 UDP+TCP, 3478 UDP+TCP, 80, 443, SSH.
4. **Frees port 53** — Ubuntu's `systemd-resolved` stub listener holds it.
5. Adds a **swap file on low-RAM hosts** (the xcaddy Go build can OOM at 1 GiB).
6. Interactive `.env`: required `MW_DOMAIN` + `MW_PUBLIC_IP` (auto-detected default); generates `MW_PDNS_API_KEY`, `MW_PDNS_PROXY_KEY`, `MW_PDNS_OWNER_SECRET`, `MW_RDV_OWNER_SECRET`, `MW_UMAMI_DB_PASSWORD`, `MW_UMAMI_SECRET`; optional ACME email and own-cert paths.
7. `docker compose up -d --build` (with progress for the slow caddy build).
8. Prints — and saves to **`NEXT-STEPS.txt`** — the checklist of things only the operator can do (§10.6).

`renew-certs.sh` is the post-delegation helper: it **refuses to run until public DNS resolution actually works** (protects Let's Encrypt's ~5 failures/hour budget), then restarts Caddy and tails the logs for `certificate obtained successfully`.

## 10.6 Manual steps (VM / cloud / registrar)

The stack cannot do these for you:

**Cloud/VPS (example: Azure, Standard B2ats v2 + Standard SSD, Ubuntu 24.04):**

1. Make the public IP **static** (it becomes `MW_PUBLIC_IP` and goes to the registrar).
2. Disable any auto-shutdown (a DNS server, and a server holding everyone's lines, is 24/7).
3. Open the **cloud firewall / NSG** (in addition to the host firewall): 53/udp, 53/tcp, 3478/udp, 3478/tcp, 80/tcp, 443/tcp, and SSH restricted to your IP. ⚠️ Azure pitfall: check *Effective security rules* — a rule with *Source port ranges = 53* instead of `*` silently breaks TCP-source-port-random resolvers.

**Registrar (where the domain was bought):**

1. **Glue/host records**: `ns1.{MW_DOMAIN}` and `ns2.{MW_DOMAIN}` → `MW_PUBLIC_IP`.
2. **Delegation**: set the domain's NS to `ns1.`/`ns2.{MW_DOMAIN}` (one IP for both works; a second VM on another IP for `ns2` is the cheap redundancy upgrade).
3. **DNSSEC**: submit the **DS record** printed on first boot (`docker compose logs pdns` or `pdnsutil export-zone-ds`).

**After delegation propagates** (`dig +short stream.{MW_DOMAIN} @8.8.8.8` returns the VM's IP): run `./renew-certs.sh`. Certificate failures on first boot are **expected** — Let's Encrypt can't validate until delegation + cloud port 53 are live (symptoms: `DNS problem: SERVFAIL`, browser `ERR_SSL_PROTOCOL_ERROR`).

**Umami one-time setup**: log into `https://stats.{domain}` (`admin`/`umami`), change the password, create the website entry, paste the generated UUID into `website/index.html`'s `data-website-id`, `docker compose restart caddy`.

**MoonlightWeb side**: set `MW_DOMAIN` in the server's `.env` (or CI secrets for release builds) and the instances reach `https://stream.{MW_DOMAIN}` on their own. `MW_RENDEZVOUS_URL` overrides that derivation outright when the introduction server lives somewhere else.

## 10.7 The introduction server (`mw-rendezvous`)

`deploy/powerdns/mw-rendezvous/`, Go, stdlib only — same rule as `mw-proxy` and for the same reason: a poisoned dependency is a realistic way into this box, and there is nothing here worth importing one for.

**What it does.** A host holds one WebSocket open. A browser arrives naming that host's identifier. The two exchange WebRTC signalling through this process until they have a direct path, and every byte after that flows between them without touching this box.

**Why the line is held rather than announced.** The address a browser actually needs is the reflexive candidate — a port that only exists while a connection is being made. It cannot be stored and it cannot be re-used, so there is nothing an instance could usefully publish in advance. The held socket *is* the reachability: while it is up the machine is reachable, and while it is down it simply is not. Nothing is published and nothing is polled.

**Three endpoints**, all under `/v1/`:

| Endpoint | Who calls it | What happens |
|---|---|---|
| `POST /v1/claim` | An instance, once | Registers its identifier, presenting `X-MW-Owner`. The store keeps `HMAC(token)` keyed by the identifier (trust-on-first-use); every later use of that identifier must present the matching token. Rate-limited per client IP (`MW_RDV_MAX_NEW_PER_HOUR`, default 20). `DELETE` on the same path releases a claim — only for the holder of the token. |
| `GET /v1/host` (upgrade) | An instance, continuously | The held line. Authenticated with the same ownership token. Keep-alives both ways; a line that goes quiet is hung up rather than left to rot. |
| `GET /v1/peer` (upgrade) | A browser | Asks for one identifier, gets a session on that host's line. Rate-limited per client IP (`MW_RDV_MAX_PEER_PER_MIN`, default 30) and capped per host (`MW_RDV_MAX_SESSIONS`, default 4). |

**The identifier is a locator, not a secret.** 26 characters of Crockford base32 (`RendezvousId`, `backend/src/common/RendezvousId.{h,cpp}`), lower-case, no `i`/`l`/`o`/`u` so it survives being read aloud or copied off a screen. 128 bits, so the set cannot be walked; and it never rotates, because an address that changes cannot be bookmarked. Holding one grants nothing — what grants access is the pairing signature and the PIN ([Security §6.2](06-Security.md)).

> ⚠️ `RendezvousId::normalise()` and `normaliseID()` in `mw-rendezvous/store.go` **must agree character for character**. The server keys its ownership store on the normalised form: if the two ever disagree, a claim made under one spelling becomes unfindable under the other and an instance silently loses its own identifier. Unit tests on both sides pin the pairs.

**What it is trusted with, stated plainly.** Inherent to the role and not removable: which identifiers exist, which are online, and the residential addresses visible on the sockets while they are connected. None of it is written down — the only durable state is the identifier-to-owner claim.

**What the design takes away from it.** It relays the DTLS fingerprints the two peers use to find each other, so a compromised instance of this service could substitute its own and sit in the middle. That is exactly what **MW-BIND-v1** exists to stop: the fingerprint is signed with a pairing key this service never sees, and the payloads it copies are opaque to it ([Security §6.6](06-Security.md#66-the-entry-identifier-and-mw-bind-v1), `docs/design/pairing-signature.md`). It also never serves the page — the bootstrap is hosted separately (§10.8) — so breaking in here does not yield code execution in anyone's browser.

## 10.8 The bootstrap entry page

An instance publishes no name and opens no web port, so a browser cannot make an HTTP request to it. But the interface still has to come **from that machine** — serving it from this box would put the author's code between a user and their own desktop, which is the one thing the architecture refuses.

So `stream.{domain}/{id}` serves a few kilobytes of HTML and JavaScript (`bootstrap/`, ~5 files). That page opens a WebRTC connection straight to the machine named in the address, runs the MW-BIND-v1 handshake before creating any DTLS state, installs a Service Worker, and then fetches the actual interface **through the tunnel**. Everything after the first byte — the app, the REST API, the streaming signalling — comes from the user's own machine ([Backend §3.4](03-Backend.md#34-internet-access-and-the-way-in)).

It is small **on purpose** and published **on purpose**: it is the one piece of this project's code a browser ever loads from a server, so the honest mitigation is not "trust us" — it is that these bytes are few enough to read, identical for everyone, and published with their digests.

- `.github/workflows/pages.yml` publishes `bootstrap/` to **GitHub Pages** (the domain in `bootstrap/CNAME`) as the digest-stamped reference copy. Only the bootstrap goes there; the marketing site stays behind Caddy, which can set real headers.
- `.github/workflows/bootstrap-watch.yml` fetches every bootstrap file from the live entry host **and** from the Pages copy on a schedule and requires them to be byte-identical.

Be precise about what that buys: it catches a box serving modified bytes **to everyone**, within the watch interval. It does not catch a box serving the good bytes to whoever is checking and different ones to a chosen victim — nothing an outsider can run catches that. It covers the untargeted case, which is nearly all of them, and it is not a guarantee. A mismatch is also more likely a deploy caught halfway than an attack, which is why the watcher retries before it shouts.

## 10.9 Least-privilege API key (`mw-proxy`)

Instances never hold full PowerDNS credentials. `mw-proxy` (`deploy/powerdns/mw-proxy/`, Go, stdlib only) sits in front of the pdns API: it keeps the **full** key server-side and accepts a **restricted** key from clients. That restricted key is what authenticates an instance to the update relay (§10.10) and the session census (§10.11), and by design it cannot delete zones, rewrite NS/SOA, disable DNSSEC, dump records, or reach any other zone.

Enforcement, per request (everything else → `403`):

- **Zone + method**: only `GET`/`PATCH` on the one configured zone. Zone create/delete, `/config`, `/cryptokeys`, `/metadata`, other zones — refused.
- **No zone dumps**: a `GET` must carry a valid `rrset_name` filter — no listing the whole zone.
- **Per-IP budget** on top of Caddy's 60 req/min.

Secrets (`.env`, generated by `install.sh`): `MW_PDNS_API_KEY` (full, server-side only), `MW_PDNS_PROXY_KEY` (restricted, = the app's `MW_PDNS_TOKEN`), `MW_PDNS_OWNER_SECRET` (HMAC key for the proxy's store), `MW_RDV_OWNER_SECRET` (HMAC key for the rendezvous claim store — a **separate** secret, since it authorises a different thing). Keep all of them stable.

## 10.10 Update relay & installed-version census

Version-migration decisions ("can 0.2.x be dropped?") need to know what is still running. GitHub download counts cannot answer that: cumulative, downloads ≠ installs, silent about who upgraded since.

`updates.{domain}` (`mw-proxy/update.go`, path `/v1/update`, same container as §10.9) answers it by taking over a request the app already makes. MoonlightWeb 0.3.0+ sends its 6-hourly update check here instead of to `api.github.com`, with `?v=&os=&arch=`, authenticating with the restricted key.

- **Serves**: the *verbatim* GitHub release JSON — the client parser is untouched — from a fleet-wide cache (`MW_UPDATE_CACHE_SECONDS`, default 600). One upstream fetch per window for every instance on Earth, versus GitHub's 60 req/h unauthenticated budget.
- **Records**: version/OS/arch as an Umami event, nothing else — no identifier, no account, no per-machine history. Each value is matched against an allowlist and collapsed to `unknown`/`other` otherwise, so a forged client cannot inject strings into the dashboard or blow up its cardinality.
- **Never load-bearing**: on any failure (unreachable, 502, unusable body) the client immediately retries `api.github.com`; the relay itself serves a *stale* cached release rather than an error when GitHub is down. An outage here costs a round trip, never an update.
- **Consent first**: the instance reports nothing until the person running it has been asked, at first launch, and has said yes (`metrics_consent`; [Settings §7](07-Settings-Reference.md)). Refusing — or never answering — sends the check straight to GitHub, exactly as an instance without the relay does. Withdrawn at any time from the app's **Settings → Privacy** section, effective on the spot.
- **Opt-out**: `update_relay_enabled: false` or `MW_NO_TELEMETRY` in the app. Self-built binaries never reach it — the relay requires the `MW_PDNS_TOKEN` only official builds carry.
- **Cannot escalate into a binary**: the client refuses a relayed payload whose `download_url` is not on a GitHub host and re-asks `api.github.com` (`UpdateChecker::isGitHubUrl`). `SelfUpdater` *executes* what that URL names, and the same payload supplies the digest it is checked against — so this pin is what stops "controls the infrastructure VM" from becoming "installs code on every host". A compromised relay is left able only to lie about which release exists.

**Dashboard.** Set `MW_UMAMI_TELEMETRY_WEBSITE_ID` to a *second* Umami website (suggested `MoonlightWeb clients` / `updates.{domain}`) so fleet data stays out of the marketing stats; leave it empty and the relay serves updates while counting nothing. Checks are recorded as **pageviews** at `/uc/{version}?os={os}&arch={arch}`, which makes Umami's built-in **Pages** panel the census with zero configuration: its *Path* tab gives one row per version (the histogram), its *URL* tab keeps the platforms apart. The payload deliberately carries no `name` — Umami files a named payload as a custom event, which lands under **Events** and never feeds Visitors/Views/Pages.

Two properties of that number to keep in mind: Umami's visitor hash uses a **daily-rotating salt**, so "unique visitors / 30 days" approximates machine count rather than counting distinct machines; and since the version is part of the hash input, an upgraded machine moves to its new version's bucket the same day — which is exactly the signal a migration wants.

## 10.11 Session census

The version census says which builds are alive; it says nothing about how they are used. `metrics.{domain}` (`mw-proxy/session.go`, path `/v1/session`, the same container again) is where instances report the **shape** of a session — so "is 720p still worth supporting?", "did AV1 take off?", "how long does a session last?" stop being guesses.

MoonlightWeb 0.3.0+ POSTs a small JSON body, authenticated with the same restricted key, at two moments: when a stream first carries a picture, and when it ends. A launch that never got there is reported too.

- **Records**: resolution, frame rate, negotiated codec, HDR and 4:4:4 flags, a bitrate *band*, backend family, the transport that won, LAN-or-internet, a coarse device class, owner-or-player, and — at the end — a duration *bucket*.
- **Never records**: host name or UUID, account, session token, pairing identity, **the application launched**, or any raw address. Every field is matched against an allowlist or collapsed into a bucket before it goes anywhere, so a forged client can neither inject strings into the dashboard nor blow up its cardinality; there is no free-text field at all.
- **Never load-bearing**: the endpoint answers `204` whether or not it counts anything, the instance drops a failed report without retrying, and nothing about a stream depends on the answer.
- **Consent first**: same single answer as §10.10 — nothing is reported until the machine's owner has been asked and has agreed, and withdrawing in **Settings → Privacy** stops it immediately.
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

## 10.12 Hardening & limits

Built-in: DNS rate-limit + ANY→TCP (dnsdist), STUN without relay (coturn `--stun-only`, which is also what keeps it from being used as an amplifier), API key + 60 req/min/IP (Caddy), least-privilege restricted key (mw-proxy, §10.9), per-IP claim/connection quotas on the introduction server (§10.7), non-root/no-caps containers, resource limits, API port never published, `disable-axfr`, anonymous version string, host fail2ban + auto-updates.

The quota that matters most is `MW_RDV_MAX_PEER_PER_MIN`: it is what bounds identifier guessing, so it is a security control rather than hygiene. It sits behind a 128-bit identifier space, which is the actual defence — the quota only removes the value of trying.

Explicit non-goals: **volumetric DDoS** absorption (needs upstream scrubbing/anycast/managed secondary DNS) and in-container IP banning (fail2ban belongs on the Docker host, watching Caddy's access log; the in-container mitigation is the HTTP 429 rate limit).

## 10.13 Operations cheat-sheet

```bash
docker compose logs -f pdns|mw-proxy|mw-rendezvous|caddy|dnsdist|coturn   # logs
docker compose exec pdns pdnsutil --config-dir=/etc/powerdns list-zone <domain>
docker compose restart caddy                      # after editing website/ or bootstrap/ (bind-mounted)
docker compose up -d --build                      # after editing .env
./renew-certs.sh                                  # reissue certs once DNS is live
```

Persistence: zone DB in `pdns_data`, proxy store in `mwproxy_data`, **identifier claims in `mwrdv_data`**, certs in `caddy_data`, analytics in `umami_db` — rebuilding keeps them; remove the volumes to start fresh. Removing `mwrdv_data` is the one that hurts: every instance loses the claim on its own identifier, and every bookmark and share link pointing at it stops resolving to that machine. `pdns/init.sh` is idempotent, restarts are safe.

---

[← Installers & Packaging](09-Installers-and-Packaging.md) · [Home](Home.md) · [Next: Build, CI & Testing →](11-Build-CI-Testing.md)
