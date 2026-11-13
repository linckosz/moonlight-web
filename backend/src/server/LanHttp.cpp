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

#include "LanHttp.h"

#include <QHostAddress>
#include <QPair>

namespace LanHttp {

namespace {

// A dotted IPv4 written the one way QHostAddress writes it back: no leading
// zero, no shorthand. Anything QHostAddress would read differently from how a
// person reads it ("010.1.1.1", "192.168.1") is refused rather than guessed.
bool canonicalIpv4(const QString& text, QHostAddress* out)
{
    QHostAddress a;
    if (!a.setAddress(text) || a.protocol() != QAbstractSocket::IPv4Protocol) return false;
    if (a.toString() != text) return false;
    if (out) *out = a;
    return true;
}

// The authority a Host header carries, split. IPv6 literals are not served on
// this path, so a '[' is enough to refuse.
bool splitAuthority(const QString& value, QString* host, QString* port)
{
    if (value.isEmpty() || value.contains(QLatin1Char('[')) || value.contains(QLatin1Char('@')) ||
        value.contains(QLatin1Char('/')))
        return false;
    const int colon = value.indexOf(QLatin1Char(':'));
    if (colon != value.lastIndexOf(QLatin1Char(':'))) return false;
    *host = colon < 0 ? value : value.left(colon);
    *port = colon < 0 ? QString() : value.mid(colon + 1);
    return !host->isEmpty() && (colon < 0 || !port->isEmpty());
}

} // namespace

QString normalizePeer(const QString& address)
{
    QString text = address.trimmed();
    if (text.startsWith(QLatin1String("::ffff:"), Qt::CaseInsensitive)) {
        text = text.mid(7);
        // A dotted tail only: "::ffff:c0a8:010a" is the same address, but no
        // stack hands it to us that way, and reading it would be one more form
        // to get right.
        if (text.contains(QLatin1Char(':'))) return {};
    }
    if (text.contains(QLatin1Char('%'))) return {}; // a zone: never IPv4
    QHostAddress a;
    if (!canonicalIpv4(text, &a)) return {};
    return a.toString();
}

bool isPrivateIpv4(const QString& ipv4)
{
    QHostAddress a;
    if (!canonicalIpv4(ipv4, &a)) return false;
    static const QPair<QHostAddress, int> kRanges[] = {
        QHostAddress::parseSubnet(QStringLiteral("10.0.0.0/8")),
        QHostAddress::parseSubnet(QStringLiteral("172.16.0.0/12")),
        QHostAddress::parseSubnet(QStringLiteral("192.168.0.0/16")),
        QHostAddress::parseSubnet(QStringLiteral("169.254.0.0/16")),
    };
    for (const auto& range : kRanges)
        if (a.isInSubnet(range)) return true;
    return false;
}

QSet<QString> usableClients(const QStringList& entries, const QSet<QString>& ownAddresses,
                            QStringList* dropped)
{
    QSet<QString> own;
    for (const QString& o : ownAddresses) {
        const QString n = normalizePeer(o);
        if (!n.isEmpty()) own.insert(n);
    }
    QSet<QString> usable;
    for (const QString& entry : entries) {
        const QString n = normalizePeer(entry);
        if (n.isEmpty() || !isPrivateIpv4(n) || own.contains(n)) {
            if (dropped) dropped->append(entry);
            continue;
        }
        usable.insert(n);
    }
    return usable;
}

bool admits(const QString& peer, const QSet<QString>& usable, const QSet<QString>& ownAddresses)
{
    const QString n = normalizePeer(peer);
    if (n.isEmpty() || !usable.contains(n) || !isPrivateIpv4(n)) return false;
    for (const QString& o : ownAddresses)
        if (normalizePeer(o) == n) return false;
    return true;
}

QString validatedHost(const QString& hostHeader, const QSet<QString>& ownIpv4,
                      const QString& localName, quint16 httpPort)
{
    if (httpPort == 0) return {};
    const QString value = hostHeader.trimmed().toLower();
    QString host;
    QString port;
    if (!splitAuthority(value, &host, &port)) return {};

    // The port the page was loaded from. A browser leaves out the default one.
    if (port.isEmpty()) {
        if (httpPort != 80) return {};
    } else {
        bool ok = false;
        const uint p = port.toUInt(&ok);
        if (!ok || p != httpPort || port != QString::number(p)) return {};
    }

    if (canonicalIpv4(host, nullptr)) {
        bool ours = false;
        for (const QString& own : ownIpv4)
            if (normalizePeer(own) == host) ours = true;
        if (!ours || !isPrivateIpv4(host)) return {};
    } else {
        const QString name = localName.trimmed().toLower();
        if (name.isEmpty() || !name.endsWith(QLatin1String(".local")) || host != name) return {};
    }
    return httpPort == 80 ? host : host + QLatin1Char(':') + QString::number(httpPort);
}

bool originAllowed(const QString& origin, const QString& validatedHost)
{
    if (validatedHost.isEmpty()) return false;
    const QString o = origin.trimmed().toLower();
    if (o.isEmpty() || o == QLatin1String("null")) return false;
    return o == QStringLiteral("http://") + validatedHost;
}

namespace {

struct Route
{
    const char* method;
    const char* path;
};

// What a TV on plain HTTP needs to show the hosts, start or resume a stream,
// follow it and leave it, and to sign itself in and out. Nothing that reads
// or changes the server, another device, a share, the logs or a credential.
// A route of the server's that is not here answers 404 on this path, however
// it is guarded elsewhere: a later route is refused here until it is named.
const Route kRoutes[] = {
    {"GET", "/api/health"},
    {"GET", "/api/server/hostname"},
    {"GET", "/api/hosts"},
    {"GET", "/api/hosts/*/apps"},
    {"GET", "/api/hosts/*/running-app"},
    {"GET", "/api/hosts/*/stream-state"},
    {"POST", "/api/hosts/*/start"},
    {"POST", "/api/hosts/*/quit"},
    {"GET", "/api/settings/streaming"},
    {"GET", "/api/lan-http/status"},
    {"POST", "/api/lan-http/pair"},
    {"GET", "/api/lan-http/pair/*"},
};

bool matches(const QString& pattern, const QString& path)
{
    const QStringList want = pattern.split(QLatin1Char('/'));
    const QStringList got = path.split(QLatin1Char('/'));
    if (want.size() != got.size()) return false;
    for (int i = 0; i < want.size(); ++i) {
        if (want[i] == QLatin1String("*")) {
            if (got[i].isEmpty()) return false;
            continue;
        }
        if (want[i] != got[i]) return false;
    }
    return true;
}

} // namespace

bool route(const QString& method, const QString& path)
{
    if (path.isEmpty() || !path.startsWith(QLatin1Char('/'))) return false;
    const bool api = path == QLatin1String("/api") || path.startsWith(QLatin1String("/api/"));
    if (!api) return method == QLatin1String("GET") || method == QLatin1String("HEAD");
    for (const Route& r : kRoutes)
        if (method == QLatin1String(r.method) && matches(QLatin1String(r.path), path)) return true;
    return false;
}

int webSocketSlot(const QString& path)
{
    if (path == QLatin1String("/ws")) return 0;
    if (!path.startsWith(QLatin1String("/ws"))) return -1;
    const QString digits = path.mid(3);
    if (digits.isEmpty() || digits.size() > 3 || digits.startsWith(QLatin1Char('0'))) return -1;
    for (const QChar c : digits)
        if (c < QLatin1Char('0') || c > QLatin1Char('9')) return -1;
    return digits.toInt();
}

QString securityPolicy(const QString& validatedHost)
{
    // Built from the validated name only: a Host header is the caller's, and a
    // policy written from it unchecked would let them name where the page may
    // connect.
    const QString connectSrc = validatedHost.isEmpty()
                                   ? QStringLiteral("'self'")
                                   : QStringLiteral("'self' ws://%1").arg(validatedHost);
    return QStringLiteral("default-src 'self'; script-src 'self' 'wasm-unsafe-eval'; "
                          "style-src 'self' 'unsafe-inline'; font-src 'self'; "
                          "img-src 'self' data: blob:; connect-src %1; "
                          "worker-src 'self' blob:; frame-ancestors 'none'; base-uri 'self'; "
                          "form-action 'self'; object-src 'none'")
        .arg(connectSrc);
}

QString cookie(const QString& token, qint64 maxAgeSecs)
{
    return QStringLiteral("%1=%2; HttpOnly; Path=/; SameSite=Strict; Max-Age=%3")
        .arg(QLatin1String(kCookie), token)
        .arg(qMax<qint64>(0, maxAgeSecs));
}

QString clearedCookie()
{
    return QStringLiteral("%1=; HttpOnly; Path=/; SameSite=Strict; Max-Age=0")
        .arg(QLatin1String(kCookie));
}

QString tokenFromCookieHeader(const QString& cookieHeader)
{
    // Exactly one: a page on this origin can write a cookie of the same name
    // (without HttpOnly, or on another path), and the browser then sends both.
    // Which one would be read is not a question worth answering.
    const QString prefix = QLatin1String(kCookie) + QLatin1Char('=');
    QString found;
    int count = 0;
    for (const QString& part : cookieHeader.split(QLatin1Char(';'))) {
        const QString c = part.trimmed();
        if (!c.startsWith(prefix)) continue;
        ++count;
        found = c.mid(prefix.size());
    }
    return count == 1 ? found : QString();
}

QStringList routeTable()
{
    QStringList out;
    for (const Route& r : kRoutes)
        out.append(QLatin1String(r.method) + QLatin1Char(' ') + QLatin1String(r.path));
    return out;
}

bool openRoute(const QString& path)
{
    return path == QLatin1String("/api/health") || path == QLatin1String("/api/lan-http/status") ||
           path == QLatin1String("/api/lan-http/pair") || matches("/api/lan-http/pair/*", path);
}

Gate gate(const QString& method, const QString& path, const QMap<QString, QString>& headers,
          const QString& validatedHost, Key key, bool guardRefused)
{
    Gate g;
    const QString token = tokenFromCookieHeader(headers.value(QStringLiteral("cookie")));
    if (!token.isEmpty()) g.cookieHeader = QLatin1String(kCookie) + QLatin1Char('=') + token;

    // Not a route of the list: as if it were not there, whoever asks.
    if (!route(method, path)) return g;

    // Another page, a body that is not JSON, or a write that does not name
    // this very page as its origin.
    const bool write = method != QLatin1String("GET") && method != QLatin1String("HEAD");
    if (guardRefused ||
        (write && !originAllowed(headers.value(QStringLiteral("origin")), validatedHost))) {
        g.verdict = Verdict::Forbidden;
        g.counted = true;
        return g;
    }

    // A route that wants the key, without a good one. Counted when the key is
    // another address's, and for a write; not for a GET asked with no key or a
    // key that ran out.
    const bool api = path.startsWith(QLatin1String("/api/"));
    if (api && !openRoute(path) && key != Key::Valid) {
        g.verdict = Verdict::Unauthorized;
        g.counted = key == Key::OtherAddress || write;
        return g;
    }

    g.verdict = Verdict::Serve;
    return g;
}

WsGate webSocketGate(const QString& origin, const QString& validatedHost, const QString& path,
                     Key key, const std::function<bool(int slot)>& ownSlot)
{
    WsGate g;
    if (!originAllowed(origin, validatedHost)) {
        g.status = 403;
        g.counted = true;
        return g;
    }
    const int slot = webSocketSlot(path);
    if (slot < 0) {
        g.status = 404;
        return g;
    }
    if (key != Key::Valid) {
        g.status = 401;
        g.counted = true;
        return g;
    }
    if (!ownSlot || !ownSlot(slot)) {
        g.status = 403;
        g.counted = true;
        return g;
    }
    g.slot = slot;
    return g;
}

QString pathForLog(const QString& path)
{
    QStringList out;
    for (const QString& segment : path.split(QLatin1Char('/'))) {
        if (segment.isEmpty()) {
            out.append(segment);
            continue;
        }
        bool plain = segment.size() <= 24;
        for (const QChar c : segment)
            if (!((c >= QLatin1Char('a') && c <= QLatin1Char('z')) || c == QLatin1Char('-')))
                plain = false;
        out.append(plain ? segment : QStringLiteral("*"));
    }
    return out.join(QLatin1Char('/'));
}

} // namespace LanHttp
