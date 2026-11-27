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

#include "server/OwnerHandoff.h"

#include "common/Logger.h"
#include "common/OwnerAccount.h"
#include "network/SocketTables.h"
#include "server/RequestGuard.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>

#include <utility>

#ifndef Q_OS_WIN
#include <unistd.h>
#endif

namespace {

/// The largest body a trade is read from: {"n": "<22 characters>"} and room.
constexpr qsizetype kMaxBodyBytes = 256;

QByteArray hashOf(const QString& nonce)
{
    return QCryptographicHash::hash(nonce.toUtf8(), QCryptographicHash::Sha256);
}

/// The nonce a body carries, or nothing: JSON, an object, "n", the exact shape.
QString nonceIn(const QByteArray& body)
{
    if (body.isEmpty() || body.size() > kMaxBodyBytes) return {};
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (!doc.isObject()) return {};
    const QString nonce = doc.object().value(QStringLiteral("n")).toString();
    static const QRegularExpression shape(QStringLiteral("^[A-Za-z0-9_-]{22}$"));
    return shape.match(nonce).hasMatch() ? nonce : QString();
}

/// 127.0.0.1 or ::1, as the socket gave it (IPv4 mapped into IPv6 included).
bool loopbackPeer(const QString& text)
{
    const QHostAddress address(text);
    if (address.isNull()) return false;
    bool v4 = false;
    const quint32 ip = address.toIPv4Address(&v4);
    if (v4) return ip == 0x7F000001u;
    return address == QHostAddress(QHostAddress::LocalHostIPv6);
}

/// What the Host of a request to this page may be: the loopback address the
/// page is opened on, or that of IPv6, with this socket's port. A browser
/// leaves the port out when it is its scheme's own, and only then may it be.
bool hostNamesThisPage(const QString& host, quint16 port, bool tls)
{
    const QString p = QString::number(port);
    QStringList accepted{QStringLiteral("127.0.0.1:") + p, QStringLiteral("[::1]:") + p};
    if ((tls && port == 443) || (!tls && port == 80))
        accepted << QStringLiteral("127.0.0.1") << QStringLiteral("[::1]");
    return accepted.contains(host);
}

/// The 16 bytes of an address, IPv4 mapped into IPv6; empty for no address.
QByteArray asIpv6(const QString& text)
{
    const QHostAddress address(text);
    if (address.isNull()) return {};
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ip = address.toIPv4Address();
        QByteArray out("\0\0\0\0\0\0\0\0\0\0\xff\xff", 12);
        out.append(static_cast<char>((ip >> 24) & 0xFF));
        out.append(static_cast<char>((ip >> 16) & 0xFF));
        out.append(static_cast<char>((ip >> 8) & 0xFF));
        out.append(static_cast<char>(ip & 0xFF));
        return out;
    }
    const Q_IPV6ADDR raw = address.toIPv6Address();
    return QByteArray(reinterpret_cast<const char*>(raw.c), 16);
}

/// What, of a trade's request, refuses it before anyone is asked who holds
/// the connection.
OwnerHandoff::Refusal refusalOf(const HttpRequest& req, bool direct)
{
    using Refusal = OwnerHandoff::Refusal;
    if (req.method != QLatin1String("POST")) return Refusal::Method;
    const QString type =
        req.headers.value(QStringLiteral("content-type")).section(QLatin1Char(';'), 0, 0);
    if (type.trimmed().toLower() != QLatin1String("application/json")) return Refusal::ContentType;
    if (!direct || req.viaTunnel || req.localChannel || req.lanHttp || req.clientPort == 0 ||
        req.serverPort == 0)
        return Refusal::NotDirect;
    if (!loopbackPeer(req.clientAddress)) return Refusal::NotLoopback;
    if (RequestGuard::forwardedByProxy(req.headers)) return Refusal::Proxied;

    const QString host = req.headers.value(QStringLiteral("host"));
    if (req.repeatedHeaders.contains(QStringLiteral("host")) ||
        !hostNamesThisPage(host, req.serverPort, req.serverTls))
        return Refusal::Host;

    // Absent means a program that is not a browser, which the account check
    // below answers for. Present, it is this page's origin and no other: a
    // page elsewhere, a sandboxed frame ("null"), a redirect.
    if (req.repeatedHeaders.contains(QStringLiteral("origin")) ||
        req.repeatedHeaders.contains(QStringLiteral("sec-fetch-site")))
        return Refusal::Origin;
    const QString origin =
        (req.serverTls ? QStringLiteral("https://") : QStringLiteral("http://")) + host;
    if (req.headers.contains(QStringLiteral("origin")) &&
        req.headers.value(QStringLiteral("origin")) != origin)
        return Refusal::Origin;
    if (req.headers.contains(QStringLiteral("sec-fetch-site")) &&
        req.headers.value(QStringLiteral("sec-fetch-site")).trimmed().toLower() !=
            QLatin1String("same-origin"))
        return Refusal::Origin;
    return Refusal::None;
}

} // namespace

QString OwnerHandoff::linkWith(const Target& target, const QString& key)
{
    return target.base + QStringLiteral("#k=") + QString::fromLatin1(QUrl::toPercentEncoding(key)) +
           target.suffix;
}

QString OwnerHandoff::pageUrl(quint16 httpPort, quint16 httpsPort, const QString& nonce)
{
    if (nonce.isEmpty()) return {};
    if (httpPort != 0)
        return QStringLiteral("http://127.0.0.1:%1%2#n=%3")
            .arg(httpPort)
            .arg(QLatin1String(kPagePath), nonce);
    if (httpsPort != 0)
        return QStringLiteral("https://127.0.0.1:%1%2#n=%3")
            .arg(httpsPort)
            .arg(QLatin1String(kPagePath), nonce);
    return {};
}

bool OwnerHandoff::mayOpenFor(const MachineAccount& account)
{
#ifdef Q_OS_WIN
    // same() of a SID with itself: true only for one that reads as a SID.
    return mw::owneraccount::same(account.sid, account.sid) &&
           !mw::owneraccount::isServiceAccount(account.sid);
#else
    return account.uid > 0;
#endif
}

MachineAccount OwnerHandoff::accountOf(const LocalCaller& caller)
{
    MachineAccount account;
#ifdef Q_OS_WIN
    if (caller.readable && !caller.sid.isEmpty()) account.sid = caller.sid;
#else
    if (caller.uid >= 0) account.uid = caller.uid;
#endif
    return account;
}

bool OwnerHandoff::sameAccount(const MachineAccount& a, const MachineAccount& b)
{
#ifdef Q_OS_WIN
    return mw::owneraccount::same(a.sid, b.sid);
#else
    return a.uid >= 0 && a.uid == b.uid;
#endif
}

MachineAccount OwnerHandoff::thisProcess()
{
    MachineAccount account;
#ifdef Q_OS_WIN
    account.sid = mw::owneraccount::accountOfProcess(
        static_cast<quint32>(QCoreApplication::applicationPid()));
#else
    account.uid = static_cast<qint64>(::geteuid());
#endif
    return account;
}

std::optional<MachineAccount> OwnerHandoff::connectionAccount(const HttpRequest& req)
{
    if (req.clientPort == 0 || req.serverPort == 0) return std::nullopt;
    const QByteArray peer = asIpv6(req.clientAddress);
    const QByteArray ours = asIpv6(req.serverAddress);
    if (peer.isEmpty() || ours.isEmpty()) return std::nullopt;
    const std::optional<uint64_t> owner =
        mw::sockettables::connectionOwner({peer, req.clientPort}, {ours, req.serverPort});
    if (!owner) return std::nullopt;
    MachineAccount account;
#ifdef Q_OS_WIN
    // The process that holds it, by its token; one that cannot be opened
    // says nothing.
    account.sid = mw::owneraccount::accountOfProcess(static_cast<quint32>(*owner));
    if (account.sid.isEmpty()) return std::nullopt;
#else
    account.uid = static_cast<qint64>(*owner);
#endif
    return account;
}

OwnerHandoff::OwnerHandoff(std::function<qint64()> nowMs, std::function<QByteArray(int)> random)
    : m_NowMs(std::move(nowMs))
    , m_Random(std::move(random))
{
    if (!m_NowMs) {
        // Since this was made: a clock no change of the system's date moves.
        QElapsedTimer clock;
        clock.start();
        m_NowMs = [clock]() { return clock.elapsed(); };
    }
    if (!m_Random) {
        m_Random = [](int bytes) {
            QByteArray raw(bytes, '\0');
            for (int i = 0; i < raw.size(); ++i)
                raw[i] = static_cast<char>(QRandomGenerator::system()->bounded(256));
            return raw;
        };
    }
}

void OwnerHandoff::dropExpired()
{
    const qint64 now = m_NowMs();
    while (!m_Entries.isEmpty() && now - m_Entries.first().madeAt >= kLifetimeMs)
        m_Entries.removeFirst();
}

QString OwnerHandoff::issue(const Target& target, const MachineAccount& account)
{
    if (!mayOpenFor(account)) return {};
    dropExpired();
    const QByteArray raw = m_Random(kNonceBytes);
    if (raw.size() != kNonceBytes) return {};
    const QString nonce = QString::fromLatin1(
        raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
    while (m_Entries.size() >= kMaxPending)
        m_Entries.removeFirst();
    m_Entries.append({hashOf(nonce), m_NowMs(), target, account});
    return nonce;
}

QString OwnerHandoff::categoryOf(Refusal refusal)
{
    switch (refusal) {
    case Refusal::None: return QStringLiteral("none");
    case Refusal::Body: return QStringLiteral("body");
    case Refusal::Nonce: return QStringLiteral("nonce");
    case Refusal::Method: return QStringLiteral("method");
    case Refusal::ContentType: return QStringLiteral("content-type");
    case Refusal::NotDirect: return QStringLiteral("not-direct");
    case Refusal::NotLoopback: return QStringLiteral("not-loopback");
    case Refusal::Proxied: return QStringLiteral("proxied");
    case Refusal::Host: return QStringLiteral("host");
    case Refusal::Origin: return QStringLiteral("origin");
    case Refusal::Owner: return QStringLiteral("owner");
    }
    return QStringLiteral("unknown");
}

OwnerHandoff::Trade
OwnerHandoff::redeem(const HttpRequest& req, bool direct,
                     const std::function<std::optional<MachineAccount>()>& browserAccount)
{
    Trade out;
    dropExpired();
    const QString nonce = nonceIn(req.body);
    if (nonce.isEmpty()) {
        out.refusal = Refusal::Body;
        return out;
    }

    // Every entry is compared, each in a time that says nothing of where two
    // hashes part.
    const QByteArray given = hashOf(nonce);
    int found = -1;
    for (int i = 0; i < m_Entries.size(); ++i)
        if (RequestGuard::secretMatches(given, m_Entries[i].hash)) found = i;
    if (found < 0) {
        out.refusal = Refusal::Nonce;
        return out;
    }
    // Spent here, whatever becomes of the rest.
    const Entry entry = m_Entries.takeAt(found);

    out.refusal = refusalOf(req, direct);
    if (out.refusal != Refusal::None) return out;

    const std::optional<MachineAccount> holder =
        browserAccount ? browserAccount() : std::optional<MachineAccount>();
    if (!holder || !mayOpenFor(*holder) || !sameAccount(*holder, entry.account)) {
        out.refusal = Refusal::Owner;
        return out;
    }
    out.target = entry.target;
    return out;
}

HttpResponse
OwnerHandoff::answer(const HttpRequest& req, bool direct,
                     const std::function<std::optional<MachineAccount>()>& browserAccount,
                     const std::function<QString()>& mintKey)
{
    const Trade trade = redeem(req, direct, browserAccount);
    const QString key = trade.refusal == Refusal::None && mintKey ? mintKey() : QString();
    HttpResponse resp;
    if (trade.refusal == Refusal::Body || trade.refusal == Refusal::Nonce) {
        // No nonce made here, so none spent: anyone may ask that, as often
        // as they like. Counted, and said at most once a minute.
        ++m_UnknownSince;
        const qint64 now = m_NowMs();
        if (m_UnknownSaidAt < 0 || now - m_UnknownSaidAt >= kUnknownLineMs) {
            Logger::info(QStringLiteral("[OwnerHandoff] %1 request(s) carried no nonce of this "
                                        "machine since the last such line")
                             .arg(m_UnknownSince));
            m_UnknownSaidAt = now;
            m_UnknownSince = 0;
        }
        resp = HttpResponse::json(QJsonObject{{QStringLiteral("error"), QStringLiteral("reopen")}},
                                  403);
    } else if (key.isEmpty()) {
        Logger::warning(
            QStringLiteral("[OwnerHandoff] A link of this machine was refused (%1): it opens "
                           "nothing, and its page says to open it again")
                .arg(trade.refusal == Refusal::None ? QStringLiteral("no key")
                                                    : categoryOf(trade.refusal)));
        resp = HttpResponse::json(QJsonObject{{QStringLiteral("error"), QStringLiteral("reopen")}},
                                  403);
    } else {
        resp =
            HttpResponse::json(QJsonObject{{QStringLiteral("url"), linkWith(trade.target, key)}});
    }
    resp.headers[QStringLiteral("Cache-Control")] = QStringLiteral("no-store");
    return resp;
}

int OwnerHandoff::pending()
{
    dropExpired();
    return static_cast<int>(m_Entries.size());
}
