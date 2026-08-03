/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "RequestGuard.h"

#include <QHostAddress>
#include <QStringList>
#include <QUrl>

namespace RequestGuard {

namespace {

struct Authority
{
    QString host; // lowercased, IPv6 unbracketed
    QString port; // empty when absent or the scheme's default
};

Authority splitAuthority(const QString& value)
{
    const QString v = value.trimmed();
    if (v.isEmpty()) return {};

    QString host;
    QString port;

    // Accept both a full origin ("https://example.com:443") and a bare
    // authority ("example.com:443", "[::1]:80") — Origin is the former, Host
    // the latter.
    if (v.contains(QLatin1String("://"))) {
        const QUrl url(v);
        host = url.host(); // already unbracketed and lowercased by QUrl
        if (url.port() > 0) port = QString::number(url.port());
    } else if (v.startsWith(QLatin1Char('['))) {
        // IPv6 literal: the port, if any, follows the closing bracket.
        const int close = v.indexOf(QLatin1Char(']'));
        if (close < 0) return {v.toLower(), QString()}; // malformed; compare as-is
        host = v.mid(1, close - 1);
        const QString rest = v.mid(close + 1);
        if (rest.startsWith(QLatin1Char(':'))) port = rest.mid(1);
    } else {
        // A single colon separates host from port. Several means an unbracketed
        // IPv6 literal, which has no port to split off ("fe80::443" is an
        // address, not host "fe80:" on port 443).
        const int colon = v.lastIndexOf(QLatin1Char(':'));
        if (colon >= 0 && v.indexOf(QLatin1Char(':')) == colon) {
            host = v.left(colon);
            port = v.mid(colon + 1);
        } else {
            host = v;
        }
    }

    // Drop the scheme's default port so "example.com" and "example.com:443"
    // compare equal. A TLS-terminating tunnel forwards Host without the port
    // while the browser's Origin carries the implicit 443 (or vice versa), and
    // rejecting that pair would break every tunnelled deployment. Same-host
    // different-port is "same site" anyway — an attacker who owns another port
    // on our own hostname has already won.
    if (port == QLatin1String("80") || port == QLatin1String("443")) port.clear();

    return {host.toLower(), port};
}

} // namespace

QString normalizeAuthority(const QString& value)
{
    const Authority a = splitAuthority(value);
    if (a.host.isEmpty()) return {};
    return a.port.isEmpty() ? a.host : a.host + QLatin1Char(':') + a.port;
}

Initiator classifyInitiator(const QMap<QString, QString>& headers)
{
    // Sec-Fetch-Site is set by the browser itself and is unreachable from page
    // JavaScript, so a malicious page cannot forge it. Every engine that can run
    // a CSRF attack today sends it.
    //   same-origin → our own frontend
    //   none        → user-initiated navigation (address bar, bookmark)
    //   same-site   → a sibling subdomain; not us, treat as hostile
    //   cross-site  → another site entirely
    const QString site = headers.value(QStringLiteral("sec-fetch-site")).trimmed().toLower();
    if (!site.isEmpty()) {
        if (site == QLatin1String("same-origin") || site == QLatin1String("none"))
            return Initiator::SameOrigin;
        return Initiator::CrossSite;
    }

    // Fallback for engines that predate Sec-Fetch-Site: compare Origin (which
    // browsers always attach to cross-origin requests) against the Host we were
    // addressed as.
    const QString origin = headers.value(QStringLiteral("origin")).trimmed();
    if (origin.isEmpty()) return Initiator::Unknown; // curl, scripts, tunnels

    // "null" is what a sandboxed iframe or a cross-origin redirect produces.
    // An attacker can force it deliberately, so it never counts as ours.
    if (origin.compare(QLatin1String("null"), Qt::CaseInsensitive) == 0)
        return Initiator::CrossSite;

    const QString host = normalizeAuthority(headers.value(QStringLiteral("host")));
    if (host.isEmpty()) return Initiator::Unknown;

    return normalizeAuthority(origin) == host ? Initiator::SameOrigin : Initiator::CrossSite;
}

bool isCrossSite(const QMap<QString, QString>& headers)
{
    return classifyInitiator(headers) == Initiator::CrossSite;
}

bool isWebSocketOriginAllowed(const QString& origin, const QString& hostHeader)
{
    // WebSockets are exempt from the same-origin policy: any page may open one
    // to any host, and the handshake carries no CORS negotiation. The Origin
    // header is therefore the only thing standing between a malicious page and
    // the signaling channel (which relays keyboard and mouse events to the
    // host). Browsers always send it; non-browser clients never do.
    if (origin.trimmed().isEmpty()) return true;
    if (origin.compare(QLatin1String("null"), Qt::CaseInsensitive) == 0) return false;

    const QString host = normalizeAuthority(hostHeader);
    if (host.isEmpty()) return true; // no Host to compare against (HTTP/1.0)

    return normalizeAuthority(origin) == host;
}

bool isLocalHostName(const QString& hostHeader)
{
    // Port-insensitive on purpose: what matters is whether the NAME addresses
    // this machine. We routinely answer on a non-default HTTPS port (44729 and
    // friends after a port-parity rebind), and the port says nothing about who
    // the caller thinks they are talking to.
    const QString host = splitAuthority(hostHeader).host;

    // No Host header at all: HTTP/1.0 or a raw socket client. A browser always
    // sends one, so this cannot be a rebinding attack.
    if (host.isEmpty()) return true;

    if (host == QLatin1String("localhost") || host == QLatin1String("127.0.0.1") ||
        host == QLatin1String("::1"))
        return true;

    const QHostAddress addr(host);
    if (!addr.isNull()) {
        // An IP literal: trusted when it is loopback or one of the private
        // ranges we serve the LAN on. A public IP literal is not — nothing in
        // the product addresses us that way, and honouring it would hand the
        // local privilege to whoever reaches the forwarded port.
        if (addr.isLoopback()) return true;
        if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
            const quint32 ip = addr.toIPv4Address();
            if ((ip & 0xFF000000) == 0x0A000000) return true; // 10.0.0.0/8
            if ((ip & 0xFFF00000) == 0xAC100000) return true; // 172.16.0.0/12
            if ((ip & 0xFFFF0000) == 0xC0A80000) return true; // 192.168.0.0/16
        }
        if (addr.isLinkLocal()) return true; // 169.254/16, fe80::/10
        if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
            // Unique local addresses (fc00::/7) are the IPv6 equivalent of
            // RFC 1918 and are what a v6-only LAN hands out.
            const Q_IPV6ADDR raw = addr.toIPv6Address();
            if ((raw[0] & 0xFE) == 0xFC) return true;
        }
        return false;
    }

    // An mDNS name is only resolvable by a machine on the same link, and cannot
    // be registered by an outsider.
    return host.endsWith(QLatin1String(".local"));
}

bool isTrustedHost(const QString& hostHeader, const QString& publicDomain)
{
    if (isLocalHostName(hostHeader)) return true;

    // Our own domain also speaks for this machine — the host reaches itself
    // that way through hairpin NAT, and every remote client arrives under it.
    // This is what stops DNS rebinding: attacker.example re-pointed at
    // 127.0.0.1 still carries its own name in Host and is refused.
    const QString host = splitAuthority(hostHeader).host;
    const QString domain = splitAuthority(publicDomain).host;
    return !domain.isEmpty() && host == domain;
}

bool isBodyContentTypeAllowed(const QString& contentType, qsizetype bodySize)
{
    if (bodySize <= 0) return true; // nothing to parse

    // Every API handler parses the body as JSON, so requiring the matching
    // content type costs us nothing — and it is a CSRF barrier in its own
    // right: application/json is not a CORS-safelisted value, so a cross-origin
    // fetch carrying it must pass a preflight, which we never answer.
    //
    // An absent content type is rejected too: `fetch(url, {method: 'POST',
    // body: new Blob([json])})` sends exactly that, with no preflight.
    const QString type = contentType.section(QLatin1Char(';'), 0, 0).trimmed().toLower();
    return type == QLatin1String("application/json");
}

Request describe(const HttpRequest& req)
{
    Request out;
    out.method = req.method;
    out.path = req.path;
    out.headers = req.headers;
    out.bodySize = req.body.size();
    return out;
}

int blockStatus(Outcome outcome)
{
    switch (outcome) {
    case Outcome::BlockCrossSite: return 403;
    case Outcome::BlockContentType: return 415;
    case Outcome::Allow: break;
    }
    return 0;
}

QString blockError(Outcome outcome)
{
    switch (outcome) {
    case Outcome::BlockCrossSite: return QStringLiteral("cross_site_request_blocked");
    case Outcome::BlockContentType: return QStringLiteral("unsupported_media_type");
    case Outcome::Allow: break;
    }
    return {};
}

Decision evaluate(const Request& req, const Context& ctx)
{
    Decision d;

    const bool isApi = req.path.startsWith(QLatin1String("/api/"));

    // Only the API grants privileges or changes state; static files are public
    // by design and must keep loading from anywhere (a bookmarked page, an
    // iframe on the user's own dashboard).
    if (isApi && isCrossSite(req.headers)) {
        d.outcome = Outcome::BlockCrossSite;
        return d;
    }
    if (isApi && !isBodyContentTypeAllowed(req.headers.value(QStringLiteral("content-type")),
                                           req.bodySize)) {
        d.outcome = Outcome::BlockContentType;
        return d;
    }
    d.outcome = Outcome::Allow;

    // The Host must be a name we own. This is the one check a rebinding attack
    // cannot dodge: it makes the attacker's page same-origin with us (so the
    // cross-site test above passes, and the attacker can READ our responses),
    // but the browser still sends the attacker's own name in Host.
    const QString hostHeader = req.headers.value(QStringLiteral("host"));
    d.hostTrusted = isTrustedHost(hostHeader, ctx.publicDomain);
    d.hostUntrusted = ctx.peerLocal && !d.hostTrusted;

    // Being reached from this machine is only worth something when the name we
    // were reached under could not have come from outside. Under the public
    // domain it could: a TLS-terminating tunnel runs here and forwards every
    // visitor from loopback, so the pair (local peer, our domain) describes the
    // whole internet just as well as it describes the host.
    d.localPrivilege = ctx.peerLocal && isLocalHostName(hostHeader);

    // Two ways to be the host machine: reach us from the machine itself, or
    // prove it with the host key over the public domain.
    d.hostMachine = d.localPrivilege || ctx.hostSession;

    // Admin writes need the admin key on top of the address check. Reads do
    // not: the admin page has to load and fetch that key in the first place.
    const bool mutating = req.method != QLatin1String("GET") && req.method != QLatin1String("HEAD");
    // A LAN machine that unlocked with the remote admin password gets the same
    // privileges as the host. Note that its *unlock* is gated separately (LAN
    // peer, trusted Host, rate limit) — by the time adminSession is set, that
    // check has already passed.
    d.adminPrivilege = (d.hostMachine || ctx.adminSession) && (!mutating || ctx.adminKeyOk);

    return d;
}

AdminTokenReply adminTokenReply(const Decision& decision, const Context& ctx, bool authenticated)
{
    if (decision.hostMachine || ctx.adminSession) return AdminTokenReply::Grant;
    if (authenticated) return AdminTokenReply::Empty;
    return AdminTokenReply::Deny;
}

} // namespace RequestGuard
