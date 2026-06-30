# Self-hosted DNS stack for MoonlightWeb (Docker)

Three dedicated containers (one process each — the idiomatic Docker layout):

```
Internet ─:53──────> [dnsdist] ──> pdns:5300       anti-amplification + DNS rate-limit
Internet ─:80/:443─> [caddy]   ──> pdns:8081       HTTPS API, API rate-limit, auto-TLS
                     [pdns]    (internal, non-root) PowerDNS authoritative + REST API
```

- **dnsdist** — official `powerdns/dnsdist-19` image. The public DNS entry point;
  per-client rate limiting and ANY→TCP to neutralise reflection/amplification.
- **pdns** — official `powerdns/pdns-auth-49` image, **unmodified**. DNS on the
  internal port `5300`, REST API on `8081`. Never published to the host.
- **caddy** — official Caddy + the `caddy-ratelimit` plugin (built via xcaddy).
  Exposes the API as `https://api.{MW_DOMAIN}` with automatic TLS and request
  rate limiting, **and serves the static presentation website** at
  `https://{MW_DOMAIN}` / `https://www.{MW_DOMAIN}` (repo-root `website/`,
  bind-mounted into the container).

The MoonlightWeb server itself is **not** in this stack. It talks to this DNS
box over the REST API (`MW_PDNS_URL` + `MW_PDNS_TOKEN`) and creates per-instance
subdomains `{uniqueId}.{MW_DOMAIN}` at runtime. This stack only bootstraps the
parent zone, the nameserver glue, the `api.{MW_DOMAIN}` host and the `www`
record for the presentation site.

## Presentation website

Caddy also serves a static marketing/landing page for the project at the apex
domain and `www`:

- `https://{MW_DOMAIN}` — the canonical landing page (`website/index.html` at the
  repo root).
- `https://www.{MW_DOMAIN}` — permanently redirected to the apex.

Both get their own automatic Let's Encrypt certificate (independent of any
api-only cert you may supply via `MW_TLS_CERT`). The site is plain HTML/CSS with
the project screenshots under `website/assets/`. It is **bind-mounted** into the
Caddy container (`../../website → /srv/site`), so edit it freely then
`docker compose restart caddy` — no rebuild needed. The zone bootstrap adds the
`www` A record automatically; the apex `@` A record already existed.

## Contents

```
deploy/powerdns/
├── install.sh               # one-shot installer (Docker, security, firewall, up)
├── docker-compose.yml       # dnsdist + pdns + caddy
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

On the MoonlightWeb server, set in its own `.env`:

```
MW_DOMAIN=example.top
MW_PDNS_URL=https://api.example.top/api/v1/servers/localhost
MW_PDNS_TOKEN=<same value as MW_PDNS_API_KEY in deploy/powerdns/.env>
```

### Manual install (without the script)

```bash
cd deploy/powerdns
cp .env.sample .env
# edit .env: MW_DOMAIN, MW_PUBLIC_IP, MW_PDNS_API_KEY
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
| 443  | TCP      | caddy     | `api.{MW_DOMAIN}` REST API + presentation site (`{MW_DOMAIN}`/`www`) |

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
- **API key** — keep `MW_PDNS_API_KEY` secret and identical to the server's
  `MW_PDNS_TOKEN`. Generate one with `openssl rand -hex 24`.
