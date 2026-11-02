#!/usr/bin/env python3
"""Check that the project's names and server address are on no blocklist.

Since 0.3.0 every connection goes through stream.{domain}: a blocklist that
catches one of the names breaks MoonlightWeb outright for whoever it protects,
and nobody sees it from here. Run before a release, or whenever in doubt:

    python scripts/check-domain-reputation.py              # everything (~70 MB of feeds)
    python scripts/check-domain-reputation.py --no-feeds   # DNS + HTTP lookups only

What it asks, for every name the stack serves and every address they resolve to:
  - IP blocklists (DNSBL): Spamhaus ZEN, SpamCop, Barracuda, UCEPROTECT 1-3, ...
  - domain blocklists: Spamhaus DBL, SURBL, URIBL (asked at its own servers: it
    refuses large resolvers), SpamEatingMonkey, nordspam;
  - Google Safe Browsing and the Spamhaus domain record;
  - filtering resolvers (Cloudflare security and family, AdGuard, Quad9): each
    must answer the same addresses as an unfiltered one;
  - threat and filter feeds (OpenPhish, Phishing Army, HaGeZi TIF,
    OISD, FireHOL, Spamhaus DROP) and HaGeZi's spam-TLD list, which blocks whole
    TLDs (.top among them) save for an allowlist;
  - HaGeZi's general lists (Pro, Ultimate): a name the app can live without is
    a warning there, any other name is a failure;
  - the records that keep the domain from being used to send phishing: SPF,
    DMARC, null MX.

Spamhaus refuses queries from public resolvers (Google, Cloudflare...): run it
from a machine whose system resolver is the ISP's, or its answers come back
"refused" and are reported as unknown, never as clean.

Exit status: 1 if anything is listed, 0 otherwise (warnings and unknowns are
printed, they do not fail the run). Some services cannot be asked from a
script (captcha, no public API); their links are printed at the end.
"""
import argparse
import base64
import ipaddress
import json
import random
import socket
import struct
import sys
import time
import urllib.request

try:  # the OS verifier, when installed: it fetches missing roots as browsers do
    import truststore
    truststore.inject_into_ssl()
except ImportError:
    pass

NAMES = ("", "www", "stats", "stream", "stream.dev", "ns1", "ns2", "api",
         "dnsapi", "updates", "metrics", "packages", "app")

IP_DNSBLS = ("zen.spamhaus.org", "bl.spamcop.net", "b.barracudacentral.org",
             "psbl.surriel.com", "bl.mailspike.net", "dnsbl-1.uceprotect.net",
             "dnsbl-2.uceprotect.net", "dnsbl-3.uceprotect.net",
             "dnsbl.dronebl.org", "bl.blocklist.de", "ix.dnsbl.manitu.net",
             "all.s5h.net", "dnsbl.spfbl.net", "spam.spamrats.com")
DOMAIN_DNSBLS = ("dbl.spamhaus.org", "multi.surbl.org",
                 "uribl.spameatingmonkey.net", "dbl.nordspam.com")
URIBL = "multi.uribl.com"
URIBL_NS = "aa.uribl.com"

# name: (DoH URL or None, UDP server or None)
FILTERING_RESOLVERS = {
    # Over UDP: Python reads the Windows root store as it is on disk, and the
    # root behind Cloudflare's DoH names is one Windows only fetches on demand.
    "Cloudflare security": (None, "1.1.1.2"),
    "Cloudflare family": (None, "1.1.1.3"),
    "AdGuard": ("https://dns.adguard-dns.com/dns-query", None),
    "Quad9": (None, "9.9.9.9"),  # its DoH wants HTTP/2, which urllib lacks
}
# Unfiltered, asked over plain UDP: no CA store to get wrong.
REFERENCE = "1.1.1.1"

DOMAIN_FEEDS = {
    "OpenPhish": "https://openphish.com/feed.txt",
    "Phishing Army": "https://phishing.army/download/phishing_army_blocklist_extended.txt",
    "HaGeZi TIF": "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/tif-onlydomains.txt",
    "OISD big": "https://big.oisd.nl/domainswild",
}
IP_FEEDS = {
    "FireHOL level 1": "https://raw.githubusercontent.com/firehol/blocklist-ipsets/master/firehol_level1.netset",
    "FireHOL level 2": "https://raw.githubusercontent.com/firehol/blocklist-ipsets/master/firehol_level2.netset",
    "FireHOL level 3": "https://raw.githubusercontent.com/firehol/blocklist-ipsets/master/firehol_level3.netset",
    "Spamhaus DROP": "https://www.spamhaus.org/drop/drop.txt",
}
HAGEZI_GENERAL = {
    "HaGeZi Pro": "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/pro-onlydomains.txt",
    "HaGeZi Ultimate": "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/wildcard/ultimate-onlydomains.txt",
}
# Counters only: blocking them costs a statistic, never a stream.
DISPENSABLE = ("metrics", "stats")
SPAM_TLDS = "https://raw.githubusercontent.com/hagezi/dns-blocklists/main/adblock/spam-tlds.txt"

UA = "Mozilla/5.0 (compatible; moonlightweb-reputation-check)"

results = []  # (level, what, detail)


def report(level, what, detail=""):
    results.append((level, what, detail))
    print(f"  {level:7} {what}" + (f" - {detail}" if detail else ""), flush=True)


# --- DNS on the wire: the standard library has no resolver to aim at a server --

def dns_query(name, qtype=1):
    header = struct.pack(">HHHHHH", random.randrange(65536), 0x0100, 1, 0, 0, 0)
    labels = b"".join(bytes([len(p)]) + p.encode() for p in name.rstrip(".").split("."))
    return header + labels + b"\0" + struct.pack(">HH", qtype, 1)


def skip_name(msg, i):
    while True:
        n = msg[i]
        if n & 0xC0 == 0xC0:
            return i + 2
        if n == 0:
            return i + 1
        i += n + 1


def dns_parse(msg, qtype=1):
    """(rcode, [answers of qtype]) — A as dotted quads, TXT as joined strings."""
    rcode = msg[3] & 0x0F
    qd, an = struct.unpack(">HH", msg[4:8])
    i = 12
    for _ in range(qd):
        i = skip_name(msg, i) + 4
    out = []
    for _ in range(an):
        i = skip_name(msg, i)
        rtype, _cls, _ttl, rdlen = struct.unpack(">HHIH", msg[i:i + 10])
        i += 10
        data = msg[i:i + rdlen]
        if rtype == qtype == 1:
            out.append(".".join(map(str, data)))
        elif rtype == qtype == 16:
            parts, j = [], 0
            while j < len(data):
                parts.append(data[j + 1:j + 1 + data[j]].decode(errors="replace"))
                j += 1 + data[j]
            out.append("".join(parts))
        elif rtype == qtype:
            out.append(data)
        i += rdlen
    return rcode, out


def udp_resolve(name, server, qtype=1, timeout=5):
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.settimeout(timeout)
        s.sendto(dns_query(name, qtype), (server, 53))
        return dns_parse(s.recv(4096), qtype)


def doh_resolve(name, url, qtype=1):
    q = base64.urlsafe_b64encode(dns_query(name, qtype)).decode().rstrip("=")
    req = urllib.request.Request(f"{url}?dns={q}", headers={"accept": "application/dns-message"})
    with urllib.request.urlopen(req, timeout=10) as r:
        return dns_parse(r.read(), qtype)


def system_lookup(name):
    """A records through the system resolver; [] for NXDOMAIN."""
    try:
        return socket.gethostbyname_ex(name)[2]
    except socket.gaierror:
        return []


def fetch(url, timeout=120):
    req = urllib.request.Request(url, headers={"user-agent": UA})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read().decode(errors="replace")


# --- checks -------------------------------------------------------------------

def is_refusal(answer):
    # Spamhaus answers 127.255.255.x to a refused resolver; URIBL and SURBL 127.0.0.1.
    return answer.startswith("127.255.255.") or answer == "127.0.0.1"


# Policy listings say "this address is not a mail server" (end-user, NAT, web
# host), not "it sent spam". True of every address here, which sends no mail.
POLICY_CODES = {
    "zen.spamhaus.org": {"127.0.0.10", "127.0.0.11"},  # PBL
    "dnsbl.spfbl.net": {"127.0.0.4"},
}


def check_dnsbl(query, label, zone=""):
    try:
        answers = system_lookup(query)
    except OSError as e:
        return report("UNKNOWN", label, str(e))
    listed = [a for a in answers if a.startswith("127.") and not is_refusal(a)]
    policy = [a for a in listed if a in POLICY_CODES.get(zone, ())]
    listed = [a for a in listed if a not in policy]
    if policy and not listed:
        report("OK", label, f"policy listing {' '.join(policy)} (not a mail server), harmless")
    elif listed:
        report("FAIL", label, "listed " + " ".join(listed))
    elif answers:
        report("UNKNOWN", label, "query refused (" + " ".join(answers) + "): use the ISP's resolver")


def check_ips(ips):
    print("\nIP blocklists", flush=True)
    if not ips:
        return report("UNKNOWN", "IP blocklists", "no address resolved")
    if "127.0.0.10" not in system_lookup("2.0.0.127.zen.spamhaus.org"):
        report("WARN", "Spamhaus test point", "not answered as listed: Spamhaus results below are unusable")
    for ip in sorted(ips):
        rev = ".".join(reversed(ip.split(".")))
        for zone in IP_DNSBLS:
            check_dnsbl(f"{rev}.{zone}", f"{ip} on {zone}", zone)
    report("OK", f"{len(ips)} address(es) against {len(IP_DNSBLS)} DNSBLs", "anything listed is above")


def check_domains(fqdns):
    print("\nDomain blocklists", flush=True)
    for fq in fqdns:
        for zone in DOMAIN_DNSBLS:
            check_dnsbl(f"{fq}.{zone}", f"{fq} on {zone}")
    try:
        ns = system_lookup(URIBL_NS)[0]
        rcode, test = udp_resolve(f"test.uribl.com.{URIBL}", ns)
        if not test:
            report("UNKNOWN", "URIBL", "its test point is not listed")
        for fq in fqdns:
            _rc, ans = udp_resolve(f"{fq}.{URIBL}", ns)
            listed = [a for a in ans if a != "127.0.0.1"]
            if listed:
                report("FAIL", f"{fq} on {URIBL}", "listed " + " ".join(listed))
    except (OSError, IndexError) as e:
        report("UNKNOWN", "URIBL", str(e))
    report("OK", f"{len(fqdns)} names against {len(DOMAIN_DNSBLS) + 1} domain lists", "anything listed is above")


def check_safe_browsing(fqdns):
    print("\nGoogle Safe Browsing", flush=True)
    for fq in fqdns:
        url = f"https://transparencyreport.google.com/transparencyreport/api/v3/safebrowsing/status?site={fq}"
        status = None
        for pause in (1, 10):  # back to back, Google answers with a captcha page
            time.sleep(pause)
            try:
                body = fetch(url, 20)
                status = json.loads(body[body.index("["):])[0][1]
                break
            except Exception as e:
                error = e
        if status is None:
            report("UNKNOWN", fq, f"no answer ({error.__class__.__name__}; Google may want a captcha)")
            continue
        if status in (2, 3):
            report("FAIL", fq, f"Safe Browsing status {status}: unsafe content")
        elif status != 1:
            report("OK", fq, f"status {status} (no data)")
    report("OK", "Safe Browsing", "nothing unsafe reported above")


def check_spamhaus_record(domain):
    print("\nSpamhaus domain record", flush=True)
    url = f"https://www.spamhaus.org/api/v1/sia-proxy/api/intel/v2/byobject/domain/{domain}/overview"
    try:
        d = json.loads(fetch(url, 30))
    except Exception as e:
        return report("UNKNOWN", domain, str(e))
    if d.get("abused"):
        report("FAIL", domain, f"marked abused (tags {d.get('tags')})")
    else:
        report("OK", domain, f"not abused, score {d.get('score')}")


def check_filtering_resolvers(fqdns):
    print("\nFiltering resolvers", flush=True)
    reference = {}
    for fq in fqdns:
        try:
            reference[fq] = set(udp_resolve(fq, REFERENCE)[1])
        except Exception as e:
            report("UNKNOWN", f"{fq} (reference)", str(e))
    if not reference:
        return report("UNKNOWN", "filtering resolvers", "no reference answer to compare with")
    for name, (doh, udp) in FILTERING_RESOLVERS.items():
        bad = []
        for fq, want in reference.items():
            try:
                rcode, got = doh_resolve(fq, doh) if doh else udp_resolve(fq, udp)
            except Exception as e:
                report("UNKNOWN", f"{fq} via {name}", str(e))
                continue
            if set(got) != want:
                bad.append(f"{fq} -> rcode {rcode} {' '.join(got) or '(nothing)'}")
        if bad:
            report("FAIL", name, "; ".join(bad))
        else:
            report("OK", name, f"{len(reference)} names resolve as unfiltered")


def feed_hits(text, domain):
    return {w.strip("*.|^") for line in text.lower().splitlines() if domain in line
            for w in line.replace("\t", " ").split() if w.strip("*.|^").endswith(domain)}


def check_feeds(domain, ips):
    print("\nThreat and filter feeds", flush=True)
    for name, url in DOMAIN_FEEDS.items():
        try:
            hits = feed_hits(fetch(url), domain)
        except Exception as e:
            report("UNKNOWN", name, str(e))
            continue
        report("FAIL" if hits else "OK", name, ", ".join(sorted(hits)))
    dispensable = {f"{n}.{domain}" for n in DISPENSABLE}
    for name, url in HAGEZI_GENERAL.items():
        try:
            hits = feed_hits(fetch(url), domain)
        except Exception as e:
            report("UNKNOWN", name, str(e))
            continue
        level = "FAIL" if hits - dispensable else "WARN" if hits else "OK"
        report(level, name, ", ".join(sorted(hits)))
    addrs = [ipaddress.ip_address(ip) for ip in ips]
    for name, url in IP_FEEDS.items():
        try:
            nets = []
            for line in fetch(url).splitlines():
                tok = line.split(";")[0].split("#")[0].strip()
                if tok:
                    try:
                        nets.append(ipaddress.ip_network(tok, strict=False))
                    except ValueError:
                        pass
        except Exception as e:
            report("UNKNOWN", name, str(e))
            continue
        hits = sorted({f"{a} in {n}" for a in addrs for n in nets if a.version == n.version and a in n})
        report("FAIL" if hits else "OK", name, ", ".join(hits))


def check_spam_tlds(domain):
    print("\nWhole-TLD blocks", flush=True)
    tld = domain.rsplit(".", 1)[-1]
    try:
        text = fetch(SPAM_TLDS)
    except Exception as e:
        return report("UNKNOWN", "HaGeZi spam TLDs", str(e))
    rule = next((l for l in text.splitlines() if l.startswith(f"||*.{tld}^")), None)
    if rule is None:
        return report("OK", "HaGeZi spam TLDs", f".{tld} is not blocked")
    allowed = rule.split("denyallow=", 1)[1].split("|") if "denyallow=" in rule else []
    if domain in allowed:
        report("OK", "HaGeZi spam TLDs", f".{tld} is blocked, {domain} is allowlisted")
    else:
        report("WARN", "HaGeZi spam TLDs", f".{tld} is blocked and {domain} is not allowlisted: "
               "every name breaks for that list's users")


def check_mail_records(domain):
    print("\nAnti-spoofing records", flush=True)
    try:
        spf = [t for t in udp_resolve(domain, REFERENCE, 16)[1] if t.startswith("v=spf1")]
        dmarc = [t for t in udp_resolve(f"_dmarc.{domain}", REFERENCE, 16)[1] if t.startswith("v=DMARC1")]
        mx = udp_resolve(domain, REFERENCE, 15)[1]
    except Exception as e:
        return report("UNKNOWN", "SPF/DMARC/MX", str(e))
    report("OK" if spf else "WARN", "SPF", spf[0] if spf else "missing: anyone can send mail as the domain")
    report("OK" if dmarc else "WARN", "DMARC", dmarc[0] if dmarc else "missing")
    report("OK" if mx else "WARN", "MX", f"{len(mx)} record(s)" if mx else "none: add a null MX (0 .)")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    ap.add_argument("--domain", default="moonlightweb.top")
    ap.add_argument("--no-feeds", action="store_true", help="skip the downloaded feeds")
    args = ap.parse_args()
    domain = args.domain.lower().rstrip(".")
    fqdns = [f"{n}.{domain}" if n else domain for n in NAMES]

    print(f"Names of {domain}", flush=True)
    ips = set()
    for fq in fqdns:
        try:
            got = udp_resolve(fq, REFERENCE)[1]
        except Exception as e:
            got = []
            report("UNKNOWN", fq, str(e))
        ips.update(got)
        print(f"  {fq:34} {' '.join(got) or '(no address)'}")

    check_ips(ips)
    check_domains(fqdns)
    check_safe_browsing(fqdns)
    check_spamhaus_record(domain)
    check_filtering_resolvers(fqdns)
    if not args.no_feeds:
        check_feeds(domain, ips)
    check_spam_tlds(domain)
    check_mail_records(domain)

    print("\nBy hand (captcha or no public API):")
    for ip in sorted(ips):
        print(f"  https://www.abuseipdb.com/check/{ip}")
    print(f"  https://www.virustotal.com/gui/domain/{domain}")
    print(f"  https://urlhaus.abuse.ch/browse.php?search={domain}")  # its feeds now want an Auth-Key
    print(f"  https://talosintelligence.com/reputation_center/lookup?search={domain}")
    print(f"  https://www.fortiguard.com/webfilter?q=stream.{domain}")

    counts = {lvl: sum(1 for r in results if r[0] == lvl) for lvl in ("FAIL", "WARN", "UNKNOWN")}
    print(f"\n{counts['FAIL']} listed, {counts['WARN']} warning(s), {counts['UNKNOWN']} unknown")
    return 1 if counts["FAIL"] else 0


if __name__ == "__main__":
    sys.exit(main())
