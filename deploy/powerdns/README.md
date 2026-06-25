# Self-hosted PowerDNS for Moonlight-Web (Docker)

This stack lets anyone host their own DNS for Moonlight-Web's **Internet Access**
feature. It runs PowerDNS (authoritative, SQLite + DNSSEC) plus a Caddy reverse
proxy that exposes the PowerDNS REST API over HTTPS — the same layout as the
reference VM, packaged as Alpine containers.

The Moonlight-Web server itself is **not** in this stack. It talks to this DNS
box over the REST API (`MW_PDNS_URL` + `MW_PDNS_TOKEN`) and creates per-instance
subdomains `{uniqueId}.{MW_DOMAIN}` at runtime. This stack only bootstraps the
parent zone, the nameserver glue and the `api.{MW_DOMAIN}` host.

## Contents

```
deploy/powerdns/
├── docker-compose.yml   # pdns + caddy services
├── Dockerfile           # PowerDNS on Alpine (SQLite backend + DNSSEC)
├── entrypoint.sh        # idempotent schema + zone init, then runs pdns
├── pdns/pdns.conf.tmpl  # PowerDNS config (API key injected at boot)
├── caddy/Caddyfile      # TLS reverse proxy → api.{MW_DOMAIN}
└── .env.sample          # copy to .env and fill in
```

## Quick start

```bash
cd deploy/powerdns
cp .env.sample .env
# edit .env: MW_DOMAIN, MW_PUBLIC_IP, MW_PDNS_API_KEY
docker compose up -d --build
docker compose logs -f pdns   # note the DS record printed on first boot
```

On the Moonlight-Web server, set in its own `.env`:

```
MW_DOMAIN=example.top
MW_PDNS_URL=https://api.example.top/api/v1/servers/localhost
MW_PDNS_TOKEN=<same value as MW_PDNS_API_KEY here>
```

## Ports / protocols to open publicly

Open these on the host firewall **and** any cloud security group, forwarded to
this machine. DNS needs both UDP and TCP (TCP is used for large/truncated
answers and DNSSEC).

| Port | Protocol | Service | Why it must be public |
|------|----------|---------|-----------------------|
| 53   | UDP      | PowerDNS (DNS) | Resolvers query your zone — primary path |
| 53   | TCP      | PowerDNS (DNS) | Fallback for truncated responses, DNSSEC |
| 80   | TCP      | Caddy (HTTP)   | ACME HTTP-01 challenge for the API cert |
| 443  | TCP      | Caddy (HTTPS)  | PowerDNS REST API exposed as `api.{MW_DOMAIN}` |

The PowerDNS API port `8081` stays **internal** to the Docker network — never
expose it directly; Caddy terminates TLS in front of it.

## Register the nameserver at your domain provider

To make your zone authoritative on the public Internet, configure this at your
**domain registrar** (where you bought `MW_DOMAIN`):

1. **Glue records / host records** — register your nameservers and point them at
   this box's public IP (`MW_PUBLIC_IP`):
   - `ns1.{MW_DOMAIN}` → `MW_PUBLIC_IP`
   - `ns2.{MW_DOMAIN}` → `MW_PUBLIC_IP`
2. **Delegation (NS records)** — set the domain's nameservers to:
   - `ns1.{MW_DOMAIN}`
   - `ns2.{MW_DOMAIN}`
3. **DNSSEC (recommended)** — submit the **DS record** printed on first boot
   (`docker compose logs pdns`, or run
   `docker compose exec pdns pdnsutil --config-dir=/etc/pdns export-zone-ds {MW_DOMAIN}`)
   to your registrar's DNSSEC / DS section.

> A single public IP for both `ns1` and `ns2` works but offers no redundancy.
> For resilience, run a second instance on a different IP and point `ns2` there.

## Verify

```bash
# From another machine, once delegation has propagated:
dig +short NS example.top
dig +short api.example.top
curl -s https://api.example.top/api/v1/servers/localhost \
     -H "X-API-Key: $MW_PDNS_API_KEY"
```

## Notes

- **Persistence** — the zone DB lives in the `pdns_data` volume; rebuilding the
  image keeps your records. Remove the volume to start fresh.
- **Idempotent init** — `entrypoint.sh` only creates the zone if it is absent,
  so restarts are safe. Per-instance subdomains are managed by Moonlight-Web,
  not here.
- **API key** — keep `MW_PDNS_API_KEY` secret and identical to the server's
  `MW_PDNS_TOKEN`. Generate one with `openssl rand -hex 24`.
- **Firewall scope** — `webserver-allow-from` in `pdns.conf.tmpl` allows the
  private Docker/LAN ranges. Tighten it if your topology differs.
