/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The routes of a TV on plain HTTP and of the administrator who lets it in
 * (issue #42), through the router: who may ask what, on which path, and what
 * each side is told.
 */

#include "test_framework.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/LanHttp.h"
#include "server/LanHttpPairing.h"
#include "server/RestRouter.h"
#include "server/routes/LanHttpRoutes.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>

#include <memory>
#include <utility>

namespace {

const QString kTv = QStringLiteral("192.168.1.174");
const QString kOtherTv = QStringLiteral("192.168.1.175");
const QString kAdminPc = QStringLiteral("192.168.1.20");
const QString kTvHost = QStringLiteral("192.168.1.10:8080");

QString mwSession(const HttpRequest& req)
{
    for (const QString& part :
         req.headers.value(QStringLiteral("cookie")).split(QLatin1Char(';'))) {
        const QString p = part.trimmed();
        if (p.startsWith(QLatin1String("mw_session="))) return p.mid(11);
    }
    return {};
}

QJsonObject bodyOf(const HttpResponse& r)
{
    return QJsonDocument::fromJson(r.body).object();
}

/// The value of the mw_lan_http cookie a response sets, empty for none.
QString setToken(const HttpResponse& r)
{
    return LanHttp::tokenFromCookieHeader(
        r.headers.value(QStringLiteral("Set-Cookie")).section(QLatin1Char(';'), 0, 0));
}

struct Server
{
    QTemporaryDir tmp;
    AppSettings settings;
    std::unique_ptr<AuthManager> auth;
    LanHttpPairing book;
    RestRouter router;
    qint64 clock = 1'800'000'000;
    QString link = QStringLiteral("https://stream.example.test/abcdefghjkmnpqrstvwxyz0123");
    quint16 httpsPort = 8443;
    QStringList guarded;

    Server()
    {
        settings.m_FilePath = tmp.path() + "/settings.json";
        auth = std::make_unique<AuthManager>(&settings);
        auth->destroyAllSessions();
        registerLanHttpRoutes(router, *auth, book,
                              {[this]() { return clock; },
                               [this](const QString& address) { guarded.append(address); },
                               mwSession, [this]() { return link; },
                               [this]() { return httpsPort; }});
    }

    HttpResponse send(const HttpRequest& req)
    {
        HttpResponse reply;
        router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
        return reply;
    }

    /// A request of the TV, on plain HTTP.
    HttpRequest tv(const QString& method, const QString& path, const QString& cookie = {},
                   const QString& claim = {}, const QString& from = kTv)
    {
        HttpRequest req;
        req.method = method;
        req.path = path;
        req.clientAddress = from;
        req.lanHttp = true;
        req.lanHttpHost = kTvHost;
        if (!cookie.isEmpty())
            req.headers[QStringLiteral("cookie")] = QStringLiteral("mw_lan_http=") + cookie;
        if (!claim.isEmpty()) req.headers[QStringLiteral("x-mw-claim")] = claim;
        return req;
    }

    /// A request of the administrator, over HTTPS: an admin session, or the
    /// host machine without one.
    HttpRequest admin(const QString& method, const QString& path, const QJsonObject& body = {},
                      const QString& session = {}, bool isLocal = true)
    {
        HttpRequest req;
        req.method = method;
        req.path = path;
        req.clientAddress = kAdminPc;
        req.isLocal = isLocal;
        if (!session.isEmpty())
            req.headers[QStringLiteral("cookie")] = QStringLiteral("mw_session=") + session;
        req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        return req;
    }
};

} // namespace

void run_lan_http_routes_tests()
{
    SECTION("LanHttp routes — a TV is let in by the code it shows");

    {
        Server s;
        const QString adminToken = s.auth->createSession(kAdminPc, "Admin phone");

        // Not signed in yet.
        HttpResponse status = s.send(s.tv("GET", "/api/lan-http/status"));
        CHECK_EQ(status.statusCode, 200);
        CHECK(!bodyOf(status).value("signed_in").toBool(true));
        CHECK(!status.headers.contains("Set-Cookie"));

        // It asks: an id, a code, a claim, and the approval page under this
        // machine's public link.
        const HttpResponse asked = s.send(s.tv("POST", "/api/lan-http/pair"));
        CHECK_EQ(asked.statusCode, 200);
        const QJsonObject a = bodyOf(asked);
        const QString id = a.value("request").toString();
        const QString code = a.value("code").toString();
        const QString claim = a.value("claim").toString();
        CHECK_EQ(code.size(), qsizetype(LanHttpPairing::CODE_LENGTH));
        CHECK(!claim.isEmpty());
        CHECK_EQ(a.value("expires_in").toInt(), int(LanHttpPairing::TTL_SECS));
        CHECK_EQ(a.value("approve_url").toString(), s.link + QStringLiteral("/tv-pair-") + id);

        CHECK_EQ(bodyOf(s.send(s.tv("GET", "/api/lan-http/pair/" + id, {}, claim)))
                     .value("state")
                     .toString(),
                 QStringLiteral("pending"));

        // The approval page: the TV's address, never its code.
        const HttpResponse page = s.send(s.admin("GET", "/api/lan-http/approve/" + id));
        CHECK_EQ(page.statusCode, 200);
        CHECK_EQ(bodyOf(page).value("address").toString(), kTv);
        CHECK(!bodyOf(page).contains("code"));
        CHECK(!page.body.contains(code.toLatin1()));

        // A wrong code, then the right one, typed by an admin session.
        QString wrong = code;
        wrong[0] = wrong[0] == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
        CHECK_EQ(
            s.send(s.admin("POST", "/api/lan-http/approve/" + id, {{"code", wrong}}, adminToken))
                .statusCode,
            400);
        CHECK_EQ(
            s.send(s.admin("POST", "/api/lan-http/approve/" + id, {{"code", code}}, adminToken))
                .statusCode,
            200);

        // The TV's next ask: approved, its cookie set, and nothing about who
        // approved.
        const HttpResponse approved = s.send(s.tv("GET", "/api/lan-http/pair/" + id, {}, claim));
        CHECK_EQ(approved.statusCode, 200);
        CHECK_EQ(bodyOf(approved).value("state").toString(), QStringLiteral("approved"));
        CHECK_EQ(bodyOf(approved).keys(), QStringList{QStringLiteral("state")});
        CHECK(!approved.body.contains("Admin phone"));
        const QString cookie = approved.headers.value("Set-Cookie");
        CHECK(cookie.startsWith(QStringLiteral("mw_lan_http=")));
        CHECK(!cookie.contains(QStringLiteral("Secure")));
        CHECK(cookie.contains(QStringLiteral("Max-Age=2592000")));
        const QString token = setToken(approved);
        CHECK(!token.isEmpty());
        // Spent: asked again, there is nothing.
        CHECK_EQ(s.send(s.tv("GET", "/api/lan-http/pair/" + id, {}, claim)).statusCode, 404);

        // A TV's session: bound to its address, never an HTTPS one, revoked
        // with the session that approved it.
        CHECK(s.auth->validateLanHttpSession(token, kTv));
        CHECK(!s.auth->validateSession(token));
        const SessionInfo info = s.auth->sessionForToken(token);
        CHECK(info.lanHttp);
        CHECK_EQ(info.machineName, QStringLiteral("Browser at 192.168.1.174"));
        CHECK_EQ(info.approvedBy, s.auth->sessionIdForToken(adminToken));

        // Signed in now, and its cookie given its time again.
        status = s.send(s.tv("GET", "/api/lan-http/status", token));
        CHECK(bodyOf(status).value("signed_in").toBool(false));
        CHECK_EQ(setToken(status), token);
        CHECK(status.headers.value("Set-Cookie").contains(QStringLiteral("Max-Age=2592000")));

        // The same cookie from another address is not this TV's.
        status = s.send(s.tv("GET", "/api/lan-http/status", token, {}, kOtherTv));
        CHECK(!bodyOf(status).value("signed_in").toBool(true));
        CHECK(status.headers.value("Set-Cookie").contains(QStringLiteral("Max-Age=0")));

        s.auth->destroySession(s.auth->sessionIdForToken(adminToken));
        CHECK(!s.auth->validateLanHttpSession(token, kTv));
        CHECK(s.guarded.isEmpty());
    }

    SECTION("LanHttp routes — the approval page on this LAN, without a public link");

    {
        Server s;
        s.link.clear();
        const QJsonObject a = bodyOf(s.send(s.tv("POST", "/api/lan-http/pair")));
        CHECK_EQ(a.value("approve_url").toString(),
                 QStringLiteral("https://192.168.1.10:8443/tv-pair-") +
                     a.value("request").toString());
        s.httpsPort = 443;
        const QJsonObject b = bodyOf(s.send(s.tv("POST", "/api/lan-http/pair")));
        CHECK_EQ(b.value("approve_url").toString(),
                 QStringLiteral("https://192.168.1.10/tv-pair-") + b.value("request").toString());
    }

    SECTION("LanHttp routes — the host machine approves, or refuses");

    {
        Server s;
        const QJsonObject a = bodyOf(s.send(s.tv("POST", "/api/lan-http/pair")));
        const QString id = a.value("request").toString();
        const QString claim = a.value("claim").toString();
        CHECK_EQ(s.send(s.admin("POST", "/api/lan-http/approve/" + id, {{"code", a.value("code")}}))
                     .statusCode,
                 200);
        const HttpResponse approved = s.send(s.tv("GET", "/api/lan-http/pair/" + id, {}, claim));
        CHECK(s.auth->sessionForToken(setToken(approved)).approvedBy.isEmpty());

        const QJsonObject b = bodyOf(s.send(s.tv("POST", "/api/lan-http/pair")));
        CHECK_EQ(s.send(s.admin("POST", "/api/lan-http/refuse/" + b.value("request").toString()))
                     .statusCode,
                 200);
        const HttpResponse refused =
            s.send(s.tv("GET", "/api/lan-http/pair/" + b.value("request").toString(), {},
                        b.value("claim").toString()));
        CHECK_EQ(bodyOf(refused).value("state").toString(), QStringLiteral("refused"));
        CHECK(setToken(refused).isEmpty());
    }

    SECTION("LanHttp routes — each side's routes are not the other's");

    {
        Server s;
        const QString adminToken = s.auth->createSession(kAdminPc, "Admin phone");
        const QJsonObject a = bodyOf(s.send(s.tv("POST", "/api/lan-http/pair")));
        const QString id = a.value("request").toString();

        // The TV's routes do not exist over HTTPS, however privileged the caller.
        HttpRequest https = s.admin("POST", "/api/lan-http/pair");
        CHECK_EQ(s.send(https).statusCode, 404);
        https = s.admin("GET", "/api/lan-http/status");
        CHECK_EQ(s.send(https).statusCode, 404);
        https = s.admin("GET", "/api/lan-http/pair/" + id);
        https.headers["x-mw-claim"] = a.value("claim").toString();
        CHECK_EQ(s.send(https).statusCode, 404);

        // The administrator's routes do not exist on the TV's path, even for a
        // request somehow marked privileged.
        HttpRequest fromTv = s.tv("POST", "/api/lan-http/approve/" + id);
        fromTv.isLocal = true;
        fromTv.body = QJsonDocument(QJsonObject{{"code", a.value("code")}}).toJson();
        CHECK_EQ(s.send(fromTv).statusCode, 404);
        CHECK_EQ(s.send(s.tv("GET", "/api/lan-http/approve/" + id)).statusCode, 404);
        CHECK_EQ(s.send(s.tv("POST", "/api/lan-http/refuse/" + id)).statusCode, 404);

        // A session without the administrator's right is turned away, counted.
        CHECK_EQ(s.send(s.admin("POST", "/api/lan-http/approve/" + id, {{"code", a.value("code")}},
                                adminToken, /*isLocal=*/false))
                     .statusCode,
                 403);
        CHECK_EQ(
            s.send(s.admin("GET", "/api/lan-http/approve/" + id, {}, adminToken, false)).statusCode,
            403);
        CHECK_EQ(s.guarded.size(), qsizetype(2));
        CHECK_EQ(s.book.pending(s.clock), 1); // still waiting, untouched

        // Another device, or a page without the claim, learns nothing.
        CHECK_EQ(s.send(s.tv("GET", "/api/lan-http/pair/" + id, {}, a.value("claim").toString(),
                             kOtherTv))
                     .statusCode,
                 404);
        CHECK_EQ(s.send(s.tv("GET", "/api/lan-http/pair/" + id)).statusCode, 404);
        CHECK_EQ(s.send(s.tv("GET", "/api/lan-http/pair/not-an-id")).statusCode, 404);
        CHECK_EQ(s.book.pending(s.clock), 1);
    }

    SECTION("LanHttp routes — a full book");

    {
        Server s;
        for (int i = 0; i < LanHttpPairing::MAX_PENDING; ++i)
            CHECK_EQ(s.send(s.tv("POST", "/api/lan-http/pair", {}, {},
                                 QStringLiteral("192.168.1.%1").arg(200 + i)))
                         .statusCode,
                     200);
        CHECK_EQ(s.send(s.tv("POST", "/api/lan-http/pair")).statusCode, 429);
    }
}
