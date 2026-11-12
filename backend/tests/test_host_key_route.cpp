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

#include "test_framework.h"
#include "common/PairingCrypto.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/RestRouter.h"
#include "server/routes/HostKeyRoute.h"

#include <QDateTime>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <memory>
#include <utility>

// POST /api/auth/host-key as the server registers it, driven with requests built
// by hand: who may spend the key of the owner's link, and what the session it
// leaves with is bound to — the browser key of the rendezvous line it came by,
// or the one the browser names itself on a socket.

namespace {

const QString kLinkKey = QStringLiteral("k3yOfTheOwnersLink-0123456789abcdefghijklmn");
const QString kThisMachine = QStringLiteral("192.168.1.9");
const QString kPublicIp = QStringLiteral("203.0.113.7");

/// A fresh P-256 public key, SPKI DER.
QByteArray newKey()
{
    return PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());
}

/// The server around the route: a real AuthManager, the rest by hand.
struct Server
{
    QTemporaryDir tmp;
    AppSettings settings;
    std::unique_ptr<AuthManager> auth;
    RestRouter router;
    QString key = kLinkKey;  // the one key a link of this machine carries now
    int spent = 0;           // how many keys were redeemed
    QStringList refusedFrom; // the addresses a key that opened nothing came from

    Server()
    {
        settings.m_FilePath = tmp.path() + "/settings.json";
        auth = std::make_unique<AuthManager>(&settings);
        auth->destroyAllSessions();
        registerHostKeyRoute(router, *auth,
                             {// As the vault spends: once, by being recognised.
                              [this](const QString& given) {
                                  if (key.isEmpty() || given != key) return false;
                                  key.clear();
                                  spent++;
                                  return true;
                              },
                              [this](const QString& address) { refusedFrom.append(address); },
                              [](const QString& address) {
                                  return address == QLatin1String("127.0.0.1") ||
                                         address == kThisMachine;
                              },
                              []() { return kPublicIp; },
                              []() {
                                  return QJsonObject{{"host_public_key", "host-key-b64"},
                                                     {"host_id", "host-key-id"}};
                              }});
    }

    HttpResponse send(HttpRequest req, const QJsonObject& body)
    {
        req.method = QStringLiteral("POST");
        req.path = QStringLiteral("/api/auth/host-key");
        req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        HttpResponse reply;
        reply.statusCode = 0;
        router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
        return reply;
    }
};

/// The raw session token a reply set, empty when it set none.
QString tokenOf(const HttpResponse& reply)
{
    const QString cookie = reply.headers.value(QStringLiteral("Set-Cookie"));
    if (!cookie.startsWith(QLatin1String("mw_session="))) return {};
    return cookie.mid(11).section(QLatin1Char(';'), 0, 0);
}

QJsonObject jsonOf(const HttpResponse& reply)
{
    return QJsonDocument::fromJson(reply.body).object();
}

/// A browser on a socket, from @p address.
HttpRequest direct(const QString& address)
{
    HttpRequest req;
    req.clientAddress = address;
    req.hostTrusted = true;
    req.headers.insert(QStringLiteral("host"), QStringLiteral("localhost:8443"));
    return req;
}

/// What a page on a socket sends: the key of its link, and its browser's own.
QJsonObject withBrowserKey(const QString& linkKey, const QByteArray& browserKey = newKey())
{
    return {{"key", linkKey}, {"public_key", QString::fromLatin1(browserKey.toBase64())}};
}

/// A browser on a rendezvous line whose hello presented @p lineKey, from the
/// address ICE settled on.
HttpRequest onLine(const QString& address, const QByteArray& lineKey)
{
    HttpRequest req;
    req.clientAddress = address;
    req.viaTunnel = true;
    req.tunnelKey = lineKey;
    req.headers.insert(QStringLiteral("host"), QStringLiteral("stream.example.test"));
    return req;
}

void testWhoMaySpendTheKey()
{
    SECTION("host key: the right key, from an address that can be this machine");
    {
        Server s;
        const HttpResponse reply = s.send(direct("127.0.0.1"), withBrowserKey(kLinkKey));
        CHECK_EQ(reply.statusCode, 200);
        const QString token = tokenOf(reply);
        CHECK(!token.isEmpty());
        CHECK(s.auth->isHostSession(token));
        CHECK(!s.auth->isTunnelHostSession(token));
        CHECK_EQ(jsonOf(reply).value("is_localhost").toBool(), true);
        CHECK_EQ(s.spent, 1); // single-use
        CHECK(s.refusedFrom.isEmpty());

        // The same link a second time, as read back from a history: nothing.
        const HttpResponse again = s.send(direct("127.0.0.1"), withBrowserKey(kLinkKey));
        CHECK_EQ(again.statusCode, 403);
        CHECK(tokenOf(again).isEmpty());
        CHECK_EQ(s.auth->activeSessionCount(), 1);
        CHECK_EQ(s.spent, 1);
    }
    // This machine under its LAN address, and under its public address.
    for (const QString& address : {kThisMachine, kPublicIp}) {
        Server s;
        const HttpResponse reply = s.send(direct(address), withBrowserKey(kLinkKey));
        CHECK_EQ(reply.statusCode, 200);
        CHECK(s.auth->isHostSession(tokenOf(reply)));
    }

    SECTION("host key: refused, and nothing is spent");
    {
        Server s;
        // A wrong key, no key, and the right key from an address that is not
        // this machine's.
        CHECK_EQ(s.send(direct("127.0.0.1"), withBrowserKey("not-the-key")).statusCode, 403);
        CHECK_EQ(s.send(direct("127.0.0.1"), {}).statusCode, 403);
        CHECK_EQ(s.send(direct("198.51.100.20"), withBrowserKey(kLinkKey)).statusCode, 403);
        CHECK_EQ(s.send(onLine("198.51.100.20", newKey()), {{"key", kLinkKey}}).statusCode, 403);
        // Another machine of the house is not this machine either, whatever
        // private network it is on, on a socket or on a line.
        for (const char* other : {"192.168.1.50", "10.0.0.7", "172.16.4.2"}) {
            const HttpResponse r =
                s.send(direct(QString::fromLatin1(other)), withBrowserKey(kLinkKey));
            CHECK_EQ(r.statusCode, 403);
        }
        CHECK_EQ(s.send(onLine("192.168.1.50", newKey()), {{"key", kLinkKey}}).statusCode, 403);
        // The right key through a proxy on this machine: the peer is the proxy,
        // and the browser is wherever the proxy was reached from.
        for (const char* header : {"x-forwarded-for", "x-real-ip", "forwarded", "via"}) {
            HttpRequest proxied = direct("127.0.0.1");
            proxied.headers.insert(QString::fromLatin1(header), QStringLiteral("203.0.113.9"));
            CHECK_EQ(s.send(proxied, withBrowserKey(kLinkKey)).statusCode, 403);
        }
        CHECK_EQ(s.auth->activeSessionCount(), 0);
        CHECK_EQ(s.spent, 0);
        // Each of them was counted against the address it came from, as given.
        CHECK_EQ(static_cast<int>(s.refusedFrom.size()), 12);
        CHECK_EQ(static_cast<int>(s.refusedFrom.count(QStringLiteral("127.0.0.1"))), 6);
        CHECK_EQ(static_cast<int>(s.refusedFrom.count(QStringLiteral("198.51.100.20"))), 2);
        CHECK_EQ(static_cast<int>(s.refusedFrom.count(QStringLiteral("192.168.1.50"))), 2);
        CHECK_EQ(static_cast<int>(s.refusedFrom.count(QStringLiteral("10.0.0.7"))), 1);
        CHECK_EQ(static_cast<int>(s.refusedFrom.count(QStringLiteral("172.16.4.2"))), 1);
        // And the link is still good for the browser it was made for.
        CHECK_EQ(s.send(direct("127.0.0.1"), withBrowserKey(kLinkKey)).statusCode, 200);
        CHECK_EQ(s.spent, 1);
        // An install with no key waiting refuses an empty one too.
        CHECK_EQ(s.send(direct("127.0.0.1"), withBrowserKey(QString())).statusCode, 403);
        CHECK_EQ(s.auth->activeSessionCount(), 1);
    }
}

void testLineSessionIsBoundToItsLine()
{
    SECTION("host key: over a rendezvous line, the session is bound to the line's browser key");
    Server s;
    const QByteArray lineKey = newKey();
    const HttpResponse reply = s.send(onLine(kThisMachine, lineKey), {{"key", kLinkKey}});
    CHECK_EQ(reply.statusCode, 200);
    const QString token = tokenOf(reply);
    CHECK(s.auth->isTunnelHostSession(token));
    // The offer of a stream is signed for this key, and the browser checks it
    // against the host key it already holds for the line.
    CHECK(s.auth->sessionPairingKey(token) == lineKey);
    const QJsonObject obj = jsonOf(reply);
    CHECK_EQ(obj.value("pairing_key").toString(), QStringLiteral("bound"));
    CHECK_EQ(obj.value("host_public_key").toString(), QStringLiteral("host-key-b64"));
    CHECK_EQ(obj.value("host_id").toString(), QStringLiteral("host-key-id"));
    CHECK_EQ(s.spent, 1);

    SECTION("host key: the line's key, whatever the body names");
    {
        Server other;
        const QByteArray line = newKey();
        const QByteArray named = newKey();
        const HttpResponse r =
            other.send(onLine(kThisMachine, line),
                       {{"key", kLinkKey}, {"public_key", QString::fromLatin1(named.toBase64())}});
        CHECK_EQ(r.statusCode, 200);
        CHECK(other.auth->sessionPairingKey(tokenOf(r)) == line);
    }

    SECTION("host key: a line without a browser key opens nothing");
    {
        Server none;
        for (const QByteArray& bad : {QByteArray(), QByteArray("not a key")}) {
            const HttpResponse r = none.send(onLine(kThisMachine, bad), {{"key", kLinkKey}});
            CHECK_EQ(r.statusCode, 403);
            CHECK(tokenOf(r).isEmpty());
        }
        CHECK_EQ(none.auth->activeSessionCount(), 0);
        CHECK_EQ(none.spent, 0); // the owner's link is still good
    }
}

void testSocketSessionTakesTheKeyItIsGiven()
{
    SECTION("host key: on a socket, the session is bound to the key the browser names");
    {
        Server s;
        const QByteArray browserKey = newKey();
        const HttpResponse reply =
            s.send(direct("127.0.0.1"),
                   {{"key", kLinkKey}, {"public_key", QString::fromLatin1(browserKey.toBase64())}});
        CHECK_EQ(reply.statusCode, 200);
        CHECK(s.auth->sessionPairingKey(tokenOf(reply)) == browserKey);
        const QJsonObject obj = jsonOf(reply);
        CHECK_EQ(obj.value("pairing_key").toString(), QStringLiteral("bound"));
        CHECK_EQ(obj.value("host_id").toString(), QStringLiteral("host-key-id"));
    }

    SECTION("host key: a page that names no key opens nothing, and its link stays good");
    {
        Server s;
        const HttpResponse reply = s.send(direct("127.0.0.1"), {{"key", kLinkKey}});
        CHECK_EQ(reply.statusCode, 400);
        CHECK(tokenOf(reply).isEmpty());
        CHECK_EQ(s.auth->activeSessionCount(), 0);
        CHECK_EQ(s.spent, 0);
        // The same link, from a browser that names its key.
        const QByteArray browserKey = newKey();
        const HttpResponse again =
            s.send(direct("127.0.0.1"), withBrowserKey(kLinkKey, browserKey));
        CHECK_EQ(again.statusCode, 200);
        CHECK(s.auth->sessionPairingKey(tokenOf(again)) == browserKey);
        CHECK_EQ(s.spent, 1);
    }

    SECTION("host key: a key that does not parse is refused before the link is spent");
    {
        Server s;
        const HttpResponse reply =
            s.send(direct("127.0.0.1"), {{"key", kLinkKey}, {"public_key", "bm90IGEga2V5"}});
        CHECK_EQ(reply.statusCode, 400);
        CHECK(tokenOf(reply).isEmpty());
        CHECK_EQ(s.auth->activeSessionCount(), 0);
        CHECK_EQ(s.spent, 0);
        // The same link, from a page that names a key that parses.
        const QByteArray browserKey = newKey();
        const HttpResponse again =
            s.send(direct("127.0.0.1"),
                   {{"key", kLinkKey}, {"public_key", QString::fromLatin1(browserKey.toBase64())}});
        CHECK_EQ(again.statusCode, 200);
        CHECK(s.auth->sessionPairingKey(tokenOf(again)) == browserKey);
        CHECK_EQ(s.spent, 1);
    }
}

void testAddressThatCanBeThisMachine()
{
    SECTION("host key: the addresses that can be this machine");
    // What the server says of its own addresses, loopback included.
    const auto own = [](const QString& address) {
        return address == kThisMachine || address == QLatin1String("127.0.0.1") ||
               address == QLatin1String("::1");
    };
    CHECK(plausiblyHostAddress(kThisMachine, own, kPublicIp));
    CHECK(plausiblyHostAddress(QStringLiteral("127.0.0.1"), own, kPublicIp));
    CHECK(plausiblyHostAddress(QStringLiteral("::1"), own, kPublicIp));
    CHECK(plausiblyHostAddress(kPublicIp, own, kPublicIp));
    CHECK(!plausiblyHostAddress(QStringLiteral("198.51.100.4"), own, kPublicIp));
    CHECK(!plausiblyHostAddress(QStringLiteral("198.51.100.4"), own, QString()));
    CHECK(!plausiblyHostAddress(QString(), nullptr, QString()));
    // Another machine of a private network, of each of the three ranges and of
    // the two IPv6 ones, and one of a shared range (a VPN's): not this machine.
    for (const char* other : {"192.168.1.50", "10.0.0.7", "172.16.4.2", "169.254.7.7", "fd00::7",
                              "fe80::7", "100.64.0.9"}) {
        CHECK(!plausiblyHostAddress(QString::fromLatin1(other), own, kPublicIp));
    }
    // The public address counts only when it is known, and only itself.
    CHECK(!plausiblyHostAddress(kPublicIp, own, QString()));
    CHECK(!plausiblyHostAddress(kPublicIp, nullptr, QString()));

    SECTION("host key: which of this machine's addresses it was");
    CHECK(hostAddressOf(kThisMachine, own, kPublicIp) == HostAddress::ThisMachine);
    CHECK(hostAddressOf(QStringLiteral("127.0.0.1"), own, kPublicIp) == HostAddress::ThisMachine);
    CHECK(hostAddressOf(kPublicIp, own, kPublicIp) == HostAddress::PublicAddress);
    CHECK(hostAddressOf(QStringLiteral("192.168.1.50"), own, kPublicIp) == HostAddress::NotOurs);
    CHECK(hostAddressOf(QStringLiteral("198.51.100.4"), own, kPublicIp) == HostAddress::NotOurs);
    // A machine that holds its public address on an interface: its own first.
    CHECK(hostAddressOf(kThisMachine, own, kThisMachine) == HostAddress::ThisMachine);
}

/// A host session as a version before this one left it when the owner's link
/// was opened through a rendezvous line: made, and bound to no browser key.
QString olderLineSession(Server& s)
{
    return s.auth->createSession(kThisMachine, QStringLiteral("Host machine (remote link)"), true,
                                 false, true);
}

void testOlderLineSessionTakesTheKeyOfItsLine()
{
    using Adoption = AuthManager::LineKeyAdoption;
    const auto yes = []() { return true; };
    const auto no = []() { return false; };
    const qint64 later = QDateTime::currentSecsSinceEpoch() + 3600;

    SECTION("older host session: it takes the key of its line, once");
    {
        Server s;
        s.auth->setHostSessionRepairBefore(later);
        const QString token = olderLineSession(s);
        const QByteArray lineKey = newKey();
        CHECK(s.auth->sessionPairingKey(token).isEmpty());
        CHECK(s.auth->adoptLineKey(token, lineKey, yes) == Adoption::Bound);
        CHECK(s.auth->sessionPairingKey(token) == lineKey);
        // A session like any other from here: nothing more to take, and the
        // cookie on another browser's line keeps the key it has.
        CHECK(s.auth->adoptLineKey(token, lineKey, yes) == Adoption::NotConcerned);
        CHECK(s.auth->adoptLineKey(token, newKey(), yes) == Adoption::NotConcerned);
        CHECK(s.auth->sessionPairingKey(token) == lineKey);
        // Written with the session: it is still bound after a restart.
        s.auth->loadSessions();
        CHECK(s.auth->sessionPairingKey(token) == lineKey);
        CHECK(s.auth->isTunnelHostSession(token));
    }

    SECTION("older host session: not from an address that cannot be this machine");
    {
        Server s;
        s.auth->setHostSessionRepairBefore(later);
        const QString token = olderLineSession(s);
        const QByteArray lineKey = newKey();
        CHECK(s.auth->adoptLineKey(token, lineKey, no) == Adoption::Deferred);
        CHECK(s.auth->adoptLineKey(token, lineKey, nullptr) == Adoption::Deferred);
        CHECK(s.auth->sessionPairingKey(token).isEmpty());
        // Nothing is held against it: the same session, from home again.
        CHECK(s.auth->adoptLineKey(token, lineKey, yes) == Adoption::Bound);
        CHECK(s.auth->sessionPairingKey(token) == lineKey);
    }

    SECTION("older host session: a line without a browser key gives none");
    {
        Server s;
        s.auth->setHostSessionRepairBefore(later);
        const QString token = olderLineSession(s);
        int asked = 0;
        const auto counted = [&asked]() {
            asked++;
            return true;
        };
        CHECK(s.auth->adoptLineKey(token, QByteArray(), counted) == Adoption::NotConcerned);
        CHECK(s.auth->adoptLineKey(token, QByteArray("not a key"), counted) ==
              Adoption::NotConcerned);
        CHECK(s.auth->sessionPairingKey(token).isEmpty());
        CHECK_EQ(asked, 0); // the address is asked only for a session that can take a key
    }

    SECTION("older host session: only that kind of session, and only an older one");
    {
        Server s;
        const QByteArray lineKey = newKey();
        // No date read from the settings: nothing is older than it.
        const QString undated = olderLineSession(s);
        CHECK(s.auth->adoptLineKey(undated, lineKey, yes) == Adoption::NotConcerned);
        // A date before the session was made: the session is of this version.
        s.auth->setHostSessionRepairBefore(QDateTime::currentSecsSinceEpoch() - 3600);
        CHECK(s.auth->adoptLineKey(undated, lineKey, yes) == Adoption::NotConcerned);
        CHECK(s.auth->sessionPairingKey(undated).isEmpty());

        s.auth->setHostSessionRepairBefore(later);
        int asked = 0;
        const auto counted = [&asked]() {
            asked++;
            return true;
        };
        // A host session made on a socket, a session a PIN opened, on a socket
        // and on a line, no session at all, and a session that was ended.
        const QString onSocket =
            s.auth->createSession("127.0.0.1", QStringLiteral("Host machine"), true, false, false);
        const QString byPin = s.auth->createSession("198.51.100.4", QStringLiteral("Phone"));
        const QString lineGuest =
            s.auth->createSession("198.51.100.4", QStringLiteral("Phone"), false, false, true);
        CHECK(s.auth->adoptLineKey(onSocket, lineKey, counted) == Adoption::NotConcerned);
        CHECK(s.auth->adoptLineKey(byPin, lineKey, counted) == Adoption::NotConcerned);
        CHECK(s.auth->adoptLineKey(lineGuest, lineKey, counted) == Adoption::NotConcerned);
        CHECK(s.auth->adoptLineKey(QString(), lineKey, counted) == Adoption::NotConcerned);
        CHECK(s.auth->adoptLineKey(QStringLiteral("bogus-token"), lineKey, counted) ==
              Adoption::NotConcerned);
        const QString ended = olderLineSession(s);
        CHECK(s.auth->logoutSession(ended));
        CHECK(s.auth->adoptLineKey(ended, lineKey, counted) == Adoption::NotConcerned);
        CHECK(s.auth->sessionPairingKey(onSocket).isEmpty());
        CHECK(s.auth->sessionPairingKey(byPin).isEmpty());
        CHECK(s.auth->sessionPairingKey(lineGuest).isEmpty());
        CHECK_EQ(asked, 0);
    }

    SECTION("older host session: one that already holds a key keeps it");
    {
        Server s;
        s.auth->setHostSessionRepairBefore(later);
        const QString token = olderLineSession(s);
        const QByteArray own = newKey();
        CHECK(s.auth->bindSessionKey(token, own));
        CHECK(s.auth->adoptLineKey(token, newKey(), yes) == Adoption::NotConcerned);
        CHECK(s.auth->sessionPairingKey(token) == own);
    }
}

} // namespace

void run_host_key_route_tests()
{
    testWhoMaySpendTheKey();
    testLineSessionIsBoundToItsLine();
    testSocketSessionTakesTheKeyItIsGiven();
    testAddressThatCanBeThisMachine();
    testOlderLineSessionTakesTheKeyOfItsLine();
}
