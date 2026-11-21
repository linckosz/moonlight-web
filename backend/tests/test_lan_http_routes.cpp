/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 *
 * The routes of a TV on plain HTTP and of the administrator who lets it in
 * (issue #42), through the router: who may ask what, on which path, and what
 * each side is told.
 */

#include "test_framework.h"
#include "common/PairingCrypto.h"
#include "server/AppSettings.h"
#include "server/AuthManager.h"
#include "server/LanHttp.h"
#include "server/LanHttpPairing.h"
#include "server/RestRouter.h"
#include "server/routes/LanHttpRoutes.h"

#include <QHostAddress>
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
/// A paired phone of the house, and this host as it reaches it over HTTPS.
const QString kPhone = QStringLiteral("192.168.1.83");
const QString kHttpsHost = QStringLiteral("192.168.1.10:8443");

/// A paired device's keys: the private one it signs with, the public one its
/// session is bound to.
struct DeviceKey
{
    QByteArray pem = PairingCrypto::generatePrivateKeyPem();
    QByteArray spki = PairingCrypto::publicKeySpkiFromPrivatePem(pem);

    QString sign(const QByteArray& message) const
    {
        return QString::fromLatin1(PairingCrypto::sign(pem, message).toBase64());
    }
};

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
    /// This host's MW-BIND-v1 key, the one a paired device signs for.
    QByteArray hostKey =
        PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());

    Server()
    {
        settings.m_FilePath = tmp.path() + "/settings.json";
        auth = std::make_unique<AuthManager>(&settings);
        auth->destroyAllSessions();
        registerLanHttpRoutes(
            router, *auth, book,
            {[this]() { return clock; },
             [this](const QString& address) { guarded.append(address); }, mwSession,
             [this]() { return link; }, [this]() { return httpsPort; },
             [this]() { return hostKey; },
             []() {
                 // This host's link: the house's /24.
                 return QList<QPair<QHostAddress, int>>{
                     {QHostAddress(QStringLiteral("192.168.1.10")), 24}};
             },
             [](const QString& address) { return address == QLatin1String("192.168.1.10"); }});
    }

    QString hostId() const { return PairingCrypto::keyId(hostKey); }

    /// A paired device's session, made the way @p origin says, bound to @p key.
    QString device(SessionOrigin origin, const DeviceKey& key,
                   const QString& name = QStringLiteral("Galaxy S23"), bool ephemeral = false)
    {
        const QString token = auth->createSession(origin, kPhone, name, false, ephemeral);
        CHECK(auth->bindSessionKey(token, key.spki));
        return token;
    }

    /// A request of a paired device of the house, over HTTPS, with its session.
    HttpRequest phone(const QString& method, const QString& path, const QString& session,
                      const QJsonObject& body = {}, const QString& from = kPhone)
    {
        HttpRequest req;
        req.method = method;
        req.path = path;
        req.clientAddress = from;
        req.hostTrusted = true;
        req.headers[QStringLiteral("host")] = kHttpsHost;
        req.headers[QStringLiteral("cookie")] = QStringLiteral("mw_session=") + session;
        req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
        return req;
    }

    /// A paired device's yes to @p id, signed by @p key for this host.
    HttpRequest signedYes(const QString& id, const QString& code, const QString& session,
                          const DeviceKey& key)
    {
        return phone("POST", "/api/lan-http/approve/" + id, session,
                     {{"code", code},
                      {"sig", key.sign(LanHttpPairing::approveMessage(hostId(), id, code))}});
    }

    /// Its no, signed the same way.
    HttpRequest signedNo(const QString& id, const QString& session, const DeviceKey& key)
    {
        return phone("POST", "/api/lan-http/refuse/" + id, session,
                     {{"sig", key.sign(LanHttpPairing::refuseMessage(hostId(), id))}});
    }

    /// A TV asks: its request, code and claim.
    QJsonObject ask(const QString& from = kTv)
    {
        return QJsonDocument::fromJson(send(tv("POST", "/api/lan-http/pair", {}, {}, from)).body)
            .object();
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

QString errorOf(const HttpResponse& r)
{
    return bodyOf(r).value(QStringLiteral("error")).toString();
}

/// A code the TV does not show: its first character changed.
QString wrongCode(const QString& code)
{
    QString wrong = code;
    wrong[0] = wrong[0] == QLatin1Char('A') ? QLatin1Char('B') : QLatin1Char('A');
    return wrong;
}

/// The Max-Age of the cookie a response sets, -1 for none.
int maxAgeOf(const HttpResponse& r)
{
    const QString cookie = r.headers.value(QStringLiteral("Set-Cookie"));
    if (!cookie.contains(QLatin1String("Max-Age="))) return -1;
    return cookie.section(QStringLiteral("Max-Age="), 1).section(QLatin1Char(';'), 0, 0).toInt();
}

/// The TV at @p from asks and is let in for a short while by the device of
/// @p session: its cookie's token, empty when it was not let in.
QString letInShortly(Server& s, const QString& session, const DeviceKey& key,
                     const QString& from = kTv, HttpResponse* answer = nullptr)
{
    const QJsonObject a = s.ask(from);
    const QString id = a.value("request").toString();
    if (s.send(s.signedYes(id, a.value("code").toString(), session, key)).statusCode != 200)
        return {};
    const HttpResponse approved =
        s.send(s.tv("GET", "/api/lan-http/pair/" + id, {}, a.value("claim").toString(), from));
    if (answer) *answer = approved;
    return setToken(approved);
}

} // namespace

void run_lan_http_routes_tests()
{
    SECTION("LanHttp routes — a TV is let in by the code it shows");

    {
        Server s;
        const QString adminToken =
            s.auth->createSession(SessionOrigin::Unknown, kAdminPc, "Admin phone");

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
        // approved. Its browser gives its model.
        HttpRequest ask = s.tv("GET", "/api/lan-http/pair/" + id, {}, claim);
        ask.headers[QStringLiteral("user-agent")] =
            QStringLiteral("Mozilla/5.0 (Linux; Android 11; MiTV-MOSR4 Build/RTM5.220609.003; wv) "
                           "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.8103.38");
        const HttpResponse approved = s.send(ask);
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
        // Named after that model until somebody renames it.
        CHECK_EQ(info.machineName, QStringLiteral("MiTV-MOSR4"));
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
        // No model given: named by its address.
        CHECK_EQ(s.auth->sessionForToken(setToken(approved)).machineName, kTv);

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
        const QString adminToken =
            s.auth->createSession(SessionOrigin::Unknown, kAdminPc, "Admin phone");
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

        // A session without the administrator's right, that may not let a TV
        // in for a short while either (no key, not from the house): the one
        // answer, not counted, since it is only a way it may not take.
        const HttpResponse notAdmin =
            s.send(s.admin("POST", "/api/lan-http/approve/" + id, {{"code", a.value("code")}},
                           adminToken, /*isLocal=*/false));
        CHECK_EQ(notAdmin.statusCode, 403);
        CHECK_EQ(bodyOf(notAdmin).value("error").toString(), QStringLiteral("not_allowed_short"));
        CHECK_EQ(
            s.send(s.admin("GET", "/api/lan-http/approve/" + id, {}, adminToken, false)).statusCode,
            403);
        CHECK(s.guarded.isEmpty());
        // Without a live session at all: turned away and counted, as before.
        const HttpResponse noSession =
            s.send(s.admin("POST", "/api/lan-http/approve/" + id, {{"code", a.value("code")}},
                           QStringLiteral("not-a-session"), false));
        CHECK_EQ(noSession.statusCode, 403);
        CHECK_EQ(bodyOf(noSession).value("error").toString(), QStringLiteral("not_allowed"));
        CHECK_EQ(s.guarded.size(), qsizetype(1));
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

    SECTION("LanHttp routes — a paired device lets a TV in for a short while (TVNONADM)");

    {
        Server s;
        const DeviceKey key;
        const QString phone = s.device(SessionOrigin::Pin, key);
        const QJsonObject a = s.ask();
        const QString id = a.value("request").toString();
        const QString code = a.value("code").toString();
        CHECK_EQ(id.size(), qsizetype(22)); // 128 bits (R2)

        // The page: for how long, and what to sign for; never the code.
        const HttpResponse page = s.send(s.phone("GET", "/api/lan-http/approve/" + id, phone));
        CHECK_EQ(page.statusCode, 200);
        const QJsonObject p = bodyOf(page);
        CHECK_EQ(p.value("address").toString(), kTv);
        CHECK(p.value("short").toBool(false));
        CHECK_EQ(p.value("host_id").toString(), s.hostId());
        const qint64 endsIn = p.value("ends_in").toInteger();
        CHECK(endsIn > 8 * 3600 - 60 && endsIn <= 8 * 3600);
        CHECK(!page.body.contains(code.toLatin1()));

        CHECK_EQ(s.send(s.signedYes(id, code, phone, key)).statusCode, 200);
        const HttpResponse approved =
            s.send(s.tv("GET", "/api/lan-http/pair/" + id, {}, a.value("claim").toString()));
        CHECK_EQ(bodyOf(approved).value("state").toString(), QStringLiteral("approved"));
        const QString token = setToken(approved);
        CHECK(s.auth->validateLanHttpSession(token, kTv));
        const SessionInfo tv = s.auth->sessionForToken(token);
        CHECK(tv.lanHttp);
        CHECK(tv.approverBound);
        CHECK_EQ(tv.approvedBy, s.auth->sessionIdForToken(phone));
        CHECK(tv.endsAt > 0);
        // R8: its cookie lasts until its end, never past it.
        const int maxAge = maxAgeOf(approved);
        CHECK(maxAge > 8 * 3600 - 60 && maxAge <= 8 * 3600);
        // Its next visit: the same bound, never longer.
        const int again = maxAgeOf(s.send(s.tv("GET", "/api/lan-http/status", token)));
        CHECK(again >= 0 && again <= maxAge);
        // The administrator is told: the TV's address, by whom.
        CHECK_EQ(s.auth->shortNotices().size(), qsizetype(1));
        CHECK_EQ(s.auth->shortNotices().last().tvAddress, kTv);
        CHECK_EQ(s.auth->shortNotices().last().approverName, QStringLiteral("Galaxy S23"));
        CHECK(s.guarded.isEmpty());

        // The device revoked: the TV goes with it.
        s.auth->destroySession(s.auth->sessionIdForToken(phone));
        CHECK(!s.auth->validateLanHttpSession(token, kTv));
    }

    SECTION("LanHttp routes — a short approval is signed by the device's own key (R1)");

    {
        Server s;
        const DeviceKey key, other;
        const QString phone = s.device(SessionOrigin::Qr, key);
        const QString otherPhone = s.device(SessionOrigin::Pin, other, QStringLiteral("Pixel 8"));
        const QJsonObject a = s.ask();
        const QString id = a.value("request").toString();
        const QString code = a.value("code").toString();
        const auto yesWith = [&](const QJsonValue& sig) {
            QJsonObject body{{"code", code}};
            if (!sig.isNull()) body["sig"] = sig;
            return s.send(s.phone("POST", "/api/lan-http/approve/" + id, phone, body));
        };
        const auto refused = [&](const HttpResponse& r) {
            CHECK_EQ(r.statusCode, 403);
            CHECK_EQ(errorOf(r), QStringLiteral("not_allowed_short"));
        };

        // Signed by another device, with this one's session; and the reverse.
        refused(yesWith(other.sign(LanHttpPairing::approveMessage(s.hostId(), id, code))));
        refused(s.send(s.signedYes(id, code, otherPhone, key)));
        // Signed for another host, another request, as a no, or under MW-PAIR-v1.
        refused(yesWith(
            key.sign(LanHttpPairing::approveMessage(PairingCrypto::keyId(other.spki), id, code))));
        refused(yesWith(key.sign(LanHttpPairing::approveMessage(
            s.hostId(), QStringLiteral("AAAAAAAAAAAAAAAAAAAAAA"), code))));
        refused(yesWith(key.sign(LanHttpPairing::refuseMessage(s.hostId(), id))));
        refused(yesWith(key.sign(
            PairingCrypto::buildSignedMessage({QByteArrayLiteral("MW-PAIR-v1|approve"),
                                               s.hostId().toUtf8(), id.toUtf8(), code.toUtf8()}))));
        // Not signed at all, or with nonsense.
        refused(yesWith(QJsonValue()));
        refused(yesWith(QStringLiteral("not base64!")));
        // A no signed by another key.
        refused(s.send(s.signedNo(id, phone, other)));
        // Each counted, as a failed sign-in is.
        CHECK_EQ(s.guarded.size(), qsizetype(9));

        // R1b: a code out of the alphabet without a valid signature spends
        // nothing either, however often.
        for (int i = 0; i < LanHttpPairing::CODE_TRIES + 1; ++i)
            refused(s.send(s.phone("POST", "/api/lan-http/approve/" + id, phone,
                                   {{"code", QStringLiteral("!!!!!!")}})));
        CHECK_EQ(s.book.pending(s.clock), 1);
        // None of that spent a try: the right code, signed, still lets it in.
        CHECK_EQ(s.send(s.signedYes(id, code, phone, key)).statusCode, 200);
    }

    SECTION("LanHttp routes — wrong codes on the short path: the TV, the device (R1b, R3, S3)");

    {
        Server s;
        const DeviceKey key;
        const QString phone = s.device(SessionOrigin::PinProof, key);
        // Two failures of this TV already: one more locks it out.
        s.auth->countPairingFailure(kTv);
        s.auth->countPairingFailure(kTv);
        const QJsonObject a = s.ask();
        const QString id = a.value("request").toString();
        const QString wrong = wrongCode(a.value("code").toString());

        // Out of the alphabet, signed: a wrong code, as the comparison finds it.
        HttpResponse r = s.send(s.signedYes(id, QStringLiteral("!!!!!!"), phone, key));
        CHECK_EQ(r.statusCode, 400);
        CHECK_EQ(errorOf(r), QStringLiteral("wrong_code"));
        r = s.send(s.signedYes(id, wrong, phone, key));
        CHECK_EQ(r.statusCode, 400);
        // The last try: the request goes, the TV is counted, and the device,
        // at its third wrong code, is held back.
        r = s.send(s.signedYes(id, wrong, phone, key));
        CHECK_EQ(r.statusCode, 403);
        CHECK_EQ(errorOf(r), QStringLiteral("not_allowed_short"));
        CHECK_EQ(s.book.pending(s.clock), 0);
        CHECK_EQ(s.auth->failedAttemptCount(kTv), 3);
        CHECK(s.guarded.isEmpty());
        // R3: locked out, the TV may not ask again for now.
        CHECK_EQ(s.send(s.tv("POST", "/api/lan-http/pair")).statusCode, 429);
        // S3: the device may let no TV in for now, another one included.
        CHECK(!s.auth->mayLetInShortly(phone));
        const QJsonObject b = s.ask(kOtherTv);
        CHECK_EQ(errorOf(s.send(s.phone(
                     "GET", "/api/lan-http/approve/" + b.value("request").toString(), phone))),
                 QStringLiteral("not_allowed_short"));
    }

    {
        // The administrator's way, as it always was (R3): its last try spent,
        // the request is gone, and the TV is not counted.
        Server s;
        const QJsonObject a = s.ask();
        const QString id = a.value("request").toString();
        const QString wrong = wrongCode(a.value("code").toString());
        for (int i = 0; i < LanHttpPairing::CODE_TRIES - 1; ++i)
            CHECK_EQ(s.send(s.admin("POST", "/api/lan-http/approve/" + id, {{"code", wrong}}))
                         .statusCode,
                     400);
        CHECK_EQ(
            s.send(s.admin("POST", "/api/lan-http/approve/" + id, {{"code", wrong}})).statusCode,
            404);
        CHECK_EQ(s.auth->failedAttemptCount(kTv), 0);
        CHECK_EQ(s.send(s.tv("POST", "/api/lan-http/pair")).statusCode, 200);
    }

    SECTION("LanHttp routes — the shapes of a short approval, which spend nothing (R2, R1b)");

    {
        Server s;
        const DeviceKey key;
        const QString phone = s.device(SessionOrigin::Pin, key);
        const QJsonObject a = s.ask();
        const QString id = a.value("request").toString();
        const QString code = a.value("code").toString();

        // An id not of 22 characters is never looked up: the one answer.
        for (const QString& bad :
             {id.left(16), id + QLatin1Char('A'), QStringLiteral("not-an-id")}) {
            CHECK_EQ(errorOf(s.send(s.phone("GET", "/api/lan-http/approve/" + bad, phone))),
                     QStringLiteral("not_allowed_short"));
            CHECK_EQ(errorOf(s.send(s.signedYes(bad, code, phone, key))),
                     QStringLiteral("not_allowed_short"));
            CHECK_EQ(errorOf(s.send(s.signedNo(bad, phone, key))),
                     QStringLiteral("not_allowed_short"));
        }
        // An unknown id of the right shape: the same answer.
        const QString unknown = QStringLiteral("AAAAAAAAAAAAAAAAAAAAAA");
        CHECK_EQ(errorOf(s.send(s.phone("GET", "/api/lan-http/approve/" + unknown, phone))),
                 QStringLiteral("not_allowed_short"));
        CHECK_EQ(errorOf(s.send(s.signedYes(unknown, code, phone, key))),
                 QStringLiteral("not_allowed_short"));
        // A code longer than 16 bytes, signed: not looked at, no try spent.
        for (int i = 0; i < LanHttpPairing::CODE_TRIES + 1; ++i)
            CHECK_EQ(errorOf(s.send(s.signedYes(id, QString(17, QLatin1Char('A')), phone, key))),
                     QStringLiteral("not_allowed_short"));
        CHECK_EQ(s.book.pending(s.clock), 1);
        CHECK(s.auth->mayLetInShortly(phone)); // nor counted against the device
        CHECK(s.guarded.isEmpty());
        CHECK_EQ(s.send(s.signedYes(id, code, phone, key)).statusCode, 200);
        // The administrator keeps its own answer for an unknown id.
        CHECK_EQ(s.send(s.admin("GET", "/api/lan-http/approve/" + unknown)).statusCode, 404);
    }

    SECTION("LanHttp routes — who may not let a TV in for a short while (S1)");

    {
        Server s;
        const DeviceKey key;
        const QJsonObject a = s.ask();
        const QString id = a.value("request").toString();
        const QString path = "/api/lan-http/approve/" + id;
        // Each is told the one sentence, never why, and is not counted.
        const auto turnedAway = [&](const HttpRequest& req) {
            const HttpResponse r = s.send(req);
            CHECK_EQ(r.statusCode, 403);
            CHECK_EQ(errorOf(r), QStringLiteral("not_allowed_short"));
        };

        // Ways in that are not a person of the house's.
        for (SessionOrigin origin :
             {SessionOrigin::Certificate, SessionOrigin::HomeScreen, SessionOrigin::HostKey}) {
            const QString t = s.device(origin, key);
            turnedAway(s.phone("GET", path, t));
            turnedAway(s.signedYes(id, a.value("code").toString(), t, key));
            CHECK(s.auth->shortApprovalState(t) == AuthManager::ShortApproval::Refused);
        }
        // A session not remembered.
        turnedAway(s.phone("GET", path, s.device(SessionOrigin::Pin, key, "Guest", true)));
        // A session without a pairing key.
        turnedAway(
            s.phone("GET", path, s.auth->createSession(SessionOrigin::Pin, kPhone, "No key")));
        // The host machine's session.
        const QString host = s.auth->createSession(SessionOrigin::HostKey, kPhone, "Host", true);
        s.auth->bindSessionKey(host, key.spki);
        turnedAway(s.phone("GET", path, host));

        const QString phone = s.device(SessionOrigin::Pin, key);
        // Not from the house: the internet, this very machine, a proxy, a
        // public name, the local channel, a booth, a locked admin page.
        turnedAway(s.phone("GET", path, phone, {}, QStringLiteral("203.0.113.9")));
        turnedAway(s.phone("GET", path, phone, {}, QStringLiteral("192.168.1.10")));
        HttpRequest req = s.phone("GET", path, phone);
        req.headers[QStringLiteral("x-forwarded-for")] = QStringLiteral("192.168.1.83");
        turnedAway(req);
        req = s.phone("GET", path, phone);
        req.headers[QStringLiteral("host")] = QStringLiteral("stream.example.test");
        turnedAway(req);
        req = s.phone("GET", path, phone);
        req.localChannel = true;
        turnedAway(req);
        req = s.phone("GET", path, phone);
        req.boothShare = true;
        turnedAway(req);
        req = s.phone("GET", path, phone);
        req.boothPin = true;
        turnedAway(req);
        req = s.phone("GET", path, phone);
        req.adminLocked = true;
        turnedAway(req);
        // The administrator turned it off.
        s.settings.setLanHttpShortApproval(false);
        turnedAway(s.phone("GET", path, phone));
        s.settings.setLanHttpShortApproval(true);
        // Without a host key, no device may sign for it.
        s.hostKey.clear();
        turnedAway(s.signedYes(id, a.value("code").toString(), phone, key));
        CHECK(s.guarded.isEmpty());
        CHECK_EQ(s.book.pending(s.clock), 1);
    }

    SECTION("LanHttp routes — a device paired before v0.4.4 is told to pair again (TVNAO1 A)");

    {
        Server s;
        const DeviceKey oldKey, key;
        const QString old = s.device(SessionOrigin::Unknown, oldKey, QStringLiteral("Old phone"));
        const QString phone = s.device(SessionOrigin::Pin, key);
        CHECK(s.auth->shortApprovalState(old) == AuthManager::ShortApproval::ReappairNeeded);
        CHECK(!s.auth->mayLetInShortly(old));
        const QJsonObject a = s.ask();
        const QString id = a.value("request").toString();
        const QString code = a.value("code").toString();
        const QString path = "/api/lan-http/approve/" + id;

        // G2: it sees the request as a device that may, field for field.
        const HttpResponse seenOld = s.send(s.phone("GET", path, old));
        CHECK_EQ(seenOld.statusCode, 200);
        const QJsonObject o = bodyOf(seenOld);
        const QJsonObject n = bodyOf(s.send(s.phone("GET", path, phone)));
        CHECK_EQ(o.keys(), n.keys());
        CHECK_EQ(o.value("address").toString(), kTv);
        CHECK(o.value("short").toBool(false));
        CHECK_EQ(o.value("host_id").toString(), s.hostId());
        CHECK(!seenOld.body.contains(code.toLatin1()));

        const auto toldToPairAgain = [&](const HttpRequest& req) {
            const HttpResponse r = s.send(req);
            CHECK_EQ(r.statusCode, 403);
            CHECK_EQ(errorOf(r), QStringLiteral("reappair_needed"));
        };
        // B1: its signed yes, the right code or a wrong one, and its signed
        // no: told to pair again, the request never looked at.
        toldToPairAgain(s.signedYes(id, code, old, oldKey));
        for (int i = 0; i < LanHttpPairing::CODE_TRIES + 1; ++i)
            toldToPairAgain(s.signedYes(id, wrongCode(code), old, oldKey));
        toldToPairAgain(s.signedNo(id, old, oldKey));
        // An id that does not exist, of the right shape: the same answer.
        const QString unknown = QStringLiteral("AAAAAAAAAAAAAAAAAAAAAA");
        toldToPairAgain(s.signedYes(unknown, code, old, oldKey));
        toldToPairAgain(s.signedNo(unknown, old, oldKey));
        // Its GET of an unknown id: the one refusal, as for a device that may.
        CHECK_EQ(errorOf(s.send(s.phone("GET", "/api/lan-http/approve/" + unknown, old))),
                 QStringLiteral("not_allowed_short"));
        CHECK_EQ(errorOf(s.send(s.phone("GET", "/api/lan-http/approve/" + unknown, phone))),
                 QStringLiteral("not_allowed_short"));
        // Nothing was spent or counted: the request waits, the device is not
        // held back.
        CHECK_EQ(s.book.pending(s.clock), 1);
        CHECK(s.guarded.isEmpty());
        CHECK(s.auth->shortApprovalState(old) == AuthManager::ShortApproval::ReappairNeeded);
        // Not signed by its own key: the one refusal, counted.
        HttpResponse r = s.send(s.signedYes(id, code, old, key));
        CHECK_EQ(errorOf(r), QStringLiteral("not_allowed_short"));
        CHECK_EQ(s.guarded.size(), qsizetype(1));

        // G3: a device that may opens the same request and lets the TV in.
        CHECK_EQ(s.send(s.signedYes(id, code, phone, key)).statusCode, 200);
        CHECK_EQ(
            bodyOf(s.send(s.tv("GET", "/api/lan-http/pair/" + id, {}, a.value("claim").toString())))
                .value("state")
                .toString(),
            QStringLiteral("approved"));
    }

    SECTION("LanHttp routes — the administrator's setting and duration (S6)");

    {
        Server s;
        const DeviceKey key;
        const QString phone = s.device(SessionOrigin::Pin, key);
        CHECK(!s.settings.setLanHttpShortHours(3));
        CHECK(s.settings.setLanHttpShortHours(2));
        HttpResponse approved;
        const QString shortTv = letInShortly(s, phone, key, kTv, &approved);
        CHECK(!shortTv.isEmpty());
        const int maxAge = maxAgeOf(approved);
        CHECK(maxAge > 2 * 3600 - 60 && maxAge <= 2 * 3600);

        // A TV the administrator let in keeps its 30 days whatever this says.
        const QJsonObject b = s.ask(kOtherTv);
        CHECK_EQ(s.send(s.admin("POST", "/api/lan-http/approve/" + b.value("request").toString(),
                                {{"code", b.value("code")}}))
                     .statusCode,
                 200);
        const QString adminTv =
            setToken(s.send(s.tv("GET", "/api/lan-http/pair/" + b.value("request").toString(), {},
                                 b.value("claim").toString(), kOtherTv)));
        CHECK(!s.auth->sessionForToken(adminTv).approverBound);

        // Turned off: the TVs let in that way leave, the administrator's stays.
        s.settings.setLanHttpShortApproval(false);
        CHECK_EQ(s.auth->endShortLanHttpSessions(), 1);
        CHECK(!s.auth->validateLanHttpSession(shortTv, kTv));
        CHECK(s.auth->validateLanHttpSession(adminTv, kOtherTv));

        // Turned off between the device's yes and the TV's poll: gone, no
        // session and no cookie, not even until the TV's next request (F1).
        s.settings.setLanHttpShortApproval(true);
        const QJsonObject c = s.ask(kTv);
        const QString id = c.value("request").toString();
        CHECK_EQ(s.send(s.signedYes(id, c.value("code").toString(), phone, key)).statusCode, 200);
        s.settings.setLanHttpShortApproval(false);
        const HttpResponse late =
            s.send(s.tv("GET", "/api/lan-http/pair/" + id, {}, c.value("claim").toString(), kTv));
        CHECK_EQ(late.statusCode, 404);
        CHECK_EQ(bodyOf(late).value("state").toString(), QStringLiteral("gone"));
        CHECK(!late.headers.contains("Set-Cookie"));
        CHECK_EQ(s.auth->endShortLanHttpSessions(), 0);
        CHECK(s.auth->validateLanHttpSession(adminTv, kOtherTv));
    }

    SECTION("LanHttp routes — one TV let in for a short while per device and address (G)");

    {
        Server s;
        const DeviceKey key, other;
        const QString phone = s.device(SessionOrigin::Pin, key);
        const QString first = letInShortly(s, phone, key);
        CHECK(!first.isEmpty());
        // Let in again at the same address: the new one replaces the old.
        const QString second = letInShortly(s, phone, key);
        CHECK(!second.isEmpty());
        CHECK(!s.auth->validateLanHttpSession(first, kTv));
        CHECK(s.auth->validateLanHttpSession(second, kTv));
        // Another TV of the same device: one per device, it replaces it too.
        const QString third = letInShortly(s, phone, key, kOtherTv);
        CHECK(!third.isEmpty());
        CHECK(!s.auth->validateLanHttpSession(second, kTv));
        CHECK(s.auth->validateLanHttpSession(third, kOtherTv));
        // Another device at that address: one per address, it replaces it.
        const QString otherPhone = s.device(SessionOrigin::Qr, other, QStringLiteral("Pixel 8"));
        const QString fourth = letInShortly(s, otherPhone, other, kOtherTv);
        CHECK(!fourth.isEmpty());
        CHECK(!s.auth->validateLanHttpSession(third, kOtherTv));
        CHECK(s.auth->validateLanHttpSession(fourth, kOtherTv));
        // Another device at another address: both stay.
        const QString fifth = letInShortly(s, phone, key, kTv);
        CHECK(s.auth->validateLanHttpSession(fifth, kTv));
        CHECK(s.auth->validateLanHttpSession(fourth, kOtherTv));
        // The administrator lets a TV in at that address: the short one goes.
        const QJsonObject a = s.ask();
        CHECK_EQ(s.send(s.admin("POST", "/api/lan-http/approve/" + a.value("request").toString(),
                                {{"code", a.value("code")}}))
                     .statusCode,
                 200);
        const QString byAdmin =
            setToken(s.send(s.tv("GET", "/api/lan-http/pair/" + a.value("request").toString(), {},
                                 a.value("claim").toString())));
        CHECK(s.auth->validateLanHttpSession(byAdmin, kTv));
        CHECK(!s.auth->validateLanHttpSession(fifth, kTv));
        CHECK(s.auth->validateLanHttpSession(fourth, kOtherTv));
    }
}
