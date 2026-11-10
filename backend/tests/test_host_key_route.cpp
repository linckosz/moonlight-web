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
    QString key = kLinkKey;
    int spent = 0; // how many keys were redeemed

    Server()
    {
        settings.m_FilePath = tmp.path() + "/settings.json";
        auth = std::make_unique<AuthManager>(&settings);
        auth->destroyAllSessions();
        registerHostKeyRoute(router, *auth,
                             {[this]() { return key; }, [this]() { spent++; },
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
        const HttpResponse reply = s.send(direct("127.0.0.1"), {{"key", kLinkKey}});
        CHECK_EQ(reply.statusCode, 200);
        const QString token = tokenOf(reply);
        CHECK(!token.isEmpty());
        CHECK(s.auth->isHostSession(token));
        CHECK(!s.auth->isTunnelHostSession(token));
        CHECK_EQ(jsonOf(reply).value("is_localhost").toBool(), true);
        CHECK_EQ(s.spent, 1); // single-use
    }
    // This machine under its LAN address, another machine of the house, and
    // one behind the same public address.
    for (const QString& address : {kThisMachine, QStringLiteral("192.168.1.50"), kPublicIp}) {
        Server s;
        const HttpResponse reply = s.send(direct(address), {{"key", kLinkKey}});
        CHECK_EQ(reply.statusCode, 200);
        CHECK(s.auth->isHostSession(tokenOf(reply)));
    }

    SECTION("host key: refused, and nothing is spent");
    {
        Server s;
        // A wrong key, no key, and the right key from an address that is neither
        // private nor this machine's.
        CHECK_EQ(s.send(direct("127.0.0.1"), {{"key", "not-the-key"}}).statusCode, 403);
        CHECK_EQ(s.send(direct("127.0.0.1"), {}).statusCode, 403);
        CHECK_EQ(s.send(direct("198.51.100.20"), {{"key", kLinkKey}}).statusCode, 403);
        CHECK_EQ(s.send(onLine("198.51.100.20", newKey()), {{"key", kLinkKey}}).statusCode, 403);
        CHECK_EQ(s.auth->activeSessionCount(), 0);
        CHECK_EQ(s.spent, 0);
        // An install with no key to redeem refuses an empty one too.
        s.key.clear();
        CHECK_EQ(s.send(direct("127.0.0.1"), {{"key", ""}}).statusCode, 403);
        CHECK_EQ(s.auth->activeSessionCount(), 0);
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

    SECTION("host key: a page that names no key keeps the session it always had");
    {
        Server s;
        const HttpResponse reply = s.send(direct("127.0.0.1"), {{"key", kLinkKey}});
        CHECK_EQ(reply.statusCode, 200);
        const QString token = tokenOf(reply);
        CHECK(s.auth->isHostSession(token));
        CHECK(s.auth->sessionPairingKey(token).isEmpty());
        CHECK(!jsonOf(reply).contains("pairing_key"));
        CHECK(!jsonOf(reply).contains("host_public_key"));
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

} // namespace

void run_host_key_route_tests()
{
    testWhoMaySpendTheKey();
    testLineSessionIsBoundToItsLine();
    testSocketSessionTakesTheKeyItIsGiven();
}
