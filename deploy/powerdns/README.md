# Self-hosted DNS stack for MoonlightWeb (Docker)

> ⚠️ **Retirement.** The per-instance-subdomain mechanism this stack served is
> retired for new MoonlightWeb installs; only instances that registered a
> subdomain before the retirement still write here, and the author's shared
> instance shuts that role down in **February 2027**. The box keeps its other
> roles (marketing site, stats, update relay) beyond that date, and the stack
> remains usable by a fork that wants to run the mechanism for its own users.

Three dedicated containers (one process each — the idiomatic Docker layout):

```
Internet ─:53──────> [dnsdist] ──> pdns:5300         anti-amplification + DNS rate-limit
Internet ─:80/:443─> [caddy] ┬─ api.{domain} ──────> pdns:8081        direct PowerDNS API (compatibility)
                             ├─ dnsapi.{domain} ───> mw-proxy:8080 ─> pdns:8081   restricted API (0.2.0+)
                             ├─ updates.{domain} ──> mw-proxy:8080 ─> GitHub      release relay + version census (0.3.0+)
                             └─ stats.{domain} ────> umami:3000      analytics dashboard
                     [pdns]     (internal, non-root)  PowerDNS authoritative + REST API
                     [mw-proxy] (internal)            least-privilege filtering gateway
                     [umami+db] (internal)            privacy-friendly web analytics
```

- **dnsdist** — official `powerdns/dnsdist-19` image. The public DNS entry point;
  per-client rate limiting and ANY→TCP to neutralise reflection/amplification.
- **pdns** — official `powerdns/pdns-auth-49` image, **unmodified**. DNS on the
  internal port `5300`, REST API on `8081`. Never published to the host.
- **mw-proxy** — a small dependency-free Go gateway (built from `mw-proxy/`). It
  holds the real PowerDNS key and lets a MoonlightWeb 0.2.0+ instance manage
  **only its own** A record (+ `_owner` / `_acme-challenge` TXT), authenticating
  with a **restricted** key. Never published; Caddy fronts it as
  `https://dnsapi.{MW_DOMAIN}`. See [Least-privilege API key](#least-privilege-api-key-mw-proxy).
  The same container also serves the **update relay** at
  `https://updates.{MW_DOMAIN}` — see
  [Update relay](#update-relay--installed-version-census).
- **caddy** — official Caddy + the `caddy-ratelimit` plugin (built via xcaddy).
  Exposes the APIs (`api.{MW_DOMAIN}` compatibility, `dnsapi.{MW_DOMAIN}` restricted)
  with automatic TLS and request rate limiting, **and serves the static
  presentation website** at `https://{MW_DOMAIN}` / `https://www.{MW_DOMAIN}`
  (repo-root `website/`, bind-mounted into the container).

The MoonlightWeb server itself is **not** in this stack. A legacy instance talks
to this DNS box over the REST API (`MW_PDNS_URL` + `MW_PDNS_TOKEN`) to keep its
pre-retirement subdomain `{uniqueId}.{MW_DOMAIN}` alive; a fresh install never
calls it. This stack only bootstraps the parent zone, the nameserver glue, the
`api.{MW_DOMAIN}` host and the `www` record for the presentation site.

## Presentation website

Caddy also serves a static marketing/landing page for the project at the apex
domain and `www`:

- `https://{MW_DOMAIN}` — the canonical landing page (`website/index.html` at the
  repo root).
- `https://www.{MW_DOMAIN}` — permanently redirected to the apex.
- `https://stream.{MW_DOMAIN}` — **where people stream** (0.3.0+): serves the
  bootstrap entry page at `/{id}` and the rendezvous API under `/v1/`. It was a
  vanity alias redirected to the apex until 0.3.0; nothing redirects now.
  Deliberately its own host rather than a path on the apex, so signalling can be
  repointed at another machine later without moving the marketing site.

Both get their own automatic Let's Encrypt certificate (independent of any
api-only cert you may supply via `MW_TLS_CERT`). The site is plain HTML/CSS with
the project screenshots under `website/assets/`. It is **bind-mounted** into the
Caddy container (`../../website → /srv/site`), so edit it freely then
`docker compose restart caddy` — no rebuild needed. The bootstrap is mounted the
same way (`../../bootstrap → /srv/bootstrap`). The zone bootstrap adds the `www`
and `stream` A records automatically; the apex `@` A record already existed.

**Cache-busting.** The shared assets (`assets/chrome.js`, `chrome.css`,
`pages.css`, `i18n.js`) are referenced with a `?v=…` query stamped from each
file's modified time:

```
<script src="/assets/chrome.js?v={{(fileStat `/assets/chrome.js`).ModTime.Unix}}"></script>
```

Caddy's `templates` directive (scoped to `text/html`) evaluates this on every
HTML request, so when a shared asset changes on disk its version changes and
browsers fetch the fresh copy — no manual bump. Because of this, `{{ … }}` is
now **significant inside the HTML pages**: if you add inline JS that needs literal
double braces, wrap it so Caddy doesn't try to evaluate it (see the Caddy
`templates` docs). Plain-text files (`robots.txt`, `llms.txt`, the verification
token) are not templated.

## Website analytics (Umami)

The stack ships a self-hosted **Umami** instance — privacy-friendly, cookieless
(no consent banner needed), lightweight — that measures the presentation site:
visits, visitor **countries/regions**, referrers, devices, and **clicks** on the
key links (`Download`, `GitHub`, `Buy me a coffee`).

- Dashboard + tracker are served at `https://stats.{MW_DOMAIN}` (Caddy reverse-
  proxies the internal `umami:3000`; a `stats` A record is added to the zone).
- Umami stores data in its own internal Postgres (`umami-db`, never published).
- The site (`website/index.html`) already carries the tracker `<script>` and the
  `data-umami-event` attributes on the tracked links — nothing to add there.

**One-time setup after the first `docker compose up`:**

1. Open `https://stats.{MW_DOMAIN}` and log in with **`admin` / `umami`**.
2. **Change the admin password** immediately (Settings → Profile).
3. Settings → Websites → **Add website**: name `MoonlightWeb`, domain
   `{MW_DOMAIN}`. Umami generates a **Website ID** (a UUID).
4. Paste that UUID into the `data-website-id` attribute in
   `website/index.html` (it ships as all-zeros), then `docker compose restart caddy`.

Until step 4 the tracker loads but records nothing (the placeholder ID is
ignored) — completely harmless. The two Umami secrets (`MW_UMAMI_DB_PASSWORD`,
`MW_UMAMI_SECRET`) are generated automatically by `install.sh`; set them yourself
in `.env` for a manual install.

## Update relay & installed-version census

Planning a migration ("can 0.2.x be dropped?") needs to know what is actually
running out there. GitHub download counts cannot answer that — they are
cumulative, count downloads rather than installs, and say nothing about who
upgraded since.

So the stack serves an **update relay** at `https://updates.{MW_DOMAIN}`
(`mw-proxy/update.go`, same container as the DNS gateway, path `/v1/update`).
MoonlightWeb **0.3.0+** points its periodic update check there instead of at
`api.github.com`:

- the relay returns the **same GitHub release JSON**, from a cache shared by the
  whole fleet (default 10 min, `MW_UPDATE_CACHE_SECONDS`) — so thousands of
  instances cost a handful of upstream requests per hour instead of hammering
  GitHub's 60/h unauthenticated budget;
- it records **version, OS, architecture** as an Umami event — and nothing else.
  No identifier, no account, no per-machine history. Values are matched against
  allowlists first, so a forged client cannot inject arbitrary strings into the
  dashboard;
- clients present the restricted `MW_PDNS_PROXY_KEY`, the same key as the DNS
  path.

**The relay is never load-bearing.** Any failure — unreachable, 502, unusable
body — makes the client retry against `api.github.com` immediately, and the
relay itself serves a stale cached release rather than an error whenever GitHub
is down. Users can opt out (`update_relay_enabled: false` or `MW_NO_TELEMETRY`),
and self-built binaries never use it: they carry no `MW_PDNS_TOKEN`.

**The relay cannot ship you a binary.** The client only accepts a relayed
payload whose `download_url` is on GitHub's own hosts, and falls back to
`api.github.com` otherwise (`UpdateChecker::isGitHubUrl`). This matters because
the self-updater *runs* what that URL names, and the payload also carries the
digest it is verified against — so without the host pin, whoever controls this
VM would control what every instance installs. With it, a compromised relay can
at most lie about which GitHub release exists.

**Turning the census on** (the relay works without it, counting nothing):

1. In Umami, add a **second website**: name `MoonlightWeb clients`, domain
   `updates.{MW_DOMAIN}`. Keeping it separate stops fleet data from polluting
   the marketing site's stats.
2. Put its ID in `.env` → `MW_UMAMI_TELEMETRY_WEBSITE_ID=<id>`, then
   `docker compose up -d --force-recreate mw-proxy`. On a `.env` created before
   this feature the key is simply absent, so **append** it rather than editing
   in place (`install.sh` backfills it too). `mw-proxy` says which way it went
   at startup: `version census → http://umami:3000 (website 76ce66ce…)`, or
   `version census DISABLED (MW_UMAMI_WEBSITE_ID is empty)` — check with
   `docker compose logs mw-proxy | tail -2`.

**Reading it.** Each check is recorded as a **pageview** at
`/uc/{version}?os={os}&arch={arch}`, so the dashboard's ordinary **Pages** panel
*is* the census, with its date filter and no configuration:

- tab **Path** → one row per version (`/uc/0.2.4`, `/uc/0.3.0`, …) — the version
  histogram;
- tab **URL** → the same split by platform, when you need the OS/arch detail.

Set the range to the last 30 days and read unique visitors per version: that is
your fleet. (It has to be a pageview: a payload carrying a `name` is filed by
Umami as a *custom event*, which shows up under **Events** and leaves
Visitors/Views/Pages at zero.) Two caveats worth knowing:

- Umami identifies a visitor by a hash that includes a **daily-rotating salt**,
  so "unique visitors over 30 days" is an approximation of machine count, not a
  census of distinct machines. Good enough to decide whether a version still has
  users; not a basis for cohort or retention analysis.
- A machine that upgrades starts counting under its new version the same day
  (the version is part of the hash input), which is exactly what you want when
  watching a migration progress.

## Contents

```
deploy/powerdns/
├── install.sh               # one-shot installer (Docker, security, firewall, up)
├── renew-certs.sh           # re-issue TLS certs once DNS delegation has propagated
├── docker-compose.yml       # dnsdist + pdns + mw-proxy + caddy + umami (+ umami-db)
├── mw-proxy/
│   ├── main.go              # least-privilege filtering gateway (stdlib only)
│   ├── update.go            # release relay + installed-version census (0.3.0+)
│   ├── main_test.go         # filter + ownership unit tests
│   ├── update_test.go       # relay cache, fallback and event-sanitising tests
│   └── Dockerfile           # static Go build on a minimal runtime
├── pdns/
│   ├── init.sh              # zone bootstrap, then the official pdns wrapper
│   └── zz-mw.conf           # hardening snippet merged via include-dir
├── dnsdist/dnsdist.conf     # DNS rate limiting / anti-amplification
├── caddy/
│   ├── Dockerfile           # Caddy + caddy-ratelimit (xcaddy)
│   ├── entrypoint.sh        # renders the Caddyfile from env, runs Caddy
│   └── Caddyfile.tmpl       # api → pdns:8081  +  apex/www → static site
├── certs/                   # drop your own cert/key here (gitignored)
└── .env.sample              # copy to .env and fill in
```

The PowerDNS image already ships `pdns.conf`, the gsqlite3 schema (DB pre-created
at build) and the REST API config — generated from `PDNS_AUTH_API_KEY` by its own
startup wrapper. We only mount a hardening snippet (`pdns/zz-mw.conf`) and a zone
bootstrap script (`pdns/init.sh`). `MW_PDNS_API_KEY` from `.env` is mapped to
`PDNS_AUTH_API_KEY` in the compose file, so the API key reaches the official
mechanism unchanged.

## Quick start — automated installer (recommended)

On a fresh Linux VM (any distro: Debian/Ubuntu, RHEL/Fedora, Arch, openSUSE,
Alpine), clone the repo and run the installer. It does **everything**: installs
Docker + compose, host security tools (fail2ban, auto-updates), the host
firewall, frees port 53, adds swap on low-RAM hosts, asks for your settings,
then builds and starts the stack.

```bash
git clone <this-repo>
cd <repo>/deploy/powerdns
sudo ./install.sh
```

The installer prompts for:

- **Required** — `MW_DOMAIN` (the domain you own) and `MW_PUBLIC_IP` (this VM's
  public IPv4, auto-detected as a default). An API key is generated for you.
- **Optional** — a Let's Encrypt notification email, and your own TLS cert/key
  files. Leave the cert fields blank to let Caddy issue and renew a Let's
  Encrypt certificate automatically.

When it finishes, the VM is fully operational. The console (and a saved
`NEXT-STEPS.txt`) shows a **to-do list** for the parts only you can do: opening
the cloud firewall/NSG ports, registering `ns1`/`ns2` at your registrar, and
submitting the DNSSEC DS record. Re-run `sudo ./install.sh` any time — it is
idempotent and keeps an existing `.env`.

On the MoonlightWeb server (0.2.0+), set in its own `.env` the **restricted**
key (least privilege — see [below](#least-privilege-api-key-mw-proxy)):

```
MW_DOMAIN=example.top
MW_PDNS_URL=https://dnsapi.example.top/api/v1/servers/localhost
MW_PDNS_TOKEN=<the MW_PDNS_PROXY_KEY value from deploy/powerdns/.env>
```

### Manual install (without the script)

```bash
cd deploy/powerdns
cp .env.sample .env
# edit .env: MW_DOMAIN, MW_PUBLIC_IP, and the secrets (MW_PDNS_API_KEY,
# MW_PDNS_PROXY_KEY, MW_PDNS_OWNER_SECRET — each: openssl rand -hex 24)
docker compose up -d --build
docker compose logs -f pdns   # note the DS record printed on first boot
```

You then handle Docker, the firewall and port 53 yourself (see the Azure
section below for the individual commands).

## Deploy on Azure (Ubuntu 24.04)

Reference host, cheap and proven: **Standard B2ats v2** (2 vCPU, 1 GiB RAM) with
a **Standard SSD** OS disk running **Ubuntu Server 24.04 LTS**. Steps below are
end-to-end; they finish at the generic `docker compose up` from Quick start.

### 1. Create the VM

- Image: **Ubuntu Server 24.04 LTS**, Size: **Standard B2ats v2**.
- OS disk: **Standard SSD (LRS)** — Premium is unnecessary here.
- Authentication: **SSH public key**.

### 2. Make the public IP static

The IP becomes `MW_PUBLIC_IP` and is registered at your domain provider, so it
must not change. In the portal: **VM → Networking → the Public IP → Configuration
→ Assignment = Static → Save**. Or via CLI:

```bash
az network public-ip update -g <resource-group> -n <public-ip-name> --allocation-method Static
```

### 3. Disable the daily auto-shutdown

A DNS server must stay up 24/7: **VM → Operations → Auto-shutdown → Off → Save**.

### 4. Open the firewall (Network Security Group)

Add inbound rules for the public ports (see the table below) plus SSH. Restrict
SSH to your own IP — never `Any`.

```bash
RG=<resource-group>; NSG=<nsg-name>
az network nsg rule create -g $RG --nsg-name $NSG -n Allow-DNS-UDP --priority 100 \
  --access Allow --protocol Udp --direction Inbound --destination-port-ranges 53
az network nsg rule create -g $RG --nsg-name $NSG -n Allow-DNS-TCP --priority 110 \
  --access Allow --protocol Tcp --direction Inbound --destination-port-ranges 53
az network nsg rule create -g $RG --nsg-name $NSG -n Allow-Web --priority 120 \
  --access Allow --protocol Tcp --direction Inbound --destination-port-ranges 80 443
# SSH restricted to your IP
az network nsg rule create -g $RG --nsg-name $NSG -n Allow-SSH --priority 130 \
  --access Allow --protocol Tcp --direction Inbound --destination-port-ranges 22 \
  --source-address-prefixes <your.public.ip>/32
```

### 5. Run the installer

SSH into the VM, then let the installer handle the rest — it frees port 53
(Ubuntu's `systemd-resolved` holds it), installs Docker, fail2ban and the host
firewall, adds swap (the xcaddy Go build can OOM on 1 GiB RAM), and starts the
stack:

```bash
git clone <this-repo> && cd <repo>/deploy/powerdns
sudo ./install.sh
```

Use the **static public IP** from step 2 as `MW_PUBLIC_IP` when prompted (it is
also auto-detected as the default). When the installer finishes it prints — and
saves to `NEXT-STEPS.txt` — the remaining manual steps, including the DNSSEC DS
record to give your registrar (see *Register the nameserver* below).

## TLS — automatic Let's Encrypt or your own files

Caddy manages the certificate for `api.{MW_DOMAIN}`. **User-supplied files take
priority** over automatic issuance.

### Default — automatic Let's Encrypt

Leave `MW_TLS_CERT` and `MW_TLS_KEY` empty. Caddy obtains and auto-renews a
Let's Encrypt certificate via the HTTP-01 challenge (port 80 must be public).
`MW_TLS_EMAIL` is optional (CA expiry notices):

```
MW_TLS_EMAIL=admin@example.top
MW_TLS_CERT=
MW_TLS_KEY=
```

> **Certificates fail on first boot? That's expected — retry after DNS is live.**
> Let's Encrypt validates by resolving your domain over the public Internet, so
> its very first attempt (during `install.sh`) **fails** until two things are
> true: the registrar delegation (`ns1`/`ns2` glue → `MW_PUBLIC_IP`) has
> propagated **and** port `53` (UDP **and** TCP) is open on your **cloud
> firewall / NSG** — not just the host `ufw`/`firewalld`. The Caddy log then
> shows `DNS problem: SERVFAIL … the domain's nameservers may be malfunctioning`
> or a query timeout, and browsers get `ERR_SSL_PROTOCOL_ERROR` / TLS alert 80.
>
> Once `dig +short api.{MW_DOMAIN} @8.8.8.8` returns your VM's IP, reissue:
>
> ```bash
> cd deploy/powerdns
> ./renew-certs.sh          # verifies public DNS, then restarts Caddy & tails logs
> ```
>
> The helper refuses to run (and won't burn Let's Encrypt's ~5 failures/hour
> limit) until public resolution actually works. You can also just
> `docker compose restart caddy` and watch `docker compose logs -f caddy` for
> `certificate obtained successfully`.

### Bring your own certificate

Drop the cert and key into `deploy/powerdns/certs/` (bind-mounted read-only to
`/certs` in the Caddy container) and point the env vars at the **in-container**
paths. When **both** are set, Caddy serves them and skips Let's Encrypt entirely:

```bash
cp /path/to/fullchain.pem deploy/powerdns/certs/fullchain.pem
cp /path/to/privkey.pem   deploy/powerdns/certs/privkey.pem
```

```
MW_TLS_CERT=/certs/fullchain.pem
MW_TLS_KEY=/certs/privkey.pem
```

```bash
docker compose up -d   # entrypoint renders: tls /certs/fullchain.pem /certs/privkey.pem
```

> If only one of the two is set, it is ignored and Caddy falls back to automatic
> Let's Encrypt. The `certs/` directory is gitignored — never commit a key.

## Ports / protocols to open publicly

Open these on the host firewall **and** any cloud security group, forwarded to
this machine. DNS needs both UDP and TCP.

| Port | Protocol | Container | Why it must be public |
|------|----------|-----------|-----------------------|
| 53   | UDP      | dnsdist   | Resolvers query your zone — primary path |
| 53   | TCP      | dnsdist   | Fallback for truncated responses, DNSSEC |
| 80   | TCP      | caddy     | Let's Encrypt HTTP-01 challenge + HTTP→HTTPS redirect |
| 443  | TCP      | caddy     | `api.{MW_DOMAIN}` REST API + presentation site (`{MW_DOMAIN}`/`www`) + `stats.{MW_DOMAIN}` analytics |

PowerDNS' own ports (`5300` DNS, `8081` API) stay on the internal compose
network — never published.

## Register the nameserver at your domain provider

To make your zone authoritative on the public Internet, configure this at your
**domain registrar** (where you bought `MW_DOMAIN`):

1. **Glue records / host records** — point your nameservers at this box's public
   IP (`MW_PUBLIC_IP`):
   - `ns1.{MW_DOMAIN}` → `MW_PUBLIC_IP`
   - `ns2.{MW_DOMAIN}` → `MW_PUBLIC_IP`
2. **Delegation (NS records)** — set the domain's nameservers to `ns1.{MW_DOMAIN}`
   and `ns2.{MW_DOMAIN}`.
3. **DNSSEC (recommended)** — submit the **DS record** printed on first boot
   (`docker compose logs pdns`, or
   `docker compose exec pdns pdnsutil --config-dir=/etc/powerdns export-zone-ds {MW_DOMAIN}`)
   to your registrar's DNSSEC / DS section.

> A single public IP for both `ns1` and `ns2` works but offers no redundancy.
> For resilience, run a second instance on a different IP and point `ns2` there.

## Least-privilege API key (mw-proxy)

MoonlightWeb instances get DNS access on a least-privilege basis and never hold
full PowerDNS credentials. `mw-proxy` is a tiny Go gateway that keeps the
**real** PowerDNS key server-side and exposes a **restricted** key to the app —
by design it cannot delete zones, rewrite NS/SOA, disable DNSSEC, dump records,
or reach any other zone:

- **Only this zone, only GET/PATCH.** Zone deletion, `/config`, `/cryptokeys`
  (DNSSEC), `/metadata` and every other zone are refused.
- **Only the records the app produces.** `{uid}.{zone}` A, `_owner.{uid}.{zone}`
  TXT, `_acme-challenge.{uid}.{zone}` TXT — where `uid` is 8 hex chars. This
  automatically excludes reserved labels (`www`, `api`, `dnsapi`, `stats`,
  `updates`, …).
  Any other name/type (e.g. an NS/SOA rewrite) → `403`.
- **No zone dumps.** `GET` must carry a valid `rrset_name` filter, so nobody can
  list the zone and harvest every subdomain.
- **Server-side ownership.** The first writer of a subdomain claims it
  (Trust-On-First-Use) by presenting a per-instance token in `X-MW-Owner`;
  `mw-proxy` stores `HMAC(token)` keyed by `uid` and requires the matching
  token on every later write. The token is **never** published in DNS, so it
  cannot be read and replayed. **One subdomain per owner** is enforced.
- **Bulk-creation limit.** New subdomains are rate-limited per client IP
  (`MW_PROXY_MAX_NEW_PER_HOUR`, default 20) on top of Caddy's 60 req/min.

Relevant `.env` values (generated by `install.sh`):

| Variable | Where | Meaning |
|---|---|---|
| `MW_PDNS_API_KEY` | DNS box only | The **full** PowerDNS key. Never distributed; used by pdns and injected by mw-proxy toward pdns. |
| `MW_PDNS_PROXY_KEY` | DNS box + app | The **restricted** key. Set it as `MW_PDNS_TOKEN` on the MoonlightWeb 0.2.0+ server. |
| `MW_PDNS_OWNER_SECRET` | DNS box only | HMAC key for the ownership store. Secret; keep stable (rotating it forgets ownership). |
| `MW_PROXY_MAX_NEW_PER_HOUR` | DNS box only | New-subdomain rate limit per IP (0 = unlimited). |

The ownership store persists in the `mwproxy_data` volume
(`/data/owners.json`). It holds only `uid → HMAC(token)` pairs — a leak reveals
no usable credential. Rebuilding the stack keeps it.

> **Endpoints.** MoonlightWeb registers through the restricted `dnsapi.`
> gateway. A direct `api.` → pdns route is also present for backward
> compatibility and is retired in a later release.

## Attack-surface hardening (built into this stack)

- **DNS amplification / flood** — dnsdist drops clients over ~50 qps
  (`MaxQPSIPRule`, tunable in `dnsdist/dnsdist.conf`) and forces `ANY` to TCP.
  PowerDNS keeps `disable-axfr`, `version-string=anonymous` and the VM hardening.
- **API brute-force** — Caddy throttles `api.{MW_DOMAIN}` to 60 req/min per IP
  (`caddy-ratelimit`, tunable in `caddy/Caddyfile.tmpl`) on top of the API key.
- **Least privilege** — `pdns` runs as a **non-root** user with `cap_drop: ALL`
  (its ports are non-privileged); `dnsdist`/`caddy` keep only `NET_BIND_SERVICE`.
  All three set `no-new-privileges` and `pids_limit` / `mem_limit` to contain
  resource-exhaustion attacks. The API port is never published.

### What this stack can NOT do

- **Volumetric DDoS** (hundreds of Gbps) cannot be absorbed by a single host —
  that needs upstream protection (provider DDoS scrubbing, anycast, or a managed
  secondary DNS). The `ns2` redundancy note above is the cheapest first step.
- **IP banning (fail2ban)** belongs on the Docker **host**, not inside a
  container (it needs to manage the host firewall). If you want it, run fail2ban
  on the host watching Caddy's access log and ban at the host nftables/iptables.
  The in-container mitigation here is Caddy's rate limiting (HTTP 429).

## Verify

```bash
# From another machine, once delegation has propagated:
dig +short NS example.top
dig +short api.example.top
curl -s https://api.example.top/api/v1/servers/localhost \
     -H "X-API-Key: $MW_PDNS_API_KEY"
```

## Notes

- **Persistence** — the zone DB lives in `pdns_data`, Caddy's certs in
  `caddy_data`; rebuilding keeps both. Remove the volumes to start fresh.
- **Idempotent init** — `pdns/init.sh` only creates the zone if absent, so
  restarts are safe. Per-instance subdomains are managed by MoonlightWeb.
- **API keys** — keep `MW_PDNS_API_KEY` (full, server-side only) and
  `MW_PDNS_PROXY_KEY` (restricted, used as the server's `MW_PDNS_TOKEN`) secret.
  Generate each with `openssl rand -hex 24`. See
  [Least-privilege API key](#least-privilege-api-key-mw-proxy).
