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

// The page a link of this machine opens first (server/OwnerHandoff.h): a nonce
// made for one target and one account, good once and for a minute, traded on
// the loopback connection its page was served on, and only when that
// connection is held by the nonce's account (network/SocketTables.h). Also the
// last lock in front of a browser (common/BrowserLink.h).

#include "common/BrowserLink.h"
#include "common/Logger.h"
#include "common/OwnerAccount.h"
#include "network/SocketTables.h"
#include "server/LocalChannel.h"
#include "server/OwnerHandoff.h"
#include "test_framework.h"

#include <QCoreApplication>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QStringList>
#include <QSysInfo>
#include <QTcpServer>
#include <QTcpSocket>

#include <functional>
#include <memory>
#include <optional>
#include <utility>

namespace {

using Refusal = OwnerHandoff::Refusal;
using mw::sockettables::TcpEnd;
using mw::sockettables::TcpSocket;

/// Two people of this machine, and the system, as the platform names them.
MachineAccount personA()
{
    MachineAccount a;
#ifdef Q_OS_WIN
    a.sid = QStringLiteral("S-1-5-21-1004336348-1177238915-682003330-1001");
#else
    a.uid = 1000;
#endif
    return a;
}

MachineAccount personB()
{
    MachineAccount b;
#ifdef Q_OS_WIN
    b.sid = QStringLiteral("S-1-5-21-1004336348-1177238915-682003330-1002");
#else
    b.uid = 1001;
#endif
    return b;
}

MachineAccount theSystem()
{
    MachineAccount s;
#ifdef Q_OS_WIN
    s.sid = QStringLiteral("S-1-5-18");
#else
    s.uid = 0;
#endif
    return s;
}

const OwnerHandoff::Target kTarget{QStringLiteral("https://localhost:48443/admin"), QString()};

/// A clock the test moves by hand.
struct Clock
{
    std::shared_ptr<qint64> now = std::make_shared<qint64>(0);
    std::function<qint64()> fn() const
    {
        auto at = now;
        return [at]() { return *at; };
    }
};

/// The page's own request on plain HTTP port 48080, as a browser sends it.
HttpRequest pageRequest(const QString& nonce)
{
    HttpRequest req;
    req.method = QStringLiteral("POST");
    req.path = QLatin1String(OwnerHandoff::kTradePath);
    req.headers.insert(QStringLiteral("content-type"), QStringLiteral("application/json"));
    req.headers.insert(QStringLiteral("host"), QStringLiteral("127.0.0.1:48080"));
    req.headers.insert(QStringLiteral("origin"), QStringLiteral("http://127.0.0.1:48080"));
    req.headers.insert(QStringLiteral("sec-fetch-site"), QStringLiteral("same-origin"));
    req.body =
        QJsonDocument(QJsonObject{{QStringLiteral("n"), nonce}}).toJson(QJsonDocument::Compact);
    req.clientAddress = QStringLiteral("127.0.0.1");
    req.clientPort = 51000;
    req.serverAddress = QStringLiteral("127.0.0.1");
    req.serverPort = 48080;
    req.serverTls = false;
    return req;
}

/// Who holds the connection, as the tables would say, and how often it was asked.
struct Holder
{
    std::optional<MachineAccount> account;
    int asked = 0;
    std::function<std::optional<MachineAccount>()> fn()
    {
        return [this]() {
            ++asked;
            return account;
        };
    }
};

/// The lines the server's log is given while one of these lives.
struct LogTap
{
    QStringList warnings;
    QStringList all;
    LogTap()
    {
        Logger::instance()->setTap([this](Logger::Level level, const QString& line) {
            all << line;
            if (level == Logger::Warning) warnings << line;
        });
    }
    ~LogTap() { Logger::instance()->setTap({}); }
};

/// The 16 bytes of an address, IPv4 mapped into IPv6.
QByteArray v6(const char* text)
{
    const QHostAddress address(QString::fromLatin1(text));
    if (address.protocol() == QAbstractSocket::IPv4Protocol) {
        const quint32 ip = address.toIPv4Address();
        QByteArray out("\0\0\0\0\0\0\0\0\0\0\xff\xff", 12);
        for (int shift = 24; shift >= 0; shift -= 8)
            out.append(static_cast<char>((ip >> shift) & 0xFF));
        return out;
    }
    const Q_IPV6ADDR raw = address.toIPv6Address();
    return QByteArray(reinterpret_cast<const char*>(raw.c), 16);
}

const QByteArray kProcHeader =
    "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  "
    "timeout inode\n";

/// One row of /proc/net/tcp(6): two ends as the kernel prints them, a state, a uid.
QByteArray procRow(int sl, const char* local, const char* remote, const char* state, int uid)
{
    return QByteArray("  ") + QByteArray::number(sl) + ": " + local + " " + remote + " " + state +
           " 00000000:00000000 00:00000000 00000000  " + QByteArray::number(uid) +
           "        0 4242 1 0000000000000000 20 4 30 10 -1\n";
}

// The ends of one connection: the browser's (127.0.0.1:51000) and ours
// (127.0.0.1:48080), as /proc prints them on a little-endian machine.
const char* const kBrowserV4 = "0100007F:C738";
const char* const kOursV4 = "0100007F:BBD0";
const char* const kBrowserMapped = "0000000000000000FFFF00000100007F:C738";
const char* const kOursMapped = "0000000000000000FFFF00000100007F:BBD0";
const char* const kBrowserV6 = "00000000000000000000000001000000:C738";
const char* const kOursV6 = "00000000000000000000000001000000:BBD0";

} // namespace

void run_owner_handoff_tests()
{
    SECTION("Owner handoff — a nonce is 128 random bits, in the text a link carries");
    {
        OwnerHandoff handoff;
        const QString nonce = handoff.issue(kTarget, personA());
        CHECK_EQ(static_cast<int>(nonce.size()), 22);
        const QByteArray raw =
            QByteArray::fromBase64(nonce.toLatin1(), QByteArray::Base64UrlEncoding);
        CHECK_EQ(static_cast<int>(raw.size()), 16);
        for (const QChar c : nonce)
            CHECK(c.isLetterOrNumber() || c == QLatin1Char('-') || c == QLatin1Char('_'));
        QSet<QString> seen{nonce};
        for (int i = 0; i < 50; ++i) {
            const QString next = handoff.issue(kTarget, personA());
            CHECK(!seen.contains(next));
            seen.insert(next);
        }
    }

    SECTION("Owner handoff — made for a person's account, never root nor the system's own");
    {
        CHECK(OwnerHandoff::mayOpenFor(personA()));
        CHECK(!OwnerHandoff::mayOpenFor(theSystem()));
        CHECK(!OwnerHandoff::mayOpenFor(MachineAccount{}));
#ifdef Q_OS_WIN
        for (const char* sid : {"S-1-5-19", "S-1-5-20", "not a sid", ""}) {
            MachineAccount other;
            other.sid = QString::fromLatin1(sid);
            CHECK(!OwnerHandoff::mayOpenFor(other));
        }
#else
        MachineAccount nobody;
        nobody.uid = -5;
        CHECK(!OwnerHandoff::mayOpenFor(nobody));
#endif
        CHECK(OwnerHandoff::sameAccount(personA(), personA()));
        CHECK(!OwnerHandoff::sameAccount(personA(), personB()));
        CHECK(!OwnerHandoff::sameAccount(MachineAccount{}, MachineAccount{}));
        CHECK(OwnerHandoff::sameAccount(OwnerHandoff::thisProcess(), OwnerHandoff::thisProcess()));

        OwnerHandoff handoff;
        CHECK(handoff.issue(kTarget, theSystem()).isEmpty());
        CHECK(handoff.issue(kTarget, MachineAccount{}).isEmpty());
        CHECK_EQ(handoff.pending(), 0);
    }

    SECTION("Owner handoff — a nonce is traded once, for the target it was made for");
    {
        OwnerHandoff handoff;
        const OwnerHandoff::Target target{QStringLiteral("https://abc.moonlightweb.top/x"),
                                          QStringLiteral("&p=/admin")};
        const QString nonce = handoff.issue(target, personA());
        Holder holder{personA()};
        const OwnerHandoff::Trade first = handoff.redeem(pageRequest(nonce), true, holder.fn());
        CHECK(first.refusal == Refusal::None);
        CHECK(first.target.base == target.base);
        CHECK(first.target.suffix == target.suffix);
        CHECK_EQ(holder.asked, 1);
        CHECK_EQ(handoff.pending(), 0);
        CHECK(handoff.redeem(pageRequest(nonce), true, holder.fn()).refusal == Refusal::Nonce);
        // One of another handoff, as one from before a restart would be.
        OwnerHandoff other;
        CHECK(handoff.redeem(pageRequest(other.issue(target, personA())), true, holder.fn())
                  .refusal == Refusal::Nonce);
    }

    SECTION("Owner handoff — a minute, then nothing");
    {
        Clock clock;
        OwnerHandoff handoff(clock.fn());
        const QString early = handoff.issue(kTarget, personA());
        const QString late = handoff.issue(kTarget, personA());
        Holder holder{personA()};
        *clock.now = OwnerHandoff::kLifetimeMs - 1;
        CHECK(handoff.redeem(pageRequest(early), true, holder.fn()).refusal == Refusal::None);
        *clock.now = OwnerHandoff::kLifetimeMs;
        CHECK(handoff.redeem(pageRequest(late), true, holder.fn()).refusal == Refusal::Nonce);
        CHECK_EQ(handoff.pending(), 0);
    }

    SECTION("Owner handoff — eight wait at most; a ninth takes the oldest's place");
    {
        OwnerHandoff handoff;
        QStringList nonces;
        for (int i = 0; i <= OwnerHandoff::kMaxPending; ++i)
            nonces << handoff.issue(kTarget, personA());
        CHECK_EQ(handoff.pending(), OwnerHandoff::kMaxPending);
        Holder holder{personA()};
        CHECK(handoff.redeem(pageRequest(nonces.first()), true, holder.fn()).refusal ==
              Refusal::Nonce);
        for (int i = 1; i < nonces.size(); ++i)
            CHECK(handoff.redeem(pageRequest(nonces.at(i)), true, holder.fn()).refusal ==
                  Refusal::None);
    }

    SECTION("Owner handoff — a connection held by another account is refused, and spends the "
            "nonce");
    {
        OwnerHandoff handoff;
        const QString nonce = handoff.issue(kTarget, personA());
        Holder other{personB()};
        CHECK(handoff.redeem(pageRequest(nonce), true, other.fn()).refusal == Refusal::Owner);
        // The owner, after: that nonce is gone; the next one opens.
        Holder owner{personA()};
        CHECK(handoff.redeem(pageRequest(nonce), true, owner.fn()).refusal == Refusal::Nonce);
        const QString again = handoff.issue(kTarget, personA());
        CHECK(handoff.redeem(pageRequest(again), true, owner.fn()).refusal == Refusal::None);

        // Nobody the tables can name, and the system, hold no one's connection.
        for (const std::optional<MachineAccount>& who :
             {std::optional<MachineAccount>(), std::optional<MachineAccount>(theSystem()),
              std::optional<MachineAccount>(MachineAccount{})}) {
            const QString n = handoff.issue(kTarget, personA());
            Holder h{who};
            CHECK(handoff.redeem(pageRequest(n), true, h.fn()).refusal == Refusal::Owner);
            CHECK(handoff.redeem(pageRequest(n), true, owner.fn()).refusal == Refusal::Nonce);
        }
    }

    SECTION("Owner handoff — each refusal of the request spends the nonce, before anyone is "
            "asked who holds the connection");
    {
        struct Case
        {
            const char* what;
            std::function<void(HttpRequest&, bool&)> spoil;
            Refusal expected;
        };
        const QList<Case> cases = {
            {"a GET", [](HttpRequest& r, bool&) { r.method = QStringLiteral("GET"); },
             Refusal::Method},
            {"text, not JSON",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("content-type")] = QStringLiteral("text/plain");
             },
             Refusal::ContentType},
            {"no content type",
             [](HttpRequest& r, bool&) { r.headers.remove(QStringLiteral("content-type")); },
             Refusal::ContentType},
            {"not on a socket", [](HttpRequest&, bool& direct) { direct = false; },
             Refusal::NotDirect},
            {"through the tunnel", [](HttpRequest& r, bool&) { r.viaTunnel = true; },
             Refusal::NotDirect},
            {"over the local channel", [](HttpRequest& r, bool&) { r.localChannel = true; },
             Refusal::NotDirect},
            {"a TV on plain HTTP", [](HttpRequest& r, bool&) { r.lanHttp = true; },
             Refusal::NotDirect},
            {"no port", [](HttpRequest& r, bool&) { r.clientPort = 0; }, Refusal::NotDirect},
            {"from the LAN",
             [](HttpRequest& r, bool&) { r.clientAddress = QStringLiteral("192.168.1.9"); },
             Refusal::NotLoopback},
            {"another loopback address",
             [](HttpRequest& r, bool&) { r.clientAddress = QStringLiteral("127.0.0.2"); },
             Refusal::NotLoopback},
            {"a label, not an address",
             [](HttpRequest& r, bool&) { r.clientAddress = QStringLiteral("local-channel"); },
             Refusal::NotLoopback},
            {"through a proxy",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("x-forwarded-for")] = QStringLiteral("203.0.113.7");
             },
             Refusal::Proxied},
            {"Host localhost",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("host")] = QStringLiteral("localhost:48080");
                 r.headers[QStringLiteral("origin")] = QStringLiteral("http://localhost:48080");
             },
             Refusal::Host},
            {"Host of another port",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("host")] = QStringLiteral("127.0.0.1:48081");
             },
             Refusal::Host},
            {"Host without the port, on a port that is not its scheme's",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("host")] = QStringLiteral("127.0.0.1");
                 r.headers[QStringLiteral("origin")] = QStringLiteral("http://127.0.0.1");
             },
             Refusal::Host},
            {"Host of a rebinding name",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("host")] = QStringLiteral("evil.example:48080");
                 r.headers[QStringLiteral("origin")] = QStringLiteral("http://evil.example:48080");
             },
             Refusal::Host},
            {"no Host", [](HttpRequest& r, bool&) { r.headers.remove(QStringLiteral("host")); },
             Refusal::Host},
            {"Host twice",
             [](HttpRequest& r, bool&) { r.repeatedHeaders.insert(QStringLiteral("host")); },
             Refusal::Host},
            {"another site's Origin",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("origin")] = QStringLiteral("https://evil.example");
             },
             Refusal::Origin},
            {"Origin null",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("origin")] = QStringLiteral("null");
             },
             Refusal::Origin},
            {"Origin of the other scheme",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("origin")] = QStringLiteral("https://127.0.0.1:48080");
             },
             Refusal::Origin},
            {"Origin twice",
             [](HttpRequest& r, bool&) { r.repeatedHeaders.insert(QStringLiteral("origin")); },
             Refusal::Origin},
            {"cross-site",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("sec-fetch-site")] = QStringLiteral("cross-site");
             },
             Refusal::Origin},
            {"same-site",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("sec-fetch-site")] = QStringLiteral("same-site");
             },
             Refusal::Origin},
            {"a navigation, not the page's own request",
             [](HttpRequest& r, bool&) {
                 r.headers[QStringLiteral("sec-fetch-site")] = QStringLiteral("none");
             },
             Refusal::Origin},
        };
        for (const Case& c : cases) {
            OwnerHandoff handoff;
            const QString nonce = handoff.issue(kTarget, personA());
            HttpRequest req = pageRequest(nonce);
            bool direct = true;
            c.spoil(req, direct);
            Holder holder{personA()};
            const Refusal got = handoff.redeem(req, direct, holder.fn()).refusal;
            if (got != c.expected)
                std::fprintf(stderr, "    %s: refused as %s\n", c.what,
                             qPrintable(OwnerHandoff::categoryOf(got)));
            CHECK(got == c.expected);
            CHECK_EQ(holder.asked, 0);
            // Spent by the refusal: the page's own request finds nothing now.
            CHECK(handoff.redeem(pageRequest(nonce), true, holder.fn()).refusal == Refusal::Nonce);
        }
    }

    SECTION("Owner handoff — what a browser sends on each listener is accepted");
    {
        struct Case
        {
            const char* what;
            bool tls;
            quint16 port;
            const char* host;
            const char* origin; // nullptr: no Origin, no Sec-Fetch-Site
            const char* peer;
        };
        const QList<Case> cases = {
            {"plain, its port", false, 48080, "127.0.0.1:48080", "http://127.0.0.1:48080",
             "127.0.0.1"},
            {"plain on 80, the port left out", false, 80, "127.0.0.1", "http://127.0.0.1",
             "127.0.0.1"},
            {"plain on 80, the port written", false, 80, "127.0.0.1:80", "http://127.0.0.1:80",
             "127.0.0.1"},
            {"HTTPS, its port", true, 48443, "127.0.0.1:48443", "https://127.0.0.1:48443",
             "127.0.0.1"},
            {"HTTPS on 443, the port left out", true, 443, "127.0.0.1", "https://127.0.0.1",
             "127.0.0.1"},
            {"IPv6 loopback", false, 48080, "[::1]:48080", "http://[::1]:48080", "::1"},
            {"IPv4 mapped into IPv6 by a dual-stack socket", false, 48080, "127.0.0.1:48080",
             "http://127.0.0.1:48080", "::ffff:127.0.0.1"},
            {"a program, not a browser: no Origin", false, 48080, "127.0.0.1:48080", nullptr,
             "127.0.0.1"},
        };
        for (const Case& c : cases) {
            OwnerHandoff handoff;
            HttpRequest req = pageRequest(handoff.issue(kTarget, personA()));
            req.serverTls = c.tls;
            req.serverPort = c.port;
            req.clientAddress = QString::fromLatin1(c.peer);
            req.headers[QStringLiteral("host")] = QString::fromLatin1(c.host);
            if (c.origin) {
                req.headers[QStringLiteral("origin")] = QString::fromLatin1(c.origin);
            } else {
                req.headers.remove(QStringLiteral("origin"));
                req.headers.remove(QStringLiteral("sec-fetch-site"));
            }
            Holder holder{personA()};
            const Refusal got = handoff.redeem(req, true, holder.fn()).refusal;
            if (got != Refusal::None)
                std::fprintf(stderr, "    %s: refused as %s\n", c.what,
                             qPrintable(OwnerHandoff::categoryOf(got)));
            CHECK(got == Refusal::None);
        }

        // The Host left out on a port that is the other scheme's is not.
        for (const bool tls : {false, true}) {
            OwnerHandoff handoff;
            HttpRequest req = pageRequest(handoff.issue(kTarget, personA()));
            req.serverTls = tls;
            req.serverPort = tls ? 80 : 443;
            req.headers[QStringLiteral("host")] = QStringLiteral("127.0.0.1");
            req.headers[QStringLiteral("origin")] =
                tls ? QStringLiteral("https://127.0.0.1") : QStringLiteral("http://127.0.0.1");
            Holder holder{personA()};
            CHECK(handoff.redeem(req, true, holder.fn()).refusal == Refusal::Host);
        }
    }

    SECTION("Owner handoff — a body without a nonce spends none");
    {
        OwnerHandoff handoff;
        const QString nonce = handoff.issue(kTarget, personA());
        Holder holder{personA()};
        const QList<QByteArray> bodies = {
            QByteArray(),
            "not json",
            "[\"" + nonce.toLatin1() + "\"]",
            "{\"k\":\"" + nonce.toLatin1() + "\"}",
            "{\"n\":\"" + nonce.toLatin1().left(21) + "\"}",
            "{\"n\":\"" + nonce.toLatin1() + "A\"}",
            "{\"n\":\"" + nonce.toLatin1().left(21) + "+\"}",
            "{\"n\":\"" + nonce.toLatin1() + "\",\"pad\":\"" + QByteArray(300, 'x') + "\"}",
        };
        for (const QByteArray& body : bodies) {
            HttpRequest req = pageRequest(nonce);
            req.body = body;
            CHECK(handoff.redeem(req, true, holder.fn()).refusal == Refusal::Body);
        }
        CHECK_EQ(handoff.pending(), 1);
        CHECK(handoff.redeem(pageRequest(nonce), true, holder.fn()).refusal == Refusal::None);
    }

    SECTION("Owner handoff — the answer: the link with a key made then, or 403 and one "
            "warning that names the category alone");
    {
        OwnerHandoff handoff;
        const OwnerHandoff::Target target{QStringLiteral("https://localhost:48443/admin"),
                                          QString()};
        int minted = 0;
        const QString key = QStringLiteral("KEYkeyKEYkeyKEYkeyKEYkeyKEYkeyKEYkeyKEYkey1");
        auto mint = [&minted, &key]() {
            ++minted;
            return key;
        };

        // Refused: another account holds the connection.
        const QString othersNonce = handoff.issue(target, personA());
        {
            LogTap tap;
            Holder other{personB()};
            const HttpResponse refused =
                handoff.answer(pageRequest(othersNonce), true, other.fn(), mint);
            CHECK_EQ(refused.statusCode, 403);
            CHECK(refused.headers.value(QStringLiteral("Cache-Control")) ==
                  QStringLiteral("no-store"));
            CHECK(QJsonDocument::fromJson(refused.body).object().value("error").toString() ==
                  QStringLiteral("reopen"));
            CHECK(!refused.body.contains(key.toLatin1()));
            CHECK_EQ(minted, 0);
            CHECK_EQ(static_cast<int>(tap.warnings.size()), 1);
            CHECK_EQ(static_cast<int>(tap.all.size()), 1);
            const QString line = tap.warnings.value(0);
            CHECK(line.contains(QStringLiteral("(owner)")));
            CHECK(!line.contains(othersNonce));
            CHECK(!line.contains(key));
#ifdef Q_OS_WIN
            CHECK(!line.contains(personA().sid));
            CHECK(!line.contains(personB().sid));
#else
            CHECK(!line.contains(QStringLiteral("1000")));
            CHECK(!line.contains(QStringLiteral("1001")));
#endif
        }

        // Traded: the owner's link, with the key, said to nobody but the page.
        const QString nonce = handoff.issue(target, personA());
        {
            LogTap tap;
            Holder owner{personA()};
            const HttpResponse traded = handoff.answer(pageRequest(nonce), true, owner.fn(), mint);
            CHECK_EQ(traded.statusCode, 200);
            CHECK(traded.headers.value(QStringLiteral("Cache-Control")) ==
                  QStringLiteral("no-store"));
            const QString url =
                QJsonDocument::fromJson(traded.body).object().value("url").toString();
            CHECK(url == OwnerHandoff::linkWith(target, key));
            CHECK(url.startsWith(QStringLiteral("https://localhost:48443/admin#k=")));
            CHECK_EQ(minted, 1);
            CHECK_EQ(static_cast<int>(tap.all.size()), 0);
        }
    }

    SECTION("Owner handoff — a request with no nonce made here is counted, not written: one "
            "line a minute at most, and no warning");
    {
        Clock clock;
        OwnerHandoff handoff(clock.fn());
        int minted = 0;
        auto mint = [&minted]() {
            ++minted;
            return QStringLiteral("KEY");
        };
        Holder owner{personA()};
        QString real;
        {
            LogTap tap;
            for (int i = 0; i < 100; ++i) {
                HttpRequest req =
                    pageRequest(QStringLiteral("Unknown%1").arg(i, 15, 10, QLatin1Char('0')));
                req.clientAddress = QStringLiteral("203.0.113.7");
                const HttpResponse resp = handoff.answer(req, true, owner.fn(), mint);
                CHECK_EQ(resp.statusCode, 403);
                CHECK(QJsonDocument::fromJson(resp.body).object().value("error").toString() ==
                      QStringLiteral("reopen"));
            }
            // A real one waits meanwhile; a body without it spends nothing.
            *clock.now = 30 * 1000;
            real = handoff.issue(kTarget, personA());
            for (int i = 0; i < 20; ++i) {
                HttpRequest req = pageRequest(real);
                req.body = QByteArray("not json");
                CHECK_EQ(handoff.answer(req, true, owner.fn(), mint).statusCode, 403);
            }
            CHECK(tap.all.size() <= 1);
            CHECK_EQ(static_cast<int>(tap.warnings.size()), 0);
            for (const QString& line : std::as_const(tap.all))
                CHECK(!line.contains(QStringLiteral("203.0.113.7")));
        }
        // A minute on, the next one says how many were counted.
        *clock.now = 60 * 1000;
        {
            LogTap tap;
            HttpRequest req = pageRequest(QStringLiteral("AnotherUnknownNonce000"));
            CHECK_EQ(handoff.answer(req, true, owner.fn(), mint).statusCode, 403);
            CHECK_EQ(static_cast<int>(tap.all.size()), 1);
            CHECK_EQ(static_cast<int>(tap.warnings.size()), 0);
            CHECK(tap.all.value(0).contains(QStringLiteral("120 request(s)")));
        }
        // None of it cost the real nonce anything, nor made a key.
        CHECK_EQ(owner.asked, 0);
        CHECK_EQ(minted, 0);
        CHECK_EQ(handoff.pending(), 1);
        CHECK_EQ(handoff.answer(pageRequest(real), true, owner.fn(), mint).statusCode, 200);
    }

    SECTION("Owner handoff — the link is the one the tray always opened; the page's address "
            "carries no key");
    {
        CHECK(OwnerHandoff::linkWith({QStringLiteral("https://localhost:8443/admin"), QString()},
                                     QStringLiteral("a+b/c")) ==
              QStringLiteral("https://localhost:8443/admin#k=a%2Bb%2Fc"));
        CHECK(OwnerHandoff::linkWith(
                  {QStringLiteral("https://abc.moonlightweb.top/"), QStringLiteral("&p=/admin")},
                  QStringLiteral("KEY")) ==
              QStringLiteral("https://abc.moonlightweb.top/#k=KEY&p=/admin"));

        const QString nonce = QStringLiteral("AbCdEfGhIjKlMnOpQrStUv");
        const QString page = OwnerHandoff::pageUrl(48080, 48443, nonce);
        CHECK(page == QStringLiteral("http://127.0.0.1:48080/owner-handoff#n=") + nonce);
        CHECK(OwnerHandoff::pageUrl(0, 48443, nonce) ==
              QStringLiteral("https://127.0.0.1:48443/owner-handoff#n=") + nonce);
        CHECK(OwnerHandoff::pageUrl(80, 443, nonce) ==
              QStringLiteral("http://127.0.0.1:80/owner-handoff#n=") + nonce);
        CHECK(OwnerHandoff::pageUrl(0, 0, nonce).isEmpty());
        CHECK(OwnerHandoff::pageUrl(48080, 48443, QString()).isEmpty());
        CHECK(!mw::linkCarriesKey(page));
        CHECK(!page.contains(QStringLiteral("k=")));
    }

    SECTION("Browser link — a link that carries a key is not given to a browser, with one "
            "warning that does not repeat it");
    {
        CHECK(mw::linkCarriesKey(QStringLiteral("https://localhost/admin#k=SECRET")));
        CHECK(mw::linkCarriesKey(QStringLiteral("https://abc.moonlightweb.top/#a=1&k=SECRET")));
        CHECK(mw::linkCarriesKey(QStringLiteral("https://abc.moonlightweb.top/#k=SECRET&p=/x")));
        CHECK(!mw::linkCarriesKey(QStringLiteral("https://localhost/admin")));
        CHECK(!mw::linkCarriesKey(QStringLiteral("https://localhost/admin?k=1")));
        CHECK(!mw::linkCarriesKey(QStringLiteral("https://localhost/#kk=1&n=2")));
        CHECK(!mw::linkCarriesKey(QStringLiteral("http://127.0.0.1:48080/owner-handoff#n=x")));
#if !defined(Q_OS_MACOS)
        {
            LogTap tap;
            CHECK(!mw::mayGiveBrowser(QStringLiteral("https://localhost:48443/admin#k=SECRET")));
            CHECK_EQ(static_cast<int>(tap.warnings.size()), 1);
            const QString line = tap.warnings.value(0);
            CHECK(!line.contains(QStringLiteral("SECRET")));
            CHECK(!line.contains(QStringLiteral("localhost")));
        }
        {
            LogTap tap;
            CHECK(mw::mayGiveBrowser(QStringLiteral("http://127.0.0.1:48080/owner-handoff#n=x")));
            CHECK(mw::mayGiveBrowser(QStringLiteral("https://localhost:48443/admin")));
            CHECK_EQ(static_cast<int>(tap.all.size()), 0);
        }
#endif
    }

    SECTION("Socket tables — who holds the far end of a connection, from /proc's own text");
    if (QSysInfo::ByteOrder == QSysInfo::LittleEndian) {
        using mw::sockettables::connectionOwnerInProc;
        const TcpEnd browser{v6("127.0.0.1"), 51000};
        const TcpEnd ours{v6("127.0.0.1"), 48080};

        // IPv4: the browser's socket, ours, our listener, and a closed one.
        const QByteArray tcp = kProcHeader +
                               procRow(0, "00000000:BBD0", "00000000:0000", "0A", 1000) +
                               procRow(1, kBrowserV4, kOursV4, "01", 1001) +
                               procRow(2, kOursV4, kBrowserV4, "01", 1000) +
                               procRow(3, "0100007F:C739", kOursV4, "06", 0);
        CHECK(connectionOwnerInProc(tcp, QByteArray(), browser, ours) ==
              std::optional<uint64_t>(1001));
        // Our own socket is the reverse, and answers for our end only.
        CHECK(connectionOwnerInProc(tcp, QByteArray(), ours, browser) ==
              std::optional<uint64_t>(1000));
        // One end alone is no answer: another port, another address.
        CHECK(!connectionOwnerInProc(tcp, QByteArray(), {browser.address, 51001}, ours));
        CHECK(!connectionOwnerInProc(tcp, QByteArray(), browser, {v6("127.0.0.2"), 48080}));
        // A socket that is not established answers for nothing.
        CHECK(!connectionOwnerInProc(tcp, QByteArray(), {browser.address, 51001},
                                     {ours.address, 48080}));

        // IPv6, and IPv4 mapped into IPv6 by a dual-stack socket.
        const QByteArray tcp6 =
            kProcHeader + procRow(0, kBrowserV6, kOursV6, "01", 1002) +
            procRow(1, kBrowserMapped, "0000000000000000FFFF00000100007F:BBD1", "01", 1003);
        CHECK(connectionOwnerInProc(QByteArray(), tcp6, {v6("::1"), 51000}, {v6("::1"), 48080}) ==
              std::optional<uint64_t>(1002));
        CHECK(connectionOwnerInProc(QByteArray(), tcp6, browser, {ours.address, 48081}) ==
              std::optional<uint64_t>(1003));
        // The same connection under two owners is no answer.
        const QByteArray twice = kProcHeader + procRow(0, kBrowserMapped, kOursMapped, "01", 1004);
        CHECK(!connectionOwnerInProc(tcp, twice, browser, ours));
        // Nor is a table that is not one, or a row that does not read.
        CHECK(!connectionOwnerInProc(QByteArray("not a table\n"), QByteArray(), browser, ours));
        CHECK(!connectionOwnerInProc(kProcHeader + "  0: 0100007F:C738 zz 01\n", QByteArray(),
                                     browser, ours));
        CHECK(!connectionOwnerInProc(tcp + procRow(9, "0100007G:C738", kOursV4, "01", 5),
                                     QByteArray(), browser, ours));
        CHECK(!connectionOwnerInProc(QByteArray(), QByteArray(), browser, ours));
        CHECK(!mw::sockettables::ownerAmong({}, {QByteArray(4, '\0'), 1}, ours,
                                            mw::sockettables::kLinuxEstablished));
    }

    SECTION("Socket tables — this process holds its own connection to itself, as the system "
            "says");
    {
        for (const QHostAddress& listenOn :
             {QHostAddress(QHostAddress::LocalHost), QHostAddress(QHostAddress::Any)}) {
            QTcpServer listener;
            CHECK(listener.listen(listenOn, 0));
            QTcpSocket browser;
            browser.connectToHost(QHostAddress(QHostAddress::LocalHost), listener.serverPort());
            CHECK(browser.waitForConnected(3000));
            CHECK(listener.waitForNewConnection(3000));
            QTcpSocket* accepted = listener.nextPendingConnection();
            CHECK(accepted != nullptr);
            if (!accepted) continue;
            HttpRequest req;
            req.clientAddress = accepted->peerAddress().toString();
            req.clientPort = accepted->peerPort();
            req.serverAddress = accepted->localAddress().toString();
            req.serverPort = accepted->localPort();
            const std::optional<MachineAccount> holder = OwnerHandoff::connectionAccount(req);
#if defined(Q_OS_MACOS)
            CHECK(!holder); // no tables read here: nothing is ever traded
#else
            CHECK(holder.has_value());
            if (holder) CHECK(OwnerHandoff::sameAccount(*holder, OwnerHandoff::thisProcess()));
            // A connection that is not there is held by nobody.
            HttpRequest none = req;
            none.clientPort =
                static_cast<quint16>(req.clientPort == 65535 ? 1 : req.clientPort + 1);
            CHECK(!OwnerHandoff::connectionAccount(none));
#endif
            HttpRequest offSocket = req;
            offSocket.clientPort = 0;
            CHECK(!OwnerHandoff::connectionAccount(offSocket));
            accepted->close();
            browser.close();
        }
    }

#ifdef Q_OS_WIN
    SECTION("Owner handoff — Windows: a nonce made for this very account, and a table that "
            "says another process holds the connection: 403");
    {
        using mw::sockettables::ownerAmong;
        const MachineAccount self = OwnerHandoff::thisProcess();
        CHECK(!self.sid.isEmpty());
        const quint32 ourPid = static_cast<quint32>(QCoreApplication::applicationPid());
        const TcpEnd browser{v6("127.0.0.1"), 51000};
        const TcpEnd ours{v6("127.0.0.1"), 48080};
        // The process a table names, as its token would say: ours is us, 777 another person.
        auto accountOf = [&self, ourPid](uint64_t pid) -> std::optional<MachineAccount> {
            if (pid == ourPid) return self;
            if (pid == 777) return personB();
            return std::nullopt;
        };
        auto tableSays = [&](const QList<TcpSocket>& table) {
            return [=]() -> std::optional<MachineAccount> {
                const std::optional<uint64_t> pid =
                    ownerAmong(table, browser, ours, mw::sockettables::kWindowsEstablished);
                return pid ? accountOf(*pid) : std::nullopt;
            };
        };
        const TcpSocket held{browser, ours, mw::sockettables::kWindowsEstablished, 777};
        const TcpSocket heldByUs{browser, ours, mw::sockettables::kWindowsEstablished, ourPid};
        const TcpSocket closing{browser, ours, 8, ourPid}; // CLOSE_WAIT
        const TcpSocket unknownPid{browser, ours, mw::sockettables::kWindowsEstablished, 4242};
        const QList<QList<TcpSocket>> refused = {
            {held}, {}, {closing}, {unknownPid}, {heldByUs, held}};
        int minted = 0;
        auto mint = [&minted]() {
            ++minted;
            return QStringLiteral("KEY");
        };
        for (const QList<TcpSocket>& table : refused) {
            OwnerHandoff handoff;
            const QString nonce = handoff.issue(kTarget, self);
            CHECK(!nonce.isEmpty());
            LogTap tap;
            CHECK_EQ(handoff.answer(pageRequest(nonce), true, tableSays(table), mint).statusCode,
                     403);
            CHECK_EQ(static_cast<int>(tap.warnings.size()), 1);
            CHECK(!tap.warnings.value(0).contains(self.sid));
        }
        CHECK_EQ(minted, 0);
        // The same, held by this process: the link.
        OwnerHandoff handoff;
        const QString nonce = handoff.issue(kTarget, self);
        CHECK_EQ(handoff.answer(pageRequest(nonce), true, tableSays({heldByUs}), mint).statusCode,
                 200);
        CHECK_EQ(minted, 1);
        // And the tables' own reading of a process: ours is this account.
        CHECK(mw::owneraccount::same(mw::owneraccount::accountOfProcess(ourPid), self.sid));
        CHECK(mw::owneraccount::accountOfProcess(0x7FFFFFF0u).isEmpty());
    }
#endif

    SECTION("Local channel — who asked is the server end's to say, never the caller's");
    {
        mw::localchannel::Request asked;
        asked.method = QStringLiteral("GET");
        asked.path = QStringLiteral("/api/server/remote-link");
        asked.query = QStringLiteral("open=1&p=/admin");
        asked.caller.readable = true;
        asked.caller.sid = personA().sid;
        asked.caller.uid = personA().uid;
        asked.caller.integrityRid = RequestGuard::kMediumIntegrity;
        const HttpRequest req = mw::localchannel::httpRequestFor(asked);
        CHECK(OwnerHandoff::sameAccount(OwnerHandoff::accountOf(req.localCaller), personA()));
        // Not on the wire: what a caller would put there does not arrive.
        const std::optional<mw::localchannel::Request> decoded =
            mw::localchannel::decodeRequest(mw::localchannel::encodeRequest(asked));
        CHECK(decoded.has_value());
        if (decoded) {
            CHECK(!decoded->caller.readable);
            CHECK(decoded->caller.sid.isEmpty());
            CHECK_EQ(decoded->caller.uid, static_cast<qint64>(-1));
        }
    }

    SECTION("Local channel — a caller the channel did not read, or a sandbox, is given no nonce");
    {
        // Not read: nobody, for whom no nonce is made.
        const LocalCaller unread;
        const MachineAccount nobody = OwnerHandoff::accountOf(unread);
        CHECK(nobody.sid.isEmpty());
        CHECK_EQ(nobody.uid, static_cast<qint64>(-1));
        CHECK(!OwnerHandoff::mayOpenFor(nobody));
        LocalCaller saidRead;
        saidRead.readable = true; // with no account in it
        CHECK(!OwnerHandoff::mayOpenFor(OwnerHandoff::accountOf(saidRead)));

        // A sandbox of the owner's own account, read in full: refused at the
        // channel's gate, before remote-link?open=1 is reached at all.
        LocalCaller sandbox;
        sandbox.readable = true;
        sandbox.sid =
            personA().sid.isEmpty() ? QStringLiteral("S-1-5-21-1-2-3-1001") : personA().sid;
        sandbox.integrityRid = 0x1000;
        for (bool service : {false, true}) {
            const RequestGuard::LocalVerdict verdict = RequestGuard::localCallerVerdict(
                service, sandbox.sid, QStringLiteral("S-1-5-32-544"), sandbox);
            CHECK(verdict == RequestGuard::LocalVerdict::Sandboxed);
            CHECK(RequestGuard::localChannelGate(QStringLiteral("GET"),
                                                 QStringLiteral("/api/server/remote-link"),
                                                 verdict) == RequestGuard::LocalGate::Forbidden);
        }
    }
}
