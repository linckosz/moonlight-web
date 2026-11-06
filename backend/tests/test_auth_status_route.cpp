/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/RestRouter.h"
#include "server/routes/AuthStatusRoute.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>
#include <utility>

// GET /api/auth/status as the server registers it, driven with requests built
// by hand: this machine's name for a caller on its LAN, by the rule a device's
// QR code request is held to (AuthManager::fromThisLan), and for everyone else
// the answer it has always been.

namespace {

const QString kHostHeader = QStringLiteral("192.168.1.10:8443");

/// The raw session cookie, as HttpServer::sessionTokenFromRequest reads it.
QString cookieToken(const HttpRequest& req)
{
    const QStringList parts = req.headers.value(QStringLiteral("cookie")).split(QLatin1Char(';'));
    for (const QString& part : parts) {
        const QString p = part.trimmed();
        if (p.startsWith(QLatin1String("mw_session="))) return p.mid(11);
    }
    return {};
}

/// The server around the route: a real AuthManager, the rest by hand.
struct Server
{
    QTemporaryDir tmp;
    AppSettings settings;
    std::unique_ptr<AuthManager> auth;
    RestRouter router;
    QString name = QStringLiteral("Living-room PC");
    QStringList located; // addresses a session was looked up for

    Server()
    {
        settings.m_FilePath = tmp.path() + "/settings.json";
        auth = std::make_unique<AuthManager>(&settings);
        auth->destroyAllSessions();
        registerAuthStatusRoute(
            router, *auth,
            {cookieToken,
             [this](const QString&, const QString& address) { located.append(address); },
             [this]() { return name; }});
    }

    HttpResponse send(HttpRequest req)
    {
        req.method = QStringLiteral("GET");
        req.path = QStringLiteral("/api/auth/status");
        HttpResponse reply;
        reply.statusCode = 0;
        router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
        CHECK_EQ(reply.statusCode, 200);
        return reply;
    }

    QJsonObject status(const HttpRequest& req)
    {
        return QJsonDocument::fromJson(send(req).body).object();
    }
};

/// A caller on a socket, from @p address, under a Host naming this machine.
HttpRequest direct(const QString& address, const QString& host = kHostHeader)
{
    HttpRequest req;
    req.clientAddress = address;
    req.hostTrusted = true;
    req.headers.insert(QStringLiteral("host"), host);
    return req;
}

/// A caller on the rendezvous line, from the address ICE settled on, or the
/// label a line goes by before it has one. The browser is on the rendezvous
/// server's name, never one of this machine's.
HttpRequest onLine(const QString& pair)
{
    HttpRequest req;
    req.clientAddress = pair;
    req.viaTunnel = true;
    req.headers.insert(QStringLiteral("host"), QStringLiteral("stream.example.test"));
    return req;
}

void testNamedOnThisLan()
{
    SECTION("auth status: this machine's name, for a caller on its LAN");
    Server s;
    QList<HttpRequest> lan;
    // A private address under a name of this machine: RFC 1918, link-local,
    // unique-local IPv6, an IPv4 one written as IPv6.
    for (const char* address :
         {"192.168.1.174", "10.0.0.7", "172.20.1.5", "169.254.10.20", "fe80::1c2b:3aff:fe4d:5e6f",
          "fd12:3456:789a::5", "::ffff:192.168.1.20"})
        lan.append(direct(QString::fromLatin1(address)));
    // ...or under its mDNS name.
    lan.append(direct(QStringLiteral("192.168.1.174"), QStringLiteral("dualrtx.local:8443")));
    // This machine itself, under its own name.
    const QList<QPair<QString, QString>> loopback{
        {QStringLiteral("127.0.0.1"), QStringLiteral("localhost:8443")},
        {QStringLiteral("127.0.0.1"), QStringLiteral("127.0.0.1")},
        {QStringLiteral("::1"), QStringLiteral("[::1]:8443")}};
    for (const QPair<QString, QString>& peer : loopback) {
        HttpRequest local = direct(peer.first, peer.second);
        local.isLocal = true;
        local.isHostMachine = true;
        lan.append(local);
    }
    // A proxy on this machine, under a local name, naming a visitor of this LAN.
    HttpRequest proxied = direct(QStringLiteral("127.0.0.1"), QStringLiteral("localhost:8443"));
    proxied.headers.insert(QStringLiteral("x-forwarded-for"), QStringLiteral("192.168.1.80"));
    lan.append(proxied);
    // On the line, the ICE pair private.
    lan.append(onLine(QStringLiteral("192.168.1.174")));
    lan.append(onLine(QStringLiteral("fd12:3456:789a::5")));

    for (const HttpRequest& req : lan)
        CHECK_EQ(s.status(req).value("display_name").toString(), s.name);

    // The name as it stands at each answer: renamed, it is the new one.
    s.name = QStringLiteral("Salon de L\u00e9o");
    CHECK_EQ(s.status(direct(QStringLiteral("192.168.1.174"))).value("display_name").toString(),
             s.name);
}

void testTheSameAnswerForEveryoneElse()
{
    SECTION("auth status: anyone else, the answer it has always been, with no name");
    Server s;
    // What a caller of this LAN reads, the name aside, is what each caller
    // below reads: nothing else changes with where it is.
    QJsonObject before = s.status(direct(QStringLiteral("192.168.1.174")));
    CHECK(before.contains("display_name"));
    before.remove(QStringLiteral("display_name"));

    QList<HttpRequest> elsewhere;
    // From the internet: a public IPv4, a global IPv6, a mesh VPN's 100.64/10.
    for (const char* address : {"203.0.113.5", "2001:db8::5", "100.64.1.2"})
        elsewhere.append(direct(QString::fromLatin1(address)));
    // A LAN address under a bare host name, or under a name that is not this
    // machine's (rebinding).
    HttpRequest bare = direct(QStringLiteral("192.168.1.174"), QStringLiteral("dualrtx:8443"));
    bare.hostTrusted = false;
    elsewhere.append(bare);
    HttpRequest rebound = direct(QStringLiteral("192.168.1.174"), QStringLiteral("rebind.test"));
    rebound.hostTrusted = false;
    elsewhere.append(rebound);
    // Loopback under a public name: a proxy on this machine forwarding the
    // internet. The address it names, or none, changes nothing.
    HttpRequest proxied = direct(QStringLiteral("127.0.0.1"), QStringLiteral("pc.example.test"));
    elsewhere.append(proxied);
    proxied.headers.insert(QStringLiteral("x-forwarded-for"), QStringLiteral("192.168.1.80"));
    elsewhere.append(proxied);
    // Loopback under a local name, the proxy naming a visitor from the internet.
    HttpRequest forwarded = direct(QStringLiteral("127.0.0.1"), QStringLiteral("localhost:8443"));
    forwarded.headers.insert(QStringLiteral("x-forwarded-for"), QStringLiteral("198.51.100.9"));
    elsewhere.append(forwarded);
    // On the line, a pair that is not private, or no pair yet.
    for (const char* pair :
         {"203.0.113.7", "2001:db8::7", "100.64.1.3", "tunnel:aaaaaaaaaaaaaaaa", "peer-4f2a"})
        elsewhere.append(onLine(QString::fromLatin1(pair)));

    for (const HttpRequest& req : elsewhere) {
        const QJsonObject answer = s.status(req);
        CHECK(!answer.contains("display_name"));
        CHECK(answer == before);
    }
}

void testASessionIsReadAsBefore()
{
    SECTION("auth status: a session read and slid as before, the name by the same rule");
    Server s;
    const QString token =
        s.auth->createSession(QStringLiteral("192.168.1.174"), QStringLiteral("Laptop"));
    const QString cookie = QStringLiteral("mw_session=") + token;

    // At home: signed in, the cookie slid, the name said.
    HttpRequest home = direct(QStringLiteral("192.168.1.174"));
    home.headers.insert(QStringLiteral("cookie"), cookie);
    const HttpResponse atHome = s.send(home);
    const QJsonObject homeAnswer = QJsonDocument::fromJson(atHome.body).object();
    CHECK(homeAnswer.value("authenticated").toBool());
    CHECK(homeAnswer.value("has_session").toBool());
    CHECK_EQ(homeAnswer.value("display_name").toString(), s.name);
    const QString slid = atHome.headers.value(QStringLiteral("Set-Cookie"));
    CHECK(slid.startsWith(cookie + QLatin1Char(';')));
    CHECK(slid.contains(QStringLiteral("Max-Age=7776000")));
    CHECK(s.located.isEmpty());

    // The same session from the internet: signed in still, its new address
    // looked up, and no name.
    HttpRequest away = direct(QStringLiteral("203.0.113.5"));
    away.headers.insert(QStringLiteral("cookie"), cookie);
    const QJsonObject awayAnswer = s.status(away);
    CHECK(awayAnswer.value("authenticated").toBool());
    CHECK(!awayAnswer.contains("display_name"));
    CHECK_EQ(s.located, QStringList{QStringLiteral("203.0.113.5")});
}

} // namespace

void run_auth_status_route_tests()
{
    testNamedOnThisLan();
    testTheSameAnswerForEveryoneElse();
    testASessionIsReadAsBefore();
}
