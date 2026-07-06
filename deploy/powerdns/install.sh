#!/usr/bin/env bash
# MoonlightWeb — one-shot installer for the self-hosted DNS stack.
#
# Brings a fresh Linux VM to a fully operational state:
#   - Docker engine + compose plugin
#   - host security tools (fail2ban, automatic security updates)
#   - host firewall (ufw / firewalld / nftables, best effort)
#   - frees port 53 (systemd-resolved stub listener)
#   - swap file on low-memory hosts (xcaddy build is memory hungry)
#   - interactive .env (required + optional variables)
#   - docker compose up -d --build
#
# Works across distributions: apt, dnf/yum, pacman, zypper, apk.
# Run from this directory:  sudo ./install.sh
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
cd "$HERE"

# ── pretty output ────────────────────────────────────────────────────────────
c_blue=$'\033[1;34m'; c_green=$'\033[1;32m'; c_yellow=$'\033[1;33m'
c_red=$'\033[1;31m'; c_bold=$'\033[1m'; c_reset=$'\033[0m'
step()  { printf '\n%s==>%s %s\n' "$c_blue"  "$c_reset" "$*"; }
ok()    { printf '%s  ok%s %s\n'  "$c_green" "$c_reset" "$*"; }
warn()  { printf '%s warn%s %s\n' "$c_yellow" "$c_reset" "$*"; }
die()   { printf '%s fail%s %s\n' "$c_red" "$c_reset" "$*" >&2; exit 1; }

# ── privileges ───────────────────────────────────────────────────────────────
if [ "$(id -u)" -ne 0 ]; then
    if command -v sudo >/dev/null 2>&1; then
        warn "re-running with sudo"
        exec sudo -E bash "$0" "$@"
    fi
    die "please run as root (or install sudo)"
fi

# Real (non-root) user, so we can add them to the docker group.
TARGET_USER="${SUDO_USER:-root}"

# ── package manager detection ────────────────────────────────────────────────
PM=""
for c in apt-get dnf yum pacman zypper apk; do
    if command -v "$c" >/dev/null 2>&1; then PM="$c"; break; fi
done
[ -n "$PM" ] || die "no supported package manager found (apt/dnf/yum/pacman/zypper/apk)"
ok "package manager: $PM"

pm_install() {
    # pm_install <pkg...> — best effort, never fatal (tools are optional extras).
    case "$PM" in
        apt-get) DEBIAN_FRONTEND=noninteractive apt-get install -y "$@" ;;
        dnf|yum) "$PM" install -y "$@" ;;
        pacman)  pacman -S --needed --noconfirm "$@" ;;
        zypper)  zypper --non-interactive install "$@" ;;
        apk)     apk add --no-cache "$@" ;;
    esac
}
pm_refresh() {
    case "$PM" in
        apt-get) apt-get update -y ;;
        dnf|yum) "$PM" makecache -y || true ;;
        pacman)  pacman -Sy --noconfirm ;;
        zypper)  zypper --non-interactive refresh ;;
        apk)     apk update ;;
    esac
}

step "Refreshing package index"
pm_refresh || warn "package index refresh failed — continuing"

# Base tools used by this script.
pm_install curl ca-certificates openssl || warn "could not install base tools"
# dig, for the final self-check (package name varies per distro) — best effort.
command -v dig >/dev/null 2>&1 || pm_install dnsutils 2>/dev/null || \
    pm_install bind-utils 2>/dev/null || pm_install bind-tools 2>/dev/null || true

# ── Docker + compose ─────────────────────────────────────────────────────────
step "Installing Docker"
if command -v docker >/dev/null 2>&1; then
    ok "docker already present: $(docker --version 2>/dev/null || echo unknown)"
else
    # get.docker.com supports all mainstream distros; fall back to native pkg.
    if curl -fsSL https://get.docker.com | sh; then
        ok "docker installed via get.docker.com"
    else
        warn "get.docker.com failed — trying native packages"
        case "$PM" in
            apt-get) pm_install docker.io docker-compose-plugin ;;
            dnf|yum) pm_install docker docker-compose-plugin ;;
            pacman)  pm_install docker docker-compose ;;
            zypper)  pm_install docker docker-compose ;;
            apk)     pm_install docker docker-cli-compose ;;
        esac
    fi
fi

systemctl enable --now docker 2>/dev/null || service docker start 2>/dev/null || \
    warn "could not start docker via init system"
command -v docker >/dev/null 2>&1 || die "docker not available after install"

# Resolve the compose command (plugin v2 vs legacy binary).
if docker compose version >/dev/null 2>&1; then
    COMPOSE="docker compose"
elif command -v docker-compose >/dev/null 2>&1; then
    COMPOSE="docker-compose"
else
    warn "docker compose plugin missing — installing"
    pm_install docker-compose-plugin docker-compose 2>/dev/null || true
    if docker compose version >/dev/null 2>&1; then COMPOSE="docker compose"
    else die "docker compose not available"; fi
fi
ok "compose command: $COMPOSE"

# Let the invoking user run docker without sudo (effective next login).
if [ "$TARGET_USER" != "root" ]; then
    usermod -aG docker "$TARGET_USER" 2>/dev/null && \
        ok "added $TARGET_USER to the docker group (re-login to take effect)"
fi

# ── Host security tools ──────────────────────────────────────────────────────
step "Installing host security tools (fail2ban, auto-updates)"
pm_install fail2ban || warn "fail2ban not installed (optional)"
if command -v fail2ban-server >/dev/null 2>&1; then
    # Protect SSH at the host level (defense in depth; the stack rate-limits the API).
    if [ ! -f /etc/fail2ban/jail.d/sshd.local ]; then
        mkdir -p /etc/fail2ban/jail.d
        cat > /etc/fail2ban/jail.d/sshd.local <<'EOF'
[sshd]
enabled = true
maxretry = 5
bantime = 1h
EOF
    fi
    systemctl enable --now fail2ban 2>/dev/null || service fail2ban restart 2>/dev/null || true
    ok "fail2ban active (sshd jail)"
fi

# Unattended security upgrades (Debian/Ubuntu) — best effort, distro-specific.
case "$PM" in
    apt-get)
        pm_install unattended-upgrades && \
            dpkg-reconfigure -f noninteractive unattended-upgrades 2>/dev/null || true
        ok "unattended-upgrades configured"
        ;;
    dnf)
        pm_install dnf-automatic && \
            systemctl enable --now dnf-automatic.timer 2>/dev/null || true
        ;;
esac

# ── Firewall ─────────────────────────────────────────────────────────────────
# Public ports for the stack: 22 (ssh), 53/udp+tcp (dnsdist), 80+443 (caddy).
step "Configuring host firewall"
fw_done=""
if command -v ufw >/dev/null 2>&1; then
    # Allow SSH FIRST, then enable — enabling before the ssh rule can lock us out.
    ufw allow 22/tcp   >/dev/null 2>&1 || true
    ufw allow 53/udp   >/dev/null 2>&1 || true
    ufw allow 53/tcp   >/dev/null 2>&1 || true
    ufw allow 80/tcp   >/dev/null 2>&1 || true
    ufw allow 443/tcp  >/dev/null 2>&1 || true
    ufw --force enable >/dev/null 2>&1 || true
    fw_done="ufw"
elif command -v firewall-cmd >/dev/null 2>&1; then
    # firewalld may be installed but inactive (common on RHEL/Fedora) — start it,
    # otherwise the --permanent rules are written but never enforced.
    systemctl enable --now firewalld >/dev/null 2>&1 || true
    firewall-cmd --permanent --add-port=22/tcp  >/dev/null 2>&1 || true
    firewall-cmd --permanent --add-port=53/udp  >/dev/null 2>&1 || true
    firewall-cmd --permanent --add-port=53/tcp  >/dev/null 2>&1 || true
    firewall-cmd --permanent --add-port=80/tcp  >/dev/null 2>&1 || true
    firewall-cmd --permanent --add-port=443/tcp >/dev/null 2>&1 || true
    firewall-cmd --reload >/dev/null 2>&1 || true
    fw_done="firewalld"
fi

# Verify (read-back): the firewall must be ACTIVE and every required port present.
if [ -n "$fw_done" ]; then
    fw_active=""; fw_rules=""; missing=""
    if [ "$fw_done" = "ufw" ]; then
        fw_rules="$(ufw status verbose 2>/dev/null || true)"
        printf '%s' "$fw_rules" | grep -qi 'Status: active' && fw_active=1
    else
        systemctl is-active --quiet firewalld 2>/dev/null && fw_active=1
        fw_rules="$(firewall-cmd --list-ports 2>/dev/null || true)"
    fi
    for p in 22/tcp 53/udp 53/tcp 80/tcp 443/tcp; do
        printf '%s' "$fw_rules" | grep -q "$p" || missing="$missing $p"
    done
    if [ -n "$fw_active" ] && [ -z "$missing" ]; then
        ok "host firewall ACTIVE ($fw_done): 22/tcp, 53/udp, 53/tcp, 80/tcp, 443/tcp verified open"
    else
        [ -z "$fw_active" ] && warn "host firewall ($fw_done) is NOT active — enable it, then re-run"
        [ -n "$missing" ]   && warn "firewall ($fw_done) missing rules:$missing — re-run or add manually"
        warn "current rules:"; printf '%s\n' "$fw_rules" | sed 's/^/         /'
    fi
else
    warn "no managed firewall (ufw/firewalld) detected — relying on your cloud"
    warn "security group / NSG. Make sure 53/udp, 53/tcp, 80/tcp, 443/tcp are open there."
fi

# ── Free port 53 (systemd-resolved stub) ─────────────────────────────────────
step "Checking port 53"
if ss -lunp 2>/dev/null | grep -q ':53 ' && [ -f /etc/systemd/resolved.conf ]; then
    warn "port 53 held by systemd-resolved — disabling its stub listener"
    sed -i 's/^#\?DNSStubListener=.*/DNSStubListener=no/' /etc/systemd/resolved.conf
    grep -q '^DNSStubListener=no' /etc/systemd/resolved.conf || \
        echo 'DNSStubListener=no' >> /etc/systemd/resolved.conf
    ln -sf /run/systemd/resolve/resolv.conf /etc/resolv.conf 2>/dev/null || true
    systemctl restart systemd-resolved 2>/dev/null || true
    sleep 1
fi
if ss -lunp 2>/dev/null | grep -q ':53 '; then
    warn "port 53 still in use — dnsdist may fail to bind. Free it, then re-run."
else
    ok "port 53 is free"
fi

# ── Swap (low memory hosts) ──────────────────────────────────────────────────
step "Checking memory / swap"
mem_kb=$(awk '/MemTotal/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)
swap_kb=$(awk '/SwapTotal/ {print $2}' /proc/meminfo 2>/dev/null || echo 0)
# Under ~2 GiB RAM and no swap: xcaddy (Go build) can OOM — add a 2G swap file.
if [ "$mem_kb" -lt 2097152 ] && [ "$swap_kb" -lt 1048576 ] && [ ! -f /swapfile ]; then
    warn "low RAM and no swap — creating a 2G swap file"
    fallocate -l 2G /swapfile 2>/dev/null || dd if=/dev/zero of=/swapfile bs=1M count=2048
    chmod 600 /swapfile
    mkswap /swapfile >/dev/null && swapon /swapfile
    grep -q '^/swapfile' /etc/fstab || echo '/swapfile none swap sw 0 0' >> /etc/fstab
    ok "2G swap enabled"
else
    ok "memory/swap fine (RAM ${mem_kb}kB, swap ${swap_kb}kB)"
fi

# ── Interactive .env ─────────────────────────────────────────────────────────
step "Configuring environment (.env)"

ask() {  # ask <var> <prompt> <default>  → echoes the answer
    local var="$1" prompt="$2" def="${3:-}" ans=""
    if [ -n "$def" ]; then
        read -r -p "  $prompt [$def]: " ans </dev/tty || true
        echo "${ans:-$def}"
    else
        read -r -p "  $prompt: " ans </dev/tty || true
        echo "$ans"
    fi
}

if [ -f .env ]; then
    warn ".env already exists — keeping it (delete it to reconfigure)"
    # Backfill keys added by newer versions (e.g. the Umami analytics secrets), so
    # upgrading an existing box doesn't leave docker compose with empty variables.
    gen_secret() { openssl rand -hex 24 2>/dev/null || head -c24 /dev/urandom | od -An -tx1 | tr -d ' \n'; }
    ensure_env() {  # ensure_env <KEY> <value>  — append KEY=value if absent
        grep -q "^$1=" .env || { printf '%s=%s\n' "$1" "$2" >> .env; ok "added missing $1 to .env"; }
    }
    ensure_env MW_UMAMI_DB_PASSWORD "$(gen_secret)"
    ensure_env MW_UMAMI_SECRET      "$(gen_secret)"
else
    detected_ip="$(curl -fsS https://api.ipify.org 2>/dev/null || true)"

    echo "  Required settings:"
    MW_DOMAIN=""
    while [ -z "$MW_DOMAIN" ]; do
        MW_DOMAIN="$(ask MW_DOMAIN 'Domain you own (e.g. example.top)')"
        [ -n "$MW_DOMAIN" ] || warn "domain is required"
    done

    MW_PUBLIC_IP=""
    while [ -z "$MW_PUBLIC_IP" ]; do
        MW_PUBLIC_IP="$(ask MW_PUBLIC_IP 'Public IPv4 of this VM' "$detected_ip")"
        [ -n "$MW_PUBLIC_IP" ] || warn "public IP is required"
    done

    gen_secret() { openssl rand -hex 24 2>/dev/null || head -c24 /dev/urandom | od -An -tx1 | tr -d ' \n'; }
    gen_key="$(gen_secret)"
    MW_PDNS_API_KEY="$(ask MW_PDNS_API_KEY 'PowerDNS API key (blank = generate)' "$gen_key")"

    # Analytics secrets — generated silently (no prompt; the user rarely cares).
    MW_UMAMI_DB_PASSWORD="$(gen_secret)"
    MW_UMAMI_SECRET="$(gen_secret)"

    echo "  Optional TLS (blank = automatic Let's Encrypt via Caddy):"
    MW_TLS_EMAIL="$(ask MW_TLS_EMAIL 'Email for Let'\''s Encrypt notices' '')"
    MW_TLS_CERT="$(ask MW_TLS_CERT 'Own cert path in ./certs (blank = none)' '')"
    MW_TLS_KEY=""
    [ -n "$MW_TLS_CERT" ] && MW_TLS_KEY="$(ask MW_TLS_KEY 'Own key path in ./certs' '')"

    umask 077
    cat > .env <<EOF
# Generated by install.sh on $(date -u +%Y-%m-%dT%H:%M:%SZ)
MW_DOMAIN=${MW_DOMAIN}
MW_PUBLIC_IP=${MW_PUBLIC_IP}
MW_PDNS_API_KEY=${MW_PDNS_API_KEY}
MW_TLS_EMAIL=${MW_TLS_EMAIL}
MW_TLS_CERT=${MW_TLS_CERT}
MW_TLS_KEY=${MW_TLS_KEY}
MW_UMAMI_DB_PASSWORD=${MW_UMAMI_DB_PASSWORD}
MW_UMAMI_SECRET=${MW_UMAMI_SECRET}
EOF
    [ "$TARGET_USER" != "root" ] && chown "$TARGET_USER" .env 2>/dev/null || true
    ok ".env written"
fi

# Load values back (for the final summary), tolerating quotes/spaces.
set -a; . ./.env; set +a

# ── Bring the stack up ───────────────────────────────────────────────────────
step "Building and starting the stack (this can take a few minutes)"

# The caddy image compiles Caddy from source via xcaddy (Go build) — slow on
# low-RAM VMs. Go has no native progress, so we approximate: the Dockerfile runs
# `go build -v`, which prints each package as it compiles; --progress=plain
# streams those lines out of BuildKit and awk counts them against a rough total
# (Caddy + caddy-ratelimit ≈ 540 pkgs) to render a live bar. Field 2 of a plain
# BuildKit line is the elapsed time, which we reuse for the timer.
build_caddy_with_progress() {
    $COMPOSE build --progress=plain caddy 2>&1 | awk '
        BEGIN { total = 540; el = 0 }
        $2 ~ /^[0-9]+(\.[0-9]+)?$/ { el = $2 + 0 }
        $3 ~ /^[a-z0-9._-]+\.[a-z0-9._-]+\// {
            n++
            pct = int(n * 100 / total); if (pct > 99) pct = 99
            sec = int(el); mm = int(sec / 60); ss = sec % 60
            bars = int(pct / 2); bar = ""
            for (i = 0; i < bars; i++)  bar = bar "#"
            for (i = bars; i < 50; i++) bar = bar "-"
            printf "\r  compiling caddy [%s] %3d%%  (%d pkgs, %dm%02ds) ", bar, pct, n, mm, ss
            fflush()
        }
        END {
            if (n == 0) { print "  caddy image up to date (cached)" }
            else {
                bar = ""; for (i = 0; i < 50; i++) bar = bar "#"
                printf "\r  compiling caddy [%s] 100%%  (%d pkgs) done          \n", bar, n
            }
        }
    '
}
build_caddy_with_progress
$COMPOSE up -d --build

echo
$COMPOSE ps || true

# Grab the DS record for the registrar (first boot prints it in the pdns log).
DS_RECORD="$($COMPOSE exec -T pdns pdnsutil --config-dir=/etc/powerdns export-zone-ds "$MW_DOMAIN" 2>/dev/null || true)"

# ── Final summary + persistent to-do list ────────────────────────────────────
SUMMARY="NEXT-STEPS.txt"
{
echo "============================================================"
echo " MoonlightWeb DNS stack — INSTALL COMPLETE"
echo "============================================================"
echo
echo " Your VM is up and running. Containers:"
echo "   pdns     PowerDNS authoritative + REST API (internal only)"
echo "   dnsdist  public DNS front on :53 (UDP/TCP)"
echo "   caddy    public API https://api.${MW_DOMAIN:-<MW_DOMAIN>} (:80/:443)"
echo "   umami    web analytics  https://stats.${MW_DOMAIN:-<MW_DOMAIN>} (internal, via caddy)"
echo "   umami-db Postgres for Umami (internal only)"
echo
echo " Where to change things later:"
echo "   - Variables ............ $HERE/.env   (then: $COMPOSE up -d --build)"
echo "   - DNS rate limits ...... $HERE/dnsdist/dnsdist.conf"
echo "   - API rate limit / TLS . $HERE/caddy/Caddyfile.tmpl"
echo "   - Presentation website . $HERE/../../website/   (then: $COMPOSE restart caddy)"
echo "   - PowerDNS hardening ... $HERE/pdns/zz-mw.conf"
echo "   - Restart stack ........ $COMPOSE restart"
echo "   - View logs ............ $COMPOSE logs -f pdns"
echo
echo " ┌────────────────────────────────────────────────────────┐"
echo " │  TODO — finish your DNS infrastructure setup            │"
echo " └────────────────────────────────────────────────────────┘"
echo
echo "  [ ] 1. Open these ports on your CLOUD firewall / NSG"
echo "         (Azure NSG, AWS SG, GCP firewall, etc.):"
echo "            53/udp   public DNS"
echo "            53/tcp   public DNS (truncated answers, DNSSEC)"
echo "            80/tcp   Let's Encrypt HTTP-01 challenge"
echo "            443/tcp  PowerDNS REST API (api.${MW_DOMAIN:-<MW_DOMAIN>})"
echo "            22/tcp   SSH — restrict to YOUR IP only"
echo
echo "  [ ] 2. At your domain REGISTRAR (where you bought the domain):"
echo "         - Create host/glue records pointing to ${MW_PUBLIC_IP:-<MW_PUBLIC_IP>}:"
echo "              ns1.${MW_DOMAIN:-<MW_DOMAIN>}  ->  ${MW_PUBLIC_IP:-<MW_PUBLIC_IP>}"
echo "              ns2.${MW_DOMAIN:-<MW_DOMAIN>}  ->  ${MW_PUBLIC_IP:-<MW_PUBLIC_IP>}"
echo "         - Set the domain's nameservers (delegation) to:"
echo "              ns1.${MW_DOMAIN:-<MW_DOMAIN>}"
echo "              ns2.${MW_DOMAIN:-<MW_DOMAIN>}"
echo
echo "  [ ] 3. Submit the DNSSEC DS record to your registrar (recommended):"
if [ -n "$DS_RECORD" ]; then
    echo "$DS_RECORD" | sed 's/^/            /'
else
    echo "            (run: $COMPOSE exec pdns pdnsutil \\"
    echo "                   --config-dir=/etc/powerdns export-zone-ds ${MW_DOMAIN:-<MW_DOMAIN>} )"
fi
echo
echo "  [ ] 4. On your MoonlightWeb server, set in its own .env:"
echo "            MW_DOMAIN=${MW_DOMAIN:-<MW_DOMAIN>}"
echo "            MW_PDNS_URL=https://api.${MW_DOMAIN:-<MW_DOMAIN>}/api/v1/servers/localhost"
echo "            MW_PDNS_TOKEN=<the MW_PDNS_API_KEY value from $HERE/.env>"
echo
echo "  [ ] 5. Verify once DNS delegation has propagated (minutes to hours):"
echo "            dig +short NS ${MW_DOMAIN:-<MW_DOMAIN>}"
echo "            dig +short api.${MW_DOMAIN:-<MW_DOMAIN>}"
echo
echo "  [ ] 6. Turn ON website analytics (Umami):"
echo "         a) open  https://stats.${MW_DOMAIN:-<MW_DOMAIN>}   (default login: admin / umami)"
echo "         b) change the admin password immediately"
echo "         c) Settings -> Websites -> Add: name 'MoonlightWeb', domain '${MW_DOMAIN:-<MW_DOMAIN>}'"
echo "         d) copy the website's ID and paste it into the data-website-id"
echo "            attribute in $HERE/../../website/index.html, then: $COMPOSE restart caddy"
echo "         Visits, regions and clicks (Download / GitHub / Buy me a coffee)"
echo "         then show up in the Umami dashboard."
echo
echo " This list is saved to: $HERE/$SUMMARY"
echo "============================================================"
} | tee "$HERE/$SUMMARY"
[ "$TARGET_USER" != "root" ] && chown "$TARGET_USER" "$HERE/$SUMMARY" 2>/dev/null || true

# ── Self-check — what the public Internet actually sees ─────────────────────
# dns.google over HTTPS shows the world's view of the zone even from this VM,
# which cannot otherwise test its own INBOUND port 53 (cloud firewall / NSG).
step "Self-check — public DNS reachability"

if command -v dig >/dev/null 2>&1; then
    if [ -n "$(dig +short +time=3 SOA "$MW_DOMAIN" @127.0.0.1 2>/dev/null)" ]; then
        ok "local stack answers DNS queries on 127.0.0.1:53"
    else
        warn "local stack does NOT answer on 127.0.0.1:53 — check: $COMPOSE logs dnsdist pdns"
    fi
fi

doh_a="$(curl -fsS -m 10 "https://dns.google/resolve?name=api.${MW_DOMAIN}&type=A" 2>/dev/null || true)"
case "$doh_a" in
    *'"Status":0'*)
        if printf '%s' "$doh_a" | grep -q "\"data\":\"${MW_PUBLIC_IP}\""; then
            ok "public resolvers reach this VM (api.${MW_DOMAIN} -> ${MW_PUBLIC_IP})"
        else
            warn "api.${MW_DOMAIN} publicly resolves to a DIFFERENT address than ${MW_PUBLIC_IP}:"
            printf '%s' "$doh_a" | grep -o '"data":"[^"]*"' | sed 's/^/         /'
            warn "another DNS server still answers for this zone (stale registrar glue,"
            warn "cached delegation, or a PREVIOUS VM still running) — shut the old"
            warn "server down and/or update the ns1/ns2 glue records, then retest."
        fi
        ;;
    *'"Status":2'*)
        warn "public resolvers get SERVFAIL for ${MW_DOMAIN}: the delegation points here"
        warn "but resolvers cannot reach this VM on port 53. Almost always the CLOUD"
        warn "firewall (Azure NSG / AWS SG / GCP): open 53/udp AND 53/tcp inbound,"
        warn "then retest with:  dig NS ${MW_DOMAIN} @8.8.8.8"
        ;;
    *'"Status":3'*)
        warn "public resolvers get NXDOMAIN for ${MW_DOMAIN}: the registrar delegation"
        warn "(ns1/ns2 glue -> ${MW_PUBLIC_IP}) is missing or has not propagated yet"
        warn "(minutes to hours). See step 2 of the TODO list above."
        ;;
    '')
        warn "could not query dns.google over HTTPS — skipped the public check"
        ;;
    *)
        warn "unexpected answer from dns.google — inspect manually: dig NS ${MW_DOMAIN} @8.8.8.8"
        ;;
esac

printf '\n%s%sAll set — your VM is operational.%s See the TODO list above.\n' \
    "$c_bold" "$c_green" "$c_reset"
