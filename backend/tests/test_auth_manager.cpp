/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "common/Logger.h"
#include "common/PairingCrypto.h"
#include "common/PinBinding.h"
#include "common/Types.h"
#include "network/GeoIpService.h"
#include "server/AuthManager.h"
#include "server/AppSettings.h"
#include "server/RememberMeWrite.h"
#include "server/RestRouter.h"
#include "server/SessionCookie.h"
#include "server/routes/PinProofRoutes.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>

#include <functional>
#include <optional>
#include <utility>

namespace {

// ── MW-PIN-v1 and MW-TLS-v1 through their routes ───────────────────────────

/// A request as HttpServer hands it to a route: from @p peer, on a TLS socket
/// that was served the key @p served (none: plain HTTP).
HttpRequest requestFrom(const QString& peer, const QByteArray& served)
{
    HttpRequest req;
    req.clientAddress = peer;
    req.tlsSpki = served;
    return req;
}

HttpResponse dispatch(const RestRouter& router, HttpRequest req, const QString& method,
                      const QString& path, const QJsonObject& body = {})
{
    req.method = method;
    req.path = path;
    if (!body.isEmpty()) req.body = QJsonDocument(body).toJson(QJsonDocument::Compact);
    HttpResponse reply;
    reply.statusCode = 0; // a route that never answers shows as 0
    router.dispatchAsync(req, [&reply](HttpResponse resp) { reply = std::move(resp); });
    return reply;
}

QJsonObject bodyOf(const HttpResponse& reply)
{
    return QJsonDocument::fromJson(reply.body).object();
}

QString encoded(const QByteArray& bytes)
{
    return QString::fromLatin1(bytes.toBase64());
}

QByteArray decoded(const QJsonValue& value)
{
    return QByteArray::fromBase64(value.toString().toLatin1());
}

QString urlText(const QByteArray& bytes)
{
    return QString::fromLatin1(
        bytes.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

/// A TLS key as the routes name it: the SHA-256 of its SPKI, base64url.
QString keyText(const QByteArray& spki)
{
    return urlText(QCryptographicHash::hash(spki, QCryptographicHash::Sha256));
}

/// A fresh P-256 public key, SPKI DER.
QByteArray newKey()
{
    return PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());
}

/// w·M, uncompressed, M as RFC 9382 gives it: the first share under which, with
/// the right w, the host's K is the identity.
QByteArray wTimesM(const QByteArray& w)
{
    const QByteArray m = QByteArray::fromHex(
        QByteArrayLiteral("02886e2f97ace46e55ba9dd7242579f2993b64e16ef3dcab95afd497333d8fa12f"));
    QByteArray out;
    EC_GROUP* group = EC_GROUP_new_by_curve_name(NID_X9_62_prime256v1);
    EC_POINT* point = group ? EC_POINT_new(group) : nullptr;
    EC_POINT* product = group ? EC_POINT_new(group) : nullptr;
    BIGNUM* scalar = BN_bin2bn(reinterpret_cast<const unsigned char*>(w.constData()),
                               static_cast<int>(w.size()), nullptr);
    unsigned char buf[65];
    if (point && product && scalar &&
        EC_POINT_oct2point(group, point, reinterpret_cast<const unsigned char*>(m.constData()),
                           static_cast<size_t>(m.size()), nullptr) == 1 &&
        EC_POINT_mul(group, product, nullptr, point, scalar, nullptr) == 1 &&
        EC_POINT_point2oct(group, product, POINT_CONVERSION_UNCOMPRESSED, buf, sizeof buf,
                           nullptr) == sizeof buf)
        out = QByteArray(reinterpret_cast<const char*>(buf), sizeof buf);
    BN_free(scalar);
    EC_POINT_free(product);
    EC_POINT_free(point);
    EC_GROUP_free(group);
    return out;
}

/// The client's half of a run: the PIN it typed, its MW-BIND key, and the
/// channel it is on as it sees it.
class PinClient
{
public:
    PinClient(const QString& typed, QByteArray spki, QByteArray channel)
        : m_W(PinBinding::pinScalar(typed))
        , m_A(PinBinding::shareA(m_W))
        , m_Key(std::move(spki))
        , m_Channel(std::move(channel))
    {}

    const QByteArray& key() const { return m_Key; }

    QJsonObject startBody() const
    {
        return QJsonObject{{"protocol", QString::fromLatin1(PinBinding::PROTOCOL)},
                           {"public_key", encoded(m_Key)},
                           {"pa", m_A ? encoded(m_A->element) : QString()}};
    }

    /// Its keys once the host's first answer @p start is in, from @p hostKey.
    std::optional<PinBinding::Keys> keysFor(const QJsonObject& start,
                                            const QByteArray& hostKey) const
    {
        if (!m_A) return std::nullopt;
        return PinBinding::finishA(m_W, *m_A, decoded(start.value(QStringLiteral("pb"))), m_Key,
                                   hostKey, PinBinding::associatedData(m_Channel));
    }

private:
    QByteArray m_W;
    std::optional<PinBinding::Share> m_A;
    QByteArray m_Key;
    QByteArray m_Channel;
};

QJsonObject finishBody(const QJsonObject& start, const QByteArray& confirm)
{
    return QJsonObject{{"exchange", start.value(QStringLiteral("exchange"))},
                       {"confirm", encoded(confirm)},
                       {"machine_name", "Living room TV"},
                       {"remember", true}};
}

/// A PIN that is not @p pin.
QString otherPin(const QString& pin)
{
    QString other = pin;
    other[0] = pin.at(0) == QLatin1Char('0') ? QLatin1Char('1') : QLatin1Char('0');
    return other;
}

bool isRoutesOwn404(const HttpResponse& reply)
{
    return reply.statusCode == 404 && !bodyOf(reply)
                                           .value(QStringLiteral("error"))
                                           .toString()
                                           .startsWith(QStringLiteral("Not Found"));
}

void pinProofRouteTests()
{
    SECTION("MW-PIN-v1 and MW-TLS-v1 — through their routes");

    QTemporaryDir tmp;
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/routes.json";
    AuthManager auth(&settings);
    GeoIpService geo(nullptr); // no lookup leaves the machine
    const QByteArray hostPem = PairingCrypto::generatePrivateKeyPem();
    const QByteArray hostKey = PairingCrypto::publicKeySpkiFromPrivatePem(hostPem);
    const QString hostId = PairingCrypto::keyId(hostKey);
    RestRouter router;
    registerPinProofRoutes(router, auth, geo, hostPem);

    // The TLS key this host's sockets serve, and the channel of a client that
    // sees it.
    const QByteArray served = newKey();
    const QByteArray seen = PinBinding::tlsChannel(served);
    const QString post = QStringLiteral("POST");
    const QString start = QStringLiteral("/api/auth/pin/start");
    const QString finish = QStringLiteral("/api/auth/pin/finish");
    const QString proof = QStringLiteral("/api/auth/tls-proof");

    // A run from start to finish: counted at its start and cleared by its proof,
    // the session made with the run's key bound to it, this host's confirmation
    // and identity in the answer. A second /finish of the same run finds nothing.
    {
        const QString pin = auth.generatePin();
        const QString peer = QStringLiteral("198.51.100.60");
        const PinClient tv(pin, newKey(), seen);
        const HttpResponse first =
            dispatch(router, requestFrom(peer, served), post, start, tv.startBody());
        CHECK_EQ(first.statusCode, 200);
        const QJsonObject s = bodyOf(first);
        CHECK_EQ(s.value(QStringLiteral("protocol")).toString(), QStringLiteral("MW-PIN-v1"));
        CHECK_EQ(decoded(s.value(QStringLiteral("host_public_key"))), hostKey);
        CHECK_EQ(s.value(QStringLiteral("host_id")).toString(), hostId);
        CHECK_EQ(s.value(QStringLiteral("tls_key")).toString(), keyText(served));
        CHECK_EQ(auth.failedAttemptCount(peer), 1);
        const std::optional<PinBinding::Keys> keys = tv.keysFor(s, hostKey);
        CHECK(keys.has_value());
        if (keys) {
            const QJsonObject body = finishBody(s, keys->confirmA);
            const HttpResponse done =
                dispatch(router, requestFrom(peer, served), post, finish, body);
            CHECK_EQ(done.statusCode, 200);
            const QJsonObject f = bodyOf(done);
            CHECK_EQ(decoded(f.value(QStringLiteral("confirm"))), keys->confirmB);
            CHECK_EQ(f.value(QStringLiteral("pairing_key")).toString(), QStringLiteral("bound"));
            CHECK_EQ(decoded(f.value(QStringLiteral("host_public_key"))), hostKey);
            CHECK_EQ(f.value(QStringLiteral("host_id")).toString(), hostId);
            CHECK(f.value(QStringLiteral("pin_regenerated")).toBool());
            const QString cookie = done.headers.value(QStringLiteral("Set-Cookie"));
            CHECK(cookie.startsWith(QStringLiteral("mw_session=")));
            const QString token = cookie.section(QLatin1Char(';'), 0, 0).mid(11);
            CHECK(auth.validateSession(token));
            CHECK_EQ(auth.sessionPairingKey(token), tv.key());
            CHECK_EQ(auth.failedAttemptCount(peer), 0);

            const HttpResponse again =
                dispatch(router, requestFrom(peer, served), post, finish, body);
            CHECK_EQ(again.statusCode, 410);
            CHECK_EQ(bodyOf(again).value(QStringLiteral("error")).toString(),
                     QStringLiteral("exchange_unknown"));
        }
        CHECK_EQ(dispatch(router, requestFrom(peer, served), post, finish,
                          finishBody(QJsonObject{{"exchange", "unknown"}}, QByteArray(32, '\1')))
                     .statusCode,
                 410);
    }

    // Only the address that began a run can finish it; it is still there for that one.
    {
        const QString pin = auth.generatePin();
        const QString own = QStringLiteral("198.51.100.61");
        const PinClient tv(pin, newKey(), seen);
        const QJsonObject s =
            bodyOf(dispatch(router, requestFrom(own, served), post, start, tv.startBody()));
        const std::optional<PinBinding::Keys> keys = tv.keysFor(s, hostKey);
        CHECK(keys.has_value());
        if (keys) {
            const QJsonObject body = finishBody(s, keys->confirmA);
            CHECK_EQ(dispatch(router, requestFrom(QStringLiteral("198.51.100.62"), served), post,
                              finish, body)
                         .statusCode,
                     410);
            CHECK_EQ(dispatch(router, requestFrom(own, served), post, finish, body).statusCode,
                     200);
        }
    }

    // A wrong PIN, and the right one whose confirmation changed by one bit on its
    // way: 401 as a wrong PIN, counted once, and the run spent.
    {
        const QString pin = auth.generatePin();
        const QString peer = QStringLiteral("198.51.100.63");
        const PinClient wrong(otherPin(pin), newKey(), seen);
        const QJsonObject s =
            bodyOf(dispatch(router, requestFrom(peer, served), post, start, wrong.startBody()));
        const std::optional<PinBinding::Keys> keys = wrong.keysFor(s, hostKey);
        CHECK(keys.has_value());
        if (keys) {
            const HttpResponse refused = dispatch(router, requestFrom(peer, served), post, finish,
                                                  finishBody(s, keys->confirmA));
            CHECK_EQ(refused.statusCode, 401);
            CHECK_EQ(bodyOf(refused).value(QStringLiteral("error")).toString(),
                     QStringLiteral("invalid_pin"));
            CHECK_EQ(bodyOf(refused).value(QStringLiteral("remaining")).toInt(), 2);
        }
        CHECK_EQ(auth.failedAttemptCount(peer), 1);

        const QString flippedPeer = QStringLiteral("198.51.100.64");
        const PinClient tv(pin, newKey(), seen);
        const QJsonObject s2 =
            bodyOf(dispatch(router, requestFrom(flippedPeer, served), post, start, tv.startBody()));
        const std::optional<PinBinding::Keys> keys2 = tv.keysFor(s2, hostKey);
        CHECK(keys2.has_value());
        if (keys2) {
            QByteArray flipped = keys2->confirmA;
            flipped[0] = static_cast<char>(flipped.at(0) ^ 1);
            CHECK_EQ(dispatch(router, requestFrom(flippedPeer, served), post, finish,
                              finishBody(s2, flipped))
                         .statusCode,
                     401);
            CHECK_EQ(dispatch(router, requestFrom(flippedPeer, served), post, finish,
                              finishBody(s2, keys2->confirmA))
                         .statusCode,
                     410);
        }
        CHECK_EQ(auth.failedAttemptCount(flippedPeer), 1);
    }

    // Elements replaced on their way by other points of the group, under the right
    // PIN: pA on its way in, pB on its way out. Neither run proves.
    {
        const QString pin = auth.generatePin();
        const QByteArray w = PinBinding::pinScalar(pin);
        const std::optional<PinBinding::Share> otherA = PinBinding::shareA(w);
        const std::optional<PinBinding::Share> otherB = PinBinding::shareB(w);
        CHECK(otherA.has_value() && otherB.has_value());
        if (otherA && otherB) {
            const QString inPeer = QStringLiteral("198.51.100.65");
            const PinClient tv(pin, newKey(), seen);
            QJsonObject replaced = tv.startBody();
            replaced[QStringLiteral("pa")] = encoded(otherA->element);
            const QJsonObject s =
                bodyOf(dispatch(router, requestFrom(inPeer, served), post, start, replaced));
            const std::optional<PinBinding::Keys> keys = tv.keysFor(s, hostKey);
            CHECK(keys && dispatch(router, requestFrom(inPeer, served), post, finish,
                                   finishBody(s, keys->confirmA))
                                  .statusCode == 401);

            const QString outPeer = QStringLiteral("198.51.100.66");
            const PinClient tv2(pin, newKey(), seen);
            QJsonObject s2 = bodyOf(
                dispatch(router, requestFrom(outPeer, served), post, start, tv2.startBody()));
            s2[QStringLiteral("pb")] = encoded(otherB->element);
            const std::optional<PinBinding::Keys> keys2 = tv2.keysFor(s2, hostKey);
            CHECK(keys2 && dispatch(router, requestFrom(outPeer, served), post, finish,
                                    finishBody(s2, keys2->confirmA))
                                   .statusCode == 401);
        }
    }

    // The right PIN through something that ends the client's TLS with a key of its
    // own: /start names the key this socket served, which the client can compare
    // with the one it sees, and the run does not prove.
    {
        const QString pin = auth.generatePin();
        const QString peer = QStringLiteral("198.51.100.67");
        const QByteArray between = newKey();
        const PinClient tv(pin, newKey(), PinBinding::tlsChannel(between));
        const QJsonObject s =
            bodyOf(dispatch(router, requestFrom(peer, served), post, start, tv.startBody()));
        CHECK_EQ(s.value(QStringLiteral("tls_key")).toString(), keyText(served));
        CHECK(s.value(QStringLiteral("tls_key")).toString() != keyText(between));
        const std::optional<PinBinding::Keys> keys = tv.keysFor(s, hostKey);
        CHECK(keys && dispatch(router, requestFrom(peer, served), post, finish,
                               finishBody(s, keys->confirmA))
                              .statusCode == 401);
    }

    // The PIN changed between the two rounds: shown anew, or spent by another run.
    {
        const QString pin = auth.generatePin();
        const QString peer = QStringLiteral("198.51.100.68");
        const PinClient tv(pin, newKey(), seen);
        const QJsonObject s =
            bodyOf(dispatch(router, requestFrom(peer, served), post, start, tv.startBody()));
        const std::optional<PinBinding::Keys> keys = tv.keysFor(s, hostKey);
        // Another PIN: one draw in a million gives the same one back.
        while (auth.generatePin() == pin) {}
        CHECK(keys && dispatch(router, requestFrom(peer, served), post, finish,
                               finishBody(s, keys->confirmA))
                              .statusCode == 401);

        const QString shown = auth.generatePin();
        const QString one = QStringLiteral("198.51.100.69");
        const QString two = QStringLiteral("198.51.100.70");
        const PinClient first(shown, newKey(), seen);
        const PinClient second(shown, newKey(), seen);
        const QJsonObject s1 =
            bodyOf(dispatch(router, requestFrom(one, served), post, start, first.startBody()));
        const QJsonObject s2 =
            bodyOf(dispatch(router, requestFrom(two, served), post, start, second.startBody()));
        const std::optional<PinBinding::Keys> k1 = first.keysFor(s1, hostKey);
        const std::optional<PinBinding::Keys> k2 = second.keysFor(s2, hostKey);
        CHECK(k1.has_value() && k2.has_value());
        if (k1 && k2) {
            CHECK_EQ(dispatch(router, requestFrom(two, served), post, finish,
                              finishBody(s2, k2->confirmA))
                         .statusCode,
                     200);
            // The PIN made after it: one draw in a million is the same one.
            if (auth.currentPin() != shown)
                CHECK_EQ(dispatch(router, requestFrom(one, served), post, finish,
                                  finishBody(s1, k1->confirmA))
                             .statusCode,
                         401);
        }
    }

    // The share w·M under the right PIN: answered in the same shape as any run,
    // counted the same, and never proved.
    {
        const QString pin = auth.generatePin();
        const QString probePeer = QStringLiteral("198.51.100.71");
        const PinClient tv(pin, newKey(), seen);
        QJsonObject probe = tv.startBody();
        probe[QStringLiteral("pa")] = encoded(wTimesM(PinBinding::pinScalar(pin)));
        const HttpResponse odd =
            dispatch(router, requestFrom(probePeer, served), post, start, probe);
        const HttpResponse plain =
            dispatch(router, requestFrom(QStringLiteral("198.51.100.72"), served), post, start,
                     tv.startBody());
        CHECK_EQ(odd.statusCode, 200);
        CHECK_EQ(plain.statusCode, 200);
        const QJsonObject a = bodyOf(odd);
        const QJsonObject b = bodyOf(plain);
        CHECK_EQ(a.keys(), b.keys());
        CHECK_EQ(decoded(a.value(QStringLiteral("pb"))).size(),
                 decoded(b.value(QStringLiteral("pb"))).size());
        CHECK_EQ(a.value(QStringLiteral("exchange")).toString().size(),
                 b.value(QStringLiteral("exchange")).toString().size());
        CHECK_EQ(a.value(QStringLiteral("tls_key")).toString(), keyText(served));
        CHECK_EQ(auth.failedAttemptCount(probePeer), 1);
        CHECK_EQ(dispatch(router, requestFrom(probePeer, served), post, finish,
                          finishBody(a, QByteArray(32, '\0')))
                     .statusCode,
                 401);
        CHECK_EQ(auth.failedAttemptCount(probePeer), 1);
    }

    // Where the client does not see the key this socket served, both routes
    // answer 404 as an older host does, and count nothing: plain HTTP, a peer on
    // this machine, a request a proxy forwarded.
    {
        const PinClient tv(auth.generatePin(), newKey(), seen);
        const QString direct = QStringLiteral("198.51.100.73");
        const QString forwarded = QStringLiteral("198.51.100.74");
        QList<HttpRequest> unseen;
        unseen << requestFrom(direct, QByteArray())
               << requestFrom(QStringLiteral("127.0.0.1"), served)
               << requestFrom(QStringLiteral("::1"), served)
               << requestFrom(QStringLiteral("::ffff:127.0.0.1"), served);
        for (const char* name : {"cf-connecting-ip", "x-forwarded-for", "forwarded", "x-real-ip"}) {
            HttpRequest req = requestFrom(forwarded, served);
            req.headers.insert(QString::fromLatin1(name), QStringLiteral("203.0.113.9"));
            unseen << req;
        }
        for (const HttpRequest& req : unseen) {
            CHECK(isRoutesOwn404(dispatch(router, req, post, start, tv.startBody())));
            HttpRequest asked = req;
            asked.queryParams.insert(QStringLiteral("nonce"),
                                     urlText(PairingCrypto::generateNonce()));
            CHECK(isRoutesOwn404(dispatch(router, asked, QStringLiteral("GET"), proof)));
        }
        CHECK_EQ(auth.failedAttemptCount(direct), 0);
        CHECK_EQ(auth.failedAttemptCount(forwarded), 0);
        CHECK_EQ(auth.failedAttemptCount(QStringLiteral("203.0.113.9")), 0);
    }

    // Through the tunnel: the line's channel, the key its hello named and no other,
    // no TLS key in the answer, and no TLS proof.
    {
        const QString pin = auth.generatePin();
        const QString peer = QStringLiteral("198.51.100.75");
        const PinClient tv(pin, newKey(), PinBinding::lineChannel(hostKey));
        HttpRequest line = requestFrom(peer, QByteArray());
        line.viaTunnel = true;
        line.tunnelKey = newKey();
        CHECK_EQ(dispatch(router, line, post, start, tv.startBody()).statusCode, 400);
        CHECK_EQ(auth.failedAttemptCount(peer), 0);

        line.tunnelKey = tv.key();
        const HttpResponse first = dispatch(router, line, post, start, tv.startBody());
        CHECK_EQ(first.statusCode, 200);
        const QJsonObject s = bodyOf(first);
        CHECK(!s.contains(QStringLiteral("tls_key")));
        const std::optional<PinBinding::Keys> keys = tv.keysFor(s, hostKey);
        CHECK(keys &&
              dispatch(router, line, post, finish, finishBody(s, keys->confirmA)).statusCode ==
                  200);

        HttpRequest asked = line;
        asked.queryParams.insert(QStringLiteral("nonce"), urlText(PairingCrypto::generateNonce()));
        CHECK(isRoutesOwn404(dispatch(router, asked, QStringLiteral("GET"), proof)));
    }

    // What /start refuses up front is refused before anything is counted.
    {
        const PinClient tv(auth.generatePin(), newKey(), seen);
        const QString peer = QStringLiteral("198.51.100.79");
        QJsonObject unknown = tv.startBody();
        unknown[QStringLiteral("protocol")] = QStringLiteral("MW-PIN-v0");
        QJsonObject noKey = tv.startBody();
        noKey[QStringLiteral("public_key")] = encoded(QByteArray("not a key"));
        QJsonObject noPoint = tv.startBody();
        noPoint[QStringLiteral("pa")] = encoded(QByteArray(65, '\x04'));
        for (const QJsonObject& body : {unknown, noKey, noPoint})
            CHECK_EQ(dispatch(router, requestFrom(peer, served), post, start, body).statusCode,
                     400);
        CHECK_EQ(auth.failedAttemptCount(peer), 0);
    }

    // GET /api/auth/tls-proof: the key this socket served, signed by the host key
    // over the client's nonce; a nonce of 16 to 64 bytes; ten a minute per caller.
    {
        const QString peer = QStringLiteral("198.51.100.76");
        const auto ask = [&](const QString& from, const QByteArray& nonce) {
            HttpRequest req = requestFrom(from, served);
            req.queryParams.insert(QStringLiteral("nonce"), urlText(nonce));
            return dispatch(router, req, QStringLiteral("GET"), proof);
        };
        CHECK_EQ(ask(peer, QByteArray()).statusCode, 400);
        CHECK_EQ(ask(peer, QByteArray(15, '\1')).statusCode, 400);
        CHECK_EQ(ask(peer, QByteArray(65, '\1')).statusCode, 400);

        const QByteArray nonce = PairingCrypto::generateNonce();
        const HttpResponse given = ask(peer, nonce);
        CHECK_EQ(given.statusCode, 200);
        const QJsonObject p = bodyOf(given);
        CHECK_EQ(p.value(QStringLiteral("protocol")).toString(), QStringLiteral("MW-TLS-v1"));
        CHECK_EQ(p.value(QStringLiteral("host_id")).toString(), hostId);
        CHECK_EQ(decoded(p.value(QStringLiteral("host_public_key"))), hostKey);
        CHECK_EQ(p.value(QStringLiteral("tls_key")).toString(), keyText(served));
        const QByteArray signedBytes = PairingCrypto::tlsProofInput(
            hostId, nonce, QCryptographicHash::hash(served, QCryptographicHash::Sha256));
        CHECK(PairingCrypto::verify(hostKey, signedBytes, decoded(p.value(QStringLiteral("sig")))));
        CHECK_EQ(ask(peer, QByteArray(64, '\1')).statusCode, 200);

        for (int i = 0; i < 8; ++i)
            CHECK_EQ(ask(peer, PairingCrypto::generateNonce()).statusCode, 200);
        CHECK_EQ(ask(peer, PairingCrypto::generateNonce()).statusCode, 429);
        CHECK_EQ(ask(QStringLiteral("198.51.100.80"), PairingCrypto::generateNonce()).statusCode,
                 200);
    }

    // A full table of runs takes no new caller's run, and counts nothing for it;
    // every run it holds stays, and the first one still proves.
    {
        RestRouter full;
        registerPinProofRoutes(full, auth, geo, hostPem);
        const QString pin = auth.generatePin();
        const QString first = QStringLiteral("198.51.100.77");
        const PinClient tv(pin, newKey(), seen);
        const QJsonObject kept =
            bodyOf(dispatch(full, requestFrom(first, served), post, start, tv.startBody()));
        // Other callers, three runs each (a fourth would find its caller locked
        // out), up to the size of the table.
        const QJsonObject other = PinClient(otherPin(pin), newKey(), seen).startBody();
        int runs = 1;
        for (int caller = 1; runs < PinBinding::Exchanges::MAX_RUNS; ++caller) {
            const QString peer = QStringLiteral("198.18.0.%1").arg(caller);
            for (int n = 0; n < 3 && runs < PinBinding::Exchanges::MAX_RUNS; ++n, ++runs)
                CHECK_EQ(dispatch(full, requestFrom(peer, served), post, start, other).statusCode,
                         200);
        }
        const QString late = QStringLiteral("198.51.100.78");
        const PinClient next(pin, newKey(), seen);
        CHECK_EQ(
            dispatch(full, requestFrom(late, served), post, start, next.startBody()).statusCode,
            503);
        CHECK_EQ(auth.failedAttemptCount(late), 0);

        const std::optional<PinBinding::Keys> keys = tv.keysFor(kept, hostKey);
        CHECK(keys && dispatch(full, requestFrom(first, served), post, finish,
                               finishBody(kept, keys->confirmA))
                              .statusCode == 200);
        // That run ended: room for one more.
        CHECK_EQ(
            dispatch(full, requestFrom(late, served), post, start, next.startBody()).statusCode,
            200);
    }
}

// ── How long a remembered device stays signed in (issue #60) ────────────────

/// settings.json holding @p obj and nothing else.
void writeSettings(AppSettings& settings, const QJsonObject& obj)
{
    QFile file(settings.m_FilePath);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

/// Every session in sessions.json last seen @p daysAgo days ago, or only the
/// one whose machine name is @p only.
void ageSessions(qint64 daysAgo, const QString& only = QString())
{
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/sessions.json";
    QFile in(path);
    CHECK(in.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    in.close();
    const qint64 then = QDateTime::currentSecsSinceEpoch() - daysAgo * 24 * 3600;
    QJsonArray aged;
    for (const QJsonValue& v : root.value("sessions").toArray()) {
        QJsonObject s = v.toObject();
        if (only.isEmpty() || s.value("machine_name").toString() == only) {
            s["last_seen"] = static_cast<double>(then);
            s["created_at"] = static_cast<double>(then);
        }
        aged.append(s);
    }
    root["sessions"] = aged;
    QFile out(path);
    CHECK(out.open(QIODevice::WriteOnly | QIODevice::Truncate));
    out.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

void rememberMeDaysTests()
{
    SECTION("remember_me_days — how long a remembered device stays signed in");

    QTemporaryDir tmp;
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/settings.json";

    // Read from the file: a whole number from 1 to 3650, anything else is 90.
    CHECK_EQ(settings.rememberMeDays(), 90); // no file at all
    const QList<std::pair<QJsonValue, int>> cases = {
        {QJsonValue(365), 365},
        {QJsonValue(3650), 3650},
        {QJsonValue(1), 1},
        {QJsonValue(365.0), 365}, // JSON has no integer type
        {QJsonValue(3651), 90},
        {QJsonValue(0), 90},
        {QJsonValue(-30), 90},
        {QJsonValue(365.5), 90},
        {QJsonValue(QStringLiteral("365")), 90},
        {QJsonValue(true), 90},
        {QJsonValue(QJsonValue::Null), 90},
        {QJsonValue(QJsonArray{365}), 90},
        {QJsonValue(1e300), 90},
    };
    for (const auto& [value, expected] : cases) {
        writeSettings(settings, {{"remember_me_days", value}});
        CHECK_EQ(settings.rememberMeDays(), expected);
    }

    // The acknowledged value: a whole number in range, or 0.
    writeSettings(settings, {});
    CHECK_EQ(settings.rememberMeDaysAcknowledged(), 0);
    writeSettings(settings, {{"remember_me_days_acknowledged", QStringLiteral("365")}});
    CHECK_EQ(settings.rememberMeDaysAcknowledged(), 0);
    writeSettings(settings, {{"remember_me_days", 365}, {"other_key", QStringLiteral("kept")}});
    settings.setRememberMeDaysAcknowledged(365);
    CHECK_EQ(settings.rememberMeDaysAcknowledged(), 365);
    CHECK_EQ(settings.rememberMeDays(), 365);
    CHECK_EQ(settings.readAll().value("other_key").toString(), QStringLiteral("kept"));

    // POST /api/admin/settings: the lifetime itself is never written by a page,
    // and a refused body writes nothing, the acknowledgment included.
    {
        writeSettings(settings, {{"remember_me_days", 365}});
        QJsonObject reply;
        QString error;
        CHECK(writeRememberMe({{"remember_me_days", 3650}}, 365, settings, reply, &error) ==
              RememberMeWrite::Refused);
        CHECK(!error.isEmpty());
        CHECK(writeRememberMe({{"remember_me_days", 3650}, {"remember_me_days_acknowledged", 365}},
                              365, settings, reply, &error) == RememberMeWrite::Refused);
        CHECK_EQ(settings.rememberMeDays(), 365);
        CHECK_EQ(settings.rememberMeDaysAcknowledged(), 0);
        CHECK(!settings.readAll().contains("remember_me_days_acknowledged"));

        // Only the value in force is acknowledged.
        const QList<QJsonValue> stale = {QJsonValue(730), QJsonValue(QStringLiteral("365")),
                                         QJsonValue(365.5), QJsonValue(QJsonValue::Null)};
        for (const QJsonValue& value : stale) {
            CHECK(writeRememberMe({{"remember_me_days_acknowledged", value}}, 365, settings, reply,
                                  &error) == RememberMeWrite::Refused);
        }
        CHECK_EQ(settings.rememberMeDaysAcknowledged(), 0);
        CHECK(reply.isEmpty());

        CHECK(writeRememberMe({{"pair_request_internet", true}}, 365, settings, reply, &error) ==
              RememberMeWrite::None);
        CHECK(writeRememberMe({{"remember_me_days_acknowledged", 365}}, 365, settings, reply,
                              &error) == RememberMeWrite::Written);
        CHECK_EQ(settings.rememberMeDaysAcknowledged(), 365);
        CHECK_EQ(reply.value("remember_me_days_acknowledged").toInt(), 365);
        CHECK_EQ(settings.rememberMeDays(), 365); // the lifetime is untouched
    }

    // The cookie: a Max-Age only when remembered, of the session's own length.
    CHECK(!sessionCookie("t", false, 31536000).contains("Max-Age"));
    CHECK(sessionCookie("t", true, 31536000).endsWith("; Max-Age=31536000"));
    CHECK(sessionCookie("t", true, 315360000).endsWith("; Max-Age=315360000"));

    // The lifetimes AuthManager applies, read at startup.
    const qint64 day = 24 * 3600;
    writeSettings(settings, {{"remember_me_days", 365}});
    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        CHECK_EQ(auth.rememberMeDays(), 365);

        const QString phone = auth.createSession(SessionOrigin::Unknown, "192.168.1.30", "Phone");
        const QString kiosk =
            auth.createSession(SessionOrigin::Unknown, "198.51.100.30", "Kiosk", false, true);
        const QString hostHere =
            auth.createSession(SessionOrigin::Unknown, "127.0.0.1", "Host here", true);
        const QString hostLine = auth.createSession(SessionOrigin::Unknown, "192.168.1.31",
                                                    "Host line", true, false, /*viaTunnel=*/true);
        CHECK_EQ(auth.cookieMaxAgeFor(phone), 365 * day);
        CHECK_EQ(auth.cookieMaxAgeFor(kiosk), 8 * 3600);     // "Remember me" declined
        CHECK_EQ(auth.cookieMaxAgeFor(hostHere), 365 * day); // a host session on a socket
        CHECK_EQ(auth.cookieMaxAgeFor(hostLine), 90 * day);  // through the tunnel: never more
        CHECK_EQ(auth.cookieMaxAgeFor("bogus-token"), 365 * day);

        // Unlocked with the remote admin password: never more than 90 days
        // either, from the moment it administers. The password is set so the
        // flag survives the reload below, as it does on a real server.
        CHECK(auth.setAdminPassword("correct-horse"));
        const QString admin =
            auth.createSession(SessionOrigin::Unknown, "192.168.1.34", "Admin PC");
        CHECK_EQ(auth.cookieMaxAgeFor(admin), 365 * day);
        CHECK(auth.promoteSessionToAdmin(admin));
        CHECK_EQ(auth.cookieMaxAgeFor(admin), 90 * day);

        // Sliding, on disk: 200 days without a visit is within 365 for the
        // phone, beyond 90 for the sessions that administer from elsewhere.
        auth.saveSessions();
        ageSessions(200);
        auth.loadSessions();
        CHECK(auth.validateSession(phone));
        CHECK(auth.validateSession(hostHere));
        CHECK(!auth.validateSession(hostLine));
        CHECK(!auth.validateSession(admin));
        CHECK(!auth.validateSession(kiosk)); // never on disk
        auth.saveSessions();
    }

    // Shortened again: the next start drops what the new value no longer
    // covers, and keeps the rest.
    writeSettings(settings, {});
    {
        AuthManager auth(&settings);
        CHECK_EQ(auth.rememberMeDays(), 90);
        CHECK_EQ(auth.activeSessionCount(), 0); // the 200-day-old ones are gone
        const QString recent = auth.createSession(SessionOrigin::Unknown, "192.168.1.32", "Recent");
        const QString old = auth.createSession(SessionOrigin::Unknown, "192.168.1.33", "Old");
        auth.saveSessions();
        ageSessions(91, QStringLiteral("Old"));
        auth.loadSessions();
        CHECK(auth.validateSession(recent));
        CHECK(!auth.validateSession(old));
        CHECK_EQ(auth.cookieMaxAgeFor(recent), 90 * day);
        auth.destroyAllSessions();
        auth.saveSessions();
    }
}

// ── A TV on plain HTTP (issue #42) ──────────────────────────────────────────

/// Applies @p edit to the session in sessions.json whose machine name is
/// @p machine.
void editStoredSession(const QString& machine, const std::function<void(QJsonObject&)>& edit)
{
    const QString path =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/sessions.json";
    QFile in(path);
    CHECK(in.open(QIODevice::ReadOnly));
    QJsonObject root = QJsonDocument::fromJson(in.readAll()).object();
    in.close();
    QJsonArray out;
    for (const QJsonValue& v : root.value("sessions").toArray()) {
        QJsonObject s = v.toObject();
        if (s.value("machine_name").toString() == machine) edit(s);
        out.append(s);
    }
    root["sessions"] = out;
    QFile file(path);
    CHECK(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
}

/// The session @p machine approved @p createdDaysAgo days ago and last seen
/// @p seenDaysAgo days ago, on disk.
void setStoredTimes(const QString& machine, qint64 createdDaysAgo, qint64 seenDaysAgo)
{
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    editStoredSession(machine, [&](QJsonObject& s) {
        s["created_at"] = static_cast<double>(now - createdDaysAgo * 24 * 3600);
        s["last_seen"] = static_cast<double>(now - seenDaysAgo * 24 * 3600);
    });
}

void lanHttpSessionTests()
{
    SECTION("lan_http_clients — a TV's session, good on plain HTTP only");

    QTemporaryDir tmp;
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/settings.json";

    // The list as written: strings only, anything else reads as none.
    writeSettings(settings, {});
    CHECK(settings.lanHttpClients().isEmpty());
    writeSettings(settings, {{"lan_http_clients", QJsonArray{"192.168.1.50", 3232235826.0, true,
                                                             QJsonValue(), "192.168.1.51"}}});
    CHECK_EQ(settings.lanHttpClients(), QStringList({"192.168.1.50", "192.168.1.51"}));
    writeSettings(settings, {{"lan_http_clients", QStringLiteral("192.168.1.50")}});
    CHECK(settings.lanHttpClients().isEmpty());

    const qint64 day = 24 * 3600;
    const QString tvName = QStringLiteral("Browser at 192.168.1.50");
    writeSettings(settings, {}); // remember_me_days: 90
    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        const QString phone = auth.createSession(SessionOrigin::Unknown, "192.168.1.30", "Phone");
        const QString approver = auth.sessionIdForToken(phone);
        const QString tv = auth.createLanHttpSession("::ffff:192.168.1.50", tvName, approver);
        CHECK(!tv.isEmpty());
        // Only for a private IPv4.
        CHECK(auth.createLanHttpSession("fe80::1", "x", {}).isEmpty());
        CHECK(auth.createLanHttpSession("8.8.8.8", "x", {}).isEmpty());
        CHECK(auth.createLanHttpSession("tv.local", "x", {}).isEmpty());

        // Good on plain HTTP, from the address it is bound to only.
        CHECK(auth.validateLanHttpSession(tv, "192.168.1.50"));
        CHECK(auth.validateLanHttpSession(tv, "::ffff:192.168.1.50"));
        CHECK(!auth.validateLanHttpSession(tv, "192.168.1.51"));
        CHECK(!auth.validateLanHttpSession(tv, ""));
        CHECK(!auth.validateLanHttpSession("bogus-token", "192.168.1.50"));
        CHECK(!auth.validateLanHttpSession(QString(), "192.168.1.50"));
        // An HTTPS session is not a TV's, even from its own address.
        CHECK(!auth.validateLanHttpSession(phone, "192.168.1.30"));
        CHECK(auth.lanHttpSessionId(phone, "192.168.1.30").isEmpty());
        CHECK_EQ(auth.lanHttpSessionId(tv, "192.168.1.50"), auth.sessionIdForToken(tv));

        // Never an HTTPS session, never more than a TV: read off the network
        // and sent as mw_session, it opens nothing.
        CHECK(!auth.validateSession(tv));
        CHECK(!auth.isHostSession(tv));
        CHECK(!auth.isTunnelHostSession(tv));
        CHECK(!auth.isAdminSession(tv));
        CHECK(!auth.isEphemeralSession(tv));
        CHECK(auth.sessionMachineName(tv).isEmpty());
        CHECK(!auth.promoteSessionToAdmin(tv));
        CHECK(!auth.bindSessionKey(tv, newKey()));
        CHECK(!auth.updateSessionAddress(tv, "192.168.1.99"));
        CHECK(auth.validateLanHttpSession(tv, "192.168.1.50")); // it has not moved
        CHECK(auth.sessionPairingKey(tv).isEmpty());

        // The address it is bound to, asked from anywhere: what tells "another
        // address" from "ran out" (LanHttp::Key).
        CHECK_EQ(auth.lanHttpBoundIp(tv), QStringLiteral("192.168.1.50"));
        CHECK(auth.lanHttpBoundIp(phone).isEmpty());
        CHECK(auth.lanHttpBoundIp("bogus-token").isEmpty());

        // Its cookie lasts 30 days; an HTTPS token has no such cookie.
        CHECK_EQ(auth.lanHttpCookieMaxAge(tv), 30 * day);
        CHECK_EQ(auth.lanHttpCookieMaxAge(phone), qint64(0));
        CHECK_EQ(auth.lanHttpCookieMaxAge("bogus-token"), qint64(0));

        // Listed among the devices, as a TV.
        int listed = 0;
        for (const SessionInfo& s : auth.sessions()) {
            if (!s.lanHttp) continue;
            listed++;
            CHECK_EQ(s.boundIp, QStringLiteral("192.168.1.50"));
            CHECK_EQ(s.ip, QStringLiteral("192.168.1.50"));
            CHECK_EQ(s.machineName, tvName);
            CHECK_EQ(s.approvedBy, approver);
            CHECK(!s.isHost && !s.isAdmin && !s.viaTunnel && !s.ephemeral);
            CHECK(s.toJson().value("lan_http").toBool());
            CHECK_EQ(s.toJson().value("bound_ip").toString(), QStringLiteral("192.168.1.50"));
        }
        CHECK_EQ(listed, 1);

        // Sliding 30 days, on disk.
        auth.saveSessions();
        setStoredTimes(tvName, 29, 29);
        auth.loadSessions();
        CHECK(auth.validateLanHttpSession(tv, "192.168.1.50"));
        setStoredTimes(tvName, 31, 31);
        auth.loadSessions();
        CHECK(!auth.validateLanHttpSession(tv, "192.168.1.50"));
        auth.destroyAllSessions();
    }

    // Approved again after remember_me_days, however often it came back (LH3).
    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        const QString tv = auth.createLanHttpSession("192.168.1.50", tvName, {});
        auth.saveSessions();
        setStoredTimes(tvName, 89, 1);
        auth.loadSessions();
        CHECK(auth.validateLanHttpSession(tv, "192.168.1.50"));
        // What is left of the 90 days, not 30.
        const qint64 left = auth.lanHttpCookieMaxAge(tv);
        CHECK(left <= day && left > day - 120);
        setStoredTimes(tvName, 91, 1);
        auth.loadSessions();
        CHECK(!auth.validateLanHttpSession(tv, "192.168.1.50"));
        auth.destroyAllSessions();
        auth.saveSessions();
    }
    writeSettings(settings, {{"remember_me_days", 365}});
    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        const QString tv = auth.createLanHttpSession("192.168.1.50", tvName, {});
        auth.saveSessions();
        setStoredTimes(tvName, 200, 1);
        auth.loadSessions();
        CHECK(auth.validateLanHttpSession(tv, "192.168.1.50"));
        CHECK_EQ(auth.lanHttpCookieMaxAge(tv), 30 * day); // sliding, the shorter one
        setStoredTimes(tvName, 366, 1);
        auth.loadSessions();
        CHECK(!auth.validateLanHttpSession(tv, "192.168.1.50"));
        auth.destroyAllSessions();
        auth.saveSessions();
    }

    // A hand-edited file never makes a TV more than a TV.
    writeSettings(settings, {});
    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        const QString tv = auth.createLanHttpSession("192.168.1.50", tvName, {});
        const QString other = auth.createLanHttpSession("192.168.1.51", "Other TV", {});
        auth.saveSessions();
        editStoredSession(tvName, [](QJsonObject& s) {
            s["is_host"] = true;
            s["is_admin"] = true;
            s["via_tunnel"] = true;
        });
        editStoredSession("Other TV", [](QJsonObject& s) { s["bound_ip"] = "8.8.8.8"; });
        auth.loadSessions();
        CHECK(auth.validateLanHttpSession(tv, "192.168.1.50"));
        CHECK(!auth.validateSession(tv));
        CHECK(!auth.isHostSession(tv));
        CHECK(!auth.isAdminSession(tv));
        for (const SessionInfo& s : auth.sessions())
            CHECK(!s.lanHttp || (!s.isHost && !s.isAdmin && !s.viaTunnel));
        // Bound to an address that is not private: dropped.
        CHECK(!auth.validateLanHttpSession(other, "192.168.1.51"));
        CHECK(!auth.validateLanHttpSession(other, "8.8.8.8"));
        CHECK_EQ(auth.activeSessionCount(), 1);
        auth.destroyAllSessions();
        auth.saveSessions();
    }

    // Revoked with the session that approved it, and on its own.
    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        const QString phone = auth.createSession(SessionOrigin::Unknown, "192.168.1.30", "Phone");
        const QString tv =
            auth.createLanHttpSession("192.168.1.50", tvName, auth.sessionIdForToken(phone));
        auth.destroySession(auth.sessionIdForToken(phone));
        CHECK(!auth.validateLanHttpSession(tv, "192.168.1.50"));

        const QString again = auth.createLanHttpSession("192.168.1.50", tvName, {});
        CHECK(auth.validateLanHttpSession(again, "192.168.1.50"));
        auth.destroySession(auth.sessionIdForToken(again));
        CHECK(!auth.validateLanHttpSession(again, "192.168.1.50"));
        auth.destroyAllSessions();
        auth.saveSessions();
    }
}

// ── The hourly purge: which ends a stream, which only loses its row ─────────

void purgeTests()
{
    SECTION("purgeExpiredSessions — a fixed bound ends the stream, a sliding one does not");

    QTemporaryDir tmp;
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/settings.json";
    writeSettings(settings, {}); // remember_me_days: 90

    const qint64 day = 24 * 3600;
    AuthManager auth(&settings);
    auth.destroyAllSessions();
    // Whether sessions.json still holds the session @p id.
    const auto onDisk = [](const QString& id) {
        QFile in(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                 "/sessions.json");
        if (!in.open(QIODevice::ReadOnly)) return false;
        for (const QJsonValue& v :
             QJsonDocument::fromJson(in.readAll()).object().value("sessions").toArray())
            if (v.toObject().value("token").toString() == id) return true;
        return false;
    };
    // A stream is stopped only once the file says the session is gone: a
    // server killed meanwhile does not bring it back (security review, P-a).
    QStringList revoked;
    QStringList stillOnDisk;
    QObject::connect(&auth, &AuthManager::streamingSessionRevoked,
                     [&revoked, &stillOnDisk, &onDisk](const QString& sessionId) {
                         revoked.append(sessionId);
                         if (onDisk(sessionId)) stillOnDisk.append(sessionId);
                     });
    int changed = 0;
    QObject::connect(&auth, &AuthManager::sessionsChanged, [&changed]() { changed++; });
    const qint64 now = QDateTime::currentSecsSinceEpoch();

    // A TV approved 91 days ago, seen yesterday, streaming: the approval has
    // run out however often it came back. Its stream ends with it.
    const QString tv = auth.createLanHttpSession("192.168.1.50", "TV approved long ago", {});
    const QString tvId = auth.sessionIdForToken(tv);
    auth.setSessionStreaming(tv, true);
    auth.setSessionTimesForTesting(tvId, now - 91 * day, now - day);

    // A phone not seen for 91 days, streaming: only an absence of visits,
    // which a running stream never counts as one. Its row goes, its stream
    // runs on (no signal), as before (security review, 20/11, O3).
    const QString phone = auth.createSession(SessionOrigin::Unknown, "192.168.1.30", "Phone away");
    const QString phoneId = auth.sessionIdForToken(phone);
    auth.setSessionStreaming(phone, true);
    auth.setSessionTimesForTesting(phoneId, now - 120 * day, now - 91 * day);

    // A TV the phone let in, fresh: kept, the administrator's from now on.
    const QString kept = auth.createLanHttpSession("192.168.1.51", "TV kept", phoneId);
    const QString keptId = auth.sessionIdForToken(kept);

    // A device the old TV let in, itself past its sliding bound: the TV's end
    // does not take it along as a revocation would, so no signal for it.
    const QString let =
        auth.createSession(SessionOrigin::Unknown, "192.168.1.31", "Let in by the TV");
    const QString letId = auth.sessionIdForToken(let);
    CHECK(auth.setSessionApprover(let, tvId));
    auth.setSessionTimesForTesting(letId, now - 100 * day, now - 91 * day);

    // A second old TV, let in by the first: each ends once, by its own token.
    const QString second = auth.createLanHttpSession("192.168.1.52", "TV let in by a TV", tvId);
    const QString secondId = auth.sessionIdForToken(second);
    auth.setSessionTimesForTesting(secondId, now - 91 * day, now - day);

    // One that has not run out at all.
    const QString fresh = auth.createSession(SessionOrigin::Unknown, "192.168.1.32", "Phone here");
    auth.saveSessions(); // the old TVs on disk with their old times
    CHECK(onDisk(tvId));

    revoked.clear();
    changed = 0;
    QTemporaryDir logDir;
    const QString before = Logger::instance()->logFilePath();
    const QString logPath = logDir.path() + "/purge.log";
    Logger::instance()->setLogFile(logPath, false);
    auth.purgeExpiredSessions();
    Logger::instance()->setLogFile(before);

    // The fixed bounds, each once; nothing for the sliding ones.
    CHECK_EQ(revoked.size(), 2);
    CHECK_EQ(revoked.count(tvId), 1);
    CHECK_EQ(revoked.count(secondId), 1);
    CHECK(!revoked.contains(phoneId));
    CHECK(!revoked.contains(letId));
    CHECK(!revoked.contains(keptId));
    CHECK(stillOnDisk.isEmpty());
    // Their rows, all four gone.
    CHECK(!auth.validateLanHttpSession(tv, "192.168.1.50"));
    CHECK(!auth.validateLanHttpSession(second, "192.168.1.52"));
    CHECK(!auth.validateSession(phone));
    CHECK(!auth.validateSession(let));
    // The kept TV goes on, without an approver.
    CHECK(auth.validateLanHttpSession(kept, "192.168.1.51"));
    CHECK(auth.sessionForToken(kept).approvedBy.isEmpty());
    CHECK(auth.validateSession(fresh));
    CHECK_EQ(auth.activeSessionCount(), 2);
    // One change for the whole purge.
    CHECK_EQ(changed, 1);

    // Its line gives numbers, never which device it was.
    {
        QFile written(logPath);
        CHECK(written.open(QIODevice::ReadOnly));
        QByteArray line;
        for (const QByteArray& l : written.readAll().split('\n'))
            if (l.contains("Purged")) line = l;
        CHECK(line.contains("Purged 4 expired session(s), 2 on a fixed bound"));
        CHECK(!line.contains("TV"));
        CHECK(!line.contains("Phone"));
        CHECK(!line.contains("192.168."));
    }

    // Nothing left to purge: nothing said, nothing changed.
    revoked.clear();
    changed = 0;
    auth.purgeExpiredSessions();
    CHECK(revoked.isEmpty());
    CHECK_EQ(changed, 0);

    // On disk as in memory: the kept TV without its approver.
    auth.loadSessions();
    CHECK(auth.validateLanHttpSession(kept, "192.168.1.51"));
    CHECK(auth.sessionForToken(kept).approvedBy.isEmpty());
    CHECK(!auth.validateSession(phone));

    // A revocation still takes along what the session let in.
    const QString owner = auth.createSession(SessionOrigin::Unknown, "192.168.1.33", "Owner");
    const QString ownersTv =
        auth.createLanHttpSession("192.168.1.53", "Owner's TV", auth.sessionIdForToken(owner));
    const QString ownersTvId = auth.sessionIdForToken(ownersTv);
    revoked.clear();
    stillOnDisk.clear();
    changed = 0;
    auth.destroySession(auth.sessionIdForToken(owner));
    CHECK(!auth.validateLanHttpSession(ownersTv, "192.168.1.53"));
    CHECK_EQ(revoked.size(), 2);
    CHECK(revoked.contains(ownersTvId));
    CHECK(stillOnDisk.isEmpty()); // the cascade too, before either stream stops
    CHECK_EQ(changed, 1);

    // A listener that calls back in (destroys a session of the same cascade)
    // finds it gone already: one signal per session, none for an unknown id.
    const QString top = auth.createSession(SessionOrigin::Unknown, "192.168.1.34", "Top");
    const QString topId = auth.sessionIdForToken(top);
    const QString mid = auth.createLanHttpSession("192.168.1.54", "Mid", topId);
    const QString midId = auth.sessionIdForToken(mid);
    const QString low = auth.createSession(SessionOrigin::Unknown, "192.168.1.35", "Low");
    const QString lowId = auth.sessionIdForToken(low);
    CHECK(auth.setSessionApprover(low, midId));
    const QMetaObject::Connection reentrant = QObject::connect(
        &auth, &AuthManager::sessionDestroyed, [&auth, &topId, &lowId](const QString& id) {
            if (id == topId) auth.destroySession(lowId);
        });
    revoked.clear();
    stillOnDisk.clear();
    auth.destroySession(topId);
    QObject::disconnect(reentrant);
    CHECK_EQ(revoked.size(), 3);
    CHECK_EQ(revoked.count(topId), 1);
    CHECK_EQ(revoked.count(midId), 1);
    CHECK_EQ(revoked.count(lowId), 1);
    CHECK(stillOnDisk.isEmpty());
    CHECK(!auth.validateSession(low));

    auth.destroyAllSessions();
    auth.saveSessions();
}

// ── TVs let in for a short while by a paired device (issue #42, TVNONADM) ───

/// A fresh P-256 public key, as a paired browser binds it.
QByteArray deviceKey()
{
    return PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());
}

void shortTvTests()
{
    SECTION("SessionOrigin — set by the server, kept on disk");

    QTemporaryDir tmp;
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/settings.json";
    writeSettings(settings, {}); // remember_me_days 90; short approval on, 8 h
    const qint64 day = 24 * 3600;
    const qint64 hour = 3600;

    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        const QString pin = auth.createSession(SessionOrigin::Pin, "192.168.1.30", "By PIN");
        const QString qr = auth.createSession(SessionOrigin::Qr, "192.168.1.31", "By QR");
        auth.createSession(SessionOrigin::Certificate, "192.168.1.32", "By certificate");
        auth.createSession(SessionOrigin::HomeScreen, "192.168.1.33", "From the home screen");
        auth.createSession(SessionOrigin::HostKey, "192.168.1.34", "By host key");
        auth.createSession(SessionOrigin::PinProof, "192.168.1.35", "By PIN proof");
        // Tv is a TV's alone: asked for an HTTPS session, it is not given.
        const QString notTv = auth.createSession(SessionOrigin::Tv, "192.168.1.36", "Claims TV");
        const QString tv = auth.createLanHttpSession("192.168.1.50", "A TV", {});
        CHECK(auth.sessionForToken(pin).origin == SessionOrigin::Pin);
        CHECK(auth.sessionForToken(qr).origin == SessionOrigin::Qr);
        CHECK(auth.sessionForToken(notTv).origin == SessionOrigin::Unknown);
        CHECK(auth.sessionForToken(tv).origin == SessionOrigin::Tv);
        auth.saveSessions();

        // Read back as written, each name its own; what the file cannot say
        // is Unknown, and a TV is a TV whatever it says.
        editStoredSession("By QR", [](QJsonObject& s) { s.remove("origin"); });
        editStoredSession("By PIN proof", [](QJsonObject& s) { s["origin"] = "admin"; });
        editStoredSession("By certificate", [](QJsonObject& s) { s["origin"] = "tv"; });
        editStoredSession("A TV", [](QJsonObject& s) { s["origin"] = "pin"; });
        auth.loadSessions();
        QHash<QString, SessionOrigin> byName;
        for (const SessionInfo& s : auth.sessions())
            byName.insert(s.machineName, s.origin);
        CHECK(byName.value("By PIN") == SessionOrigin::Pin);
        CHECK(byName.value("By QR") == SessionOrigin::Unknown);
        CHECK(byName.value("By PIN proof") == SessionOrigin::Unknown);
        CHECK(byName.value("By certificate") == SessionOrigin::Unknown);
        CHECK(byName.value("From the home screen") == SessionOrigin::HomeScreen);
        CHECK(byName.value("By host key") == SessionOrigin::HostKey);
        CHECK(byName.value("A TV") == SessionOrigin::Tv);
        auth.destroyAllSessions();
        auth.saveSessions();
    }

    SECTION("Short approval — who may let a TV in, and until when");

    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        const QString phone = auth.createSession(SessionOrigin::Pin, "192.168.1.30", "Galaxy S23");
        CHECK(auth.bindSessionKey(phone, deviceKey()));
        const QString phoneId = auth.sessionIdForToken(phone);
        CHECK(auth.mayLetInShortly(phone));
        CHECK(auth.shortApprovalAllowed());
        CHECK_EQ(auth.shortApprovalHours(), 8);

        // Each origin of the list, and only those (S1); Unknown alone is
        // told to pair again.
        const auto stateFor = [&auth](SessionOrigin origin) {
            const QString t = auth.createSession(origin, "192.168.1.40", "Origin");
            auth.bindSessionKey(t, deviceKey());
            const AuthManager::ShortApproval state = auth.shortApprovalState(t);
            auth.destroySession(auth.sessionIdForToken(t));
            return state;
        };
        CHECK(stateFor(SessionOrigin::Pin) == AuthManager::ShortApproval::Allowed);
        CHECK(stateFor(SessionOrigin::PinProof) == AuthManager::ShortApproval::Allowed);
        CHECK(stateFor(SessionOrigin::Qr) == AuthManager::ShortApproval::Allowed);
        CHECK(stateFor(SessionOrigin::Unknown) == AuthManager::ShortApproval::ReappairNeeded);
        CHECK(stateFor(SessionOrigin::Certificate) == AuthManager::ShortApproval::Refused);
        CHECK(stateFor(SessionOrigin::HomeScreen) == AuthManager::ShortApproval::Refused);
        CHECK(stateFor(SessionOrigin::HostKey) == AuthManager::ShortApproval::Refused);
        // Unknown, but not remembered, or without a key: refused, never told
        // to pair again.
        const QString guest =
            auth.createSession(SessionOrigin::Unknown, "192.168.1.41", "Guest", false, true);
        auth.bindSessionKey(guest, deviceKey());
        CHECK(auth.shortApprovalState(guest) == AuthManager::ShortApproval::Refused);
        const QString keyless =
            auth.createSession(SessionOrigin::Unknown, "192.168.1.42", "No key");
        CHECK(auth.shortApprovalState(keyless) == AuthManager::ShortApproval::Refused);
        const QString host =
            auth.createSession(SessionOrigin::Unknown, "192.168.1.43", "Host", true);
        auth.bindSessionKey(host, deviceKey());
        CHECK(auth.shortApprovalState(host) == AuthManager::ShortApproval::Refused);

        // Its TV: bound, for 8 hours, with the device.
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QString tv = auth.createShortLanHttpSession("192.168.1.60", "TV", phoneId);
        CHECK(auth.validateLanHttpSession(tv, "192.168.1.60"));
        const SessionInfo info = auth.sessionForToken(tv);
        CHECK(info.approverBound);
        CHECK(info.endsAt >= now + 8 * hour && info.endsAt <= now + 8 * hour + 5);
        CHECK_EQ(info.approvedBy, phoneId);
        CHECK(info.origin == SessionOrigin::Tv);
        // R8: its cookie never outlives it, never below zero.
        CHECK(auth.lanHttpCookieMaxAge(tv) > 8 * hour - 5 &&
              auth.lanHttpCookieMaxAge(tv) <= 8 * hour);
        // Not for an address that is not private, nor from a TV's session.
        CHECK(auth.createShortLanHttpSession("8.8.8.8", "TV", phoneId).isEmpty());
        CHECK(auth.createShortLanHttpSession("192.168.1.61", "TV", auth.sessionIdForToken(tv))
                  .isEmpty());
        CHECK(auth.createShortLanHttpSession("192.168.1.61", "TV", "no-such-session").isEmpty());

        // Never past the device's own end: one hour left to it, one hour to the TV.
        auth.setSessionTimesForTesting(phoneId, now - 100 * day, now - 90 * day + hour);
        const QString shortly = auth.createShortLanHttpSession("192.168.1.62", "TV", phoneId);
        const qint64 endsAt = auth.sessionForToken(shortly).endsAt;
        CHECK(endsAt > now + hour - 10 && endsAt <= now + hour);
        // None left to it: no TV, no cookie.
        auth.setSessionTimesForTesting(phoneId, now - 100 * day, now - 91 * day);
        CHECK(auth.createShortLanHttpSession("192.168.1.63", "TV", phoneId).isEmpty());

        // At most 12 hours, whatever the setting says.
        auth.setSessionTimesForTesting(phoneId, now, now);
        CHECK(settings.setLanHttpShortHours(12));
        CHECK_EQ(auth.shortApprovalHours(), AuthManager::SHORT_TV_MAX_HOURS);
        CHECK(!settings.setLanHttpShortHours(24));
        CHECK_EQ(settings.lanHttpShortHours(), 12);
        CHECK(settings.setLanHttpShortHours(8));

        // The device goes, however: logged out, its TV with it.
        const QString again = auth.createShortLanHttpSession("192.168.1.64", "TV", phoneId);
        CHECK(auth.validateLanHttpSession(again, "192.168.1.64"));
        CHECK(auth.logoutSession(phone));
        CHECK(!auth.validateLanHttpSession(again, "192.168.1.64"));
        auth.destroyAllSessions();
        auth.saveSessions();
    }

    SECTION("Short approval — wrong codes hold a device back, by session (S3, R4)");

    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        qint64 clock = 1'800'000'000;
        auth.setRateClockForTesting([&clock]() { return clock; });
        const QString phone = auth.createSession(SessionOrigin::Qr, "192.168.1.30", "Phone");
        CHECK(auth.bindSessionKey(phone, deviceKey()));
        const QString phoneId = auth.sessionIdForToken(phone);
        // Two, then a window gone by: counted again from none.
        CHECK(!auth.countShortApproverWrongCode(phoneId));
        CHECK(!auth.countShortApproverWrongCode(phoneId));
        clock += AuthManager::SHORT_APPROVER_WINDOW_SECS + 1;
        CHECK(!auth.countShortApproverWrongCode(phoneId));
        CHECK(!auth.countShortApproverWrongCode(phoneId));
        CHECK(auth.mayLetInShortly(phone));
        // The third within the window: held back for a while.
        CHECK(auth.countShortApproverWrongCode(phoneId));
        CHECK(!auth.mayLetInShortly(phone));
        // Not by address: another device at the same one may.
        const QString sameIp = auth.createSession(SessionOrigin::Pin, "192.168.1.30", "Other");
        CHECK(auth.bindSessionKey(sameIp, deviceKey()));
        CHECK(auth.mayLetInShortly(sameIp));
        clock += AuthManager::SHORT_APPROVER_BLOCK_SECS;
        CHECK(auth.mayLetInShortly(phone));
        CHECK(!auth.countShortApproverWrongCode(QString()));
        auth.setRateClockForTesting({});
        auth.destroyAllSessions();
        auth.saveSessions();
    }

    SECTION("Short approval — each TV ends at its own time, by either clock (R6, S5)");

    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        QStringList revoked;
        QStringList destroyed;
        QObject::connect(&auth, &AuthManager::streamingSessionRevoked,
                         [&revoked](const QString& id) { revoked.append(id); });
        QObject::connect(&auth, &AuthManager::sessionDestroyed,
                         [&destroyed](const QString& id) { destroyed.append(id); });
        const QString a = auth.createSession(SessionOrigin::Pin, "192.168.1.30", "Phone A");
        const QString b = auth.createSession(SessionOrigin::Qr, "192.168.1.31", "Phone B");
        const QString tvA =
            auth.createShortLanHttpSession("192.168.1.60", "TV A", auth.sessionIdForToken(a));
        const QString tvB =
            auth.createShortLanHttpSession("192.168.1.61", "TV B", auth.sessionIdForToken(b));
        const QString tvAId = auth.sessionIdForToken(tvA);
        const QString tvBId = auth.sessionIdForToken(tvB);
        auth.setSessionStreaming(tvA, true);
        auth.setSessionStreaming(tvB, true);
        // One timer, armed for the nearer end: 8 hours away for both.
        const int armed = auth.shortTimerRemainingForTesting();
        CHECK(armed > int(8 * hour - 10) * 1000 && armed <= int(8 * hour) * 1000 + 100);

        const qint64 now = QDateTime::currentSecsSinceEpoch();
        // TV A: its steady clock up, while the wall clock, set back, says ten
        // hours more. It ends all the same (S5), and is refused at once.
        auth.setShortEndForTesting(tvAId, now + 10 * hour, 0);
        CHECK(!auth.validateLanHttpSession(tvA, "192.168.1.60"));
        CHECK(auth.shortTimerRemainingForTesting() <= 100);
        auth.endShortTvsDueForTesting();
        CHECK_EQ(revoked, QStringList{tvAId});
        CHECK(destroyed.contains(tvAId)); // its socket is closed (O2)
        CHECK(!auth.sessions().isEmpty());
        // TV B runs on, and the timer is armed for it.
        CHECK(auth.validateLanHttpSession(tvB, "192.168.1.61"));
        CHECK(auth.shortTimerRemainingForTesting() > int(7 * hour) * 1000);

        // TV B: the wall clock past its end, the steady one not: it ends too.
        auth.setShortEndForTesting(tvBId, now - 1, 2 * hour * 1000);
        CHECK(!auth.validateLanHttpSession(tvB, "192.168.1.61"));
        auth.endShortTvsDueForTesting();
        CHECK_EQ(revoked, (QStringList{tvAId, tvBId}));
        // None left: the timer stops. The phones stay.
        CHECK_EQ(auth.shortTimerRemainingForTesting(), -1);
        CHECK(auth.validateSession(a));
        CHECK(auth.validateSession(b));
        // Nothing due: nothing said.
        auth.endShortTvsDueForTesting();
        CHECK_EQ(revoked.size(), qsizetype(2));
        auth.destroyAllSessions();
        auth.saveSessions();
    }

    SECTION("Short approval — the purge and the setting (R7, S6)");

    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        QStringList revoked;
        QObject::connect(&auth, &AuthManager::streamingSessionRevoked,
                         [&revoked](const QString& id) { revoked.append(id); });
        // What HttpServer closes a TV's WebSocket on (O2).
        QStringList destroyed;
        QObject::connect(&auth, &AuthManager::sessionDestroyed,
                         [&destroyed](const QString& id) { destroyed.append(id); });
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        // A phone gone past its sliding bound, with a TV it let in for a short
        // while and one it let in as the administrator once did.
        const QString phone = auth.createSession(SessionOrigin::Pin, "192.168.1.30", "Phone away");
        const QString phoneId = auth.sessionIdForToken(phone);
        const QString shortTv = auth.createShortLanHttpSession("192.168.1.60", "Short TV", phoneId);
        const QString shortTvId = auth.sessionIdForToken(shortTv);
        const QString adminTv = auth.createLanHttpSession("192.168.1.61", "Admin TV", phoneId);
        auth.setSessionStreaming(shortTv, true);
        auth.setSessionStreaming(phone, true);
        auth.setSessionTimesForTesting(phoneId, now - 100 * day, now - 91 * day);
        auth.purgeExpiredSessions();
        // R7: the short TV goes with it, its stream cut; the other is kept,
        // without an approver; the phone's own stream runs on (O3).
        CHECK(!auth.validateLanHttpSession(shortTv, "192.168.1.60"));
        CHECK_EQ(revoked, QStringList{shortTvId});
        CHECK(auth.validateLanHttpSession(adminTv, "192.168.1.61"));
        CHECK(auth.sessionForToken(adminTv).approvedBy.isEmpty());
        // The short TV's socket is closed; the phone's row, dropped on its
        // sliding bound, is not announced (O3), nor is the kept TV.
        CHECK_EQ(destroyed, QStringList{shortTvId});

        // S6: the setting turned off, the next purge ends what is left.
        const QString other = auth.createSession(SessionOrigin::Qr, "192.168.1.31", "Phone here");
        const QString leftTv = auth.createShortLanHttpSession("192.168.1.62", "Left TV",
                                                              auth.sessionIdForToken(other));
        CHECK(auth.validateLanHttpSession(leftTv, "192.168.1.62"));
        const QString leftTvId = auth.sessionIdForToken(leftTv);
        settings.setLanHttpShortApproval(false);
        CHECK(!auth.shortApprovalAllowed());
        CHECK(!auth.mayLetInShortly(other));
        destroyed.clear();
        auth.purgeExpiredSessions();
        CHECK(!auth.validateLanHttpSession(leftTv, "192.168.1.62"));
        CHECK_EQ(destroyed, QStringList{leftTvId});
        CHECK(auth.validateLanHttpSession(adminTv, "192.168.1.61"));
        CHECK(auth.validateSession(other));
        settings.setLanHttpShortApproval(true);
        auth.destroyAllSessions();
        auth.saveSessions();
    }

    SECTION("Short approval — read back from disk, never longer than it was given");

    {
        AuthManager auth(&settings);
        auth.destroyAllSessions();
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QString phone = auth.createSession(SessionOrigin::Pin, "192.168.1.30", "Phone");
        const QString phoneId = auth.sessionIdForToken(phone);
        const QString kept = auth.createShortLanHttpSession("192.168.1.60", "Kept", phoneId);
        const QString past = auth.createShortLanHttpSession("192.168.1.61", "Past", phoneId);
        auth.saveSessions();
        // One per device: only the last is left of these two; write the
        // others by hand from it.
        CHECK(!auth.validateLanHttpSession(kept, "192.168.1.60"));
        CHECK(auth.validateLanHttpSession(past, "192.168.1.61"));
        // Its end in the past: dropped. Beyond 12 hours: brought back to 12.
        // Made in the future: dropped. Its approver gone: dropped.
        const auto rewrite = [&](const std::function<void(QJsonObject&)>& edit) {
            editStoredSession("Past", edit);
            auth.loadSessions();
            return auth.sessionForToken(past);
        };
        SessionInfo s = rewrite([now](QJsonObject& o) { o["ends_at"] = now - 1; });
        CHECK(s.token.isEmpty());
        auth.saveSessions(); // nothing of it on disk now
        const QString later = auth.createShortLanHttpSession("192.168.1.61", "Past", phoneId);
        auth.saveSessions();
        editStoredSession("Past", [now](QJsonObject& o) { o["ends_at"] = now + 100 * hour; });
        auth.loadSessions();
        const SessionInfo capped = auth.sessionForToken(later);
        CHECK(capped.approverBound);
        CHECK(capped.endsAt <= now + 12 * hour + 5);
        CHECK(auth.lanHttpCookieMaxAge(later) <= 12 * hour + 5);
        editStoredSession("Past", [now](QJsonObject& o) { o["created_at"] = now + day; });
        auth.loadSessions();
        CHECK(!auth.validateLanHttpSession(later, "192.168.1.61"));

        const QString orphan = auth.createShortLanHttpSession("192.168.1.62", "Orphan", phoneId);
        auth.saveSessions();
        editStoredSession("Phone", [](QJsonObject& o) { o["token"] = "someone-else"; });
        auth.loadSessions();
        CHECK(!auth.validateLanHttpSession(orphan, "192.168.1.62"));
        // A TV the administrator let in is read back as before.
        const QString adminTv = auth.createLanHttpSession("192.168.1.63", "Admin TV", {});
        auth.saveSessions();
        auth.loadSessions();
        CHECK(auth.validateLanHttpSession(adminTv, "192.168.1.63"));
        CHECK(!auth.sessionForToken(adminTv).approverBound);
        auth.destroyAllSessions();
        auth.saveSessions();
    }
}

} // namespace

void run_auth_manager_tests()
{
    SECTION("AuthManager");

    QTemporaryDir tmp;
    AppSettings settings;
    settings.m_FilePath = tmp.path() + "/settings.json";

    AuthManager auth(&settings);

    // ── PIN lifecycle ──────────────────────────────────────────────────────
    QString pin = auth.generatePin();
    CHECK(!pin.isEmpty());
    CHECK(auth.hasValidPin());

    // Valid PIN from a fresh IP.
    auto ok = auth.validatePin("198.51.100.10", pin);
    CHECK(ok.result == AuthManager::Valid);

    // The PIN itself and nothing more: whatever follows it, or comes before.
    for (int pad : {1, 255, 256, 512, 1024}) {
        const QString ip = QString("198.51.100.%1").arg(100 + pad % 100);
        CHECK(auth.validatePin(ip, pin + QString(pad, QChar(0))).result != AuthManager::Valid);
        CHECK(auth.validatePin(ip, QString(pad, QChar(0)) + pin).result != AuthManager::Valid);
    }
    CHECK(auth.validatePin("198.51.100.11", pin.left(pin.size() - 1)).result != AuthManager::Valid);
    CHECK(auth.validatePin("198.51.100.11", pin).result == AuthManager::Valid);

    // The PIN made after one was used is shown to nobody, and opens nothing
    // until the administrator asks for a PIN again.
    {
        AppSettings used;
        used.m_FilePath = tmp.path() + "/used-pin.json";
        AuthManager a(&used);
        const QString first = a.generatePin();
        CHECK(a.validatePin("198.51.100.30", first).result == AuthManager::Valid);
        a.autoRegeneratePin();
        CHECK(a.isPinConsumed());
        CHECK(!a.hasValidPin());
        CHECK(a.validatePin("198.51.100.31", a.currentPin()).result != AuthManager::Valid);
        CHECK(a.validatePin("198.51.100.32", first).result != AuthManager::Valid);
        const QString fresh = a.generatePin();
        CHECK(a.hasValidPin());
        CHECK(a.validatePin("198.51.100.33", fresh).result == AuthManager::Valid);
    }

    // A failure is forgiven once per 10 minutes without a new one, by the clock:
    // attempts from other addresses do not wind a bucket back faster.
    {
        AppSettings rl;
        rl.m_FilePath = tmp.path() + "/rate.json";
        AuthManager a(&rl);
        a.generatePin();
        qint64 now = 1'900'000'000;
        a.setRateClockForTesting([&now] { return now; });
        const QString target = "203.0.113.50";
        for (int i = 0; i < 3; ++i)
            a.validatePin(target, "wrong-pin");
        CHECK_EQ(a.failedAttemptCount(target), 3);
        CHECK(a.isRateLimited(target));
        now += 31;
        CHECK(!a.isRateLimited(target));

        int other = 1;
        const auto fromOthers = [&](int n) {
            for (int i = 0; i < n; ++i, ++other) {
                const QString ip = QString("198.18.%1.%2").arg(other / 250).arg(other % 250 + 1);
                a.validatePin(ip, "wrong-pin");
            }
        };
        fromOthers(20);
        CHECK_EQ(a.failedAttemptCount(target), 3); // under 10 minutes: nothing forgiven

        now += 600 - 31; // 10 minutes after the last failure
        fromOthers(20);
        CHECK_EQ(a.failedAttemptCount(target), 2);
        now += 599;
        fromOthers(20);
        CHECK_EQ(a.failedAttemptCount(target), 2);
        now += 1;
        fromOthers(1);
        CHECK_EQ(a.failedAttemptCount(target), 1);
        now += 3 * 600; // a long quiet: every step it covers, once
        fromOthers(1);
        CHECK_EQ(a.failedAttemptCount(target), 0);
        a.setRateClockForTesting({});
    }

    // clearPin invalidates.
    auth.clearPin();
    CHECK(!auth.hasValidPin());
    auth.generatePin();
    auth.regeneratePin(); // new pin + clears sessions

    // ── Rate limiting ──────────────────────────────────────────────────────
    const QString badIp = "203.0.113.7";
    bool sawRateLimited = false;
    for (int i = 0; i < 6; ++i) {
        auto r = auth.validatePin(badIp, "definitely-wrong-pin");
        if (r.result == AuthManager::RateLimited) sawRateLimited = true;
    }
    CHECK(sawRateLimited);
    CHECK(auth.isRateLimited(badIp));
    CHECK(auth.lockoutSeconds(badIp) > 0);
    CHECK(auth.failedAttemptCount(badIp) > 0);
    CHECK(auth.remainingAttempts(badIp) >= 0); // sane, non-negative count

    // ── The PIN proved without being sent (MW-PIN-v1) ──────────────────────
    // A run is counted when it begins, on the typed PIN's bucket and tiers, and
    // cleared by its proof; a run made under a PIN that has since changed proves
    // nothing.
    {
        const QString shown = auth.generatePin();
        const QString honest = "198.51.100.40";
        CHECK(auth.beginPinProof(honest).result == AuthManager::Valid);
        CHECK_EQ(auth.failedAttemptCount(honest), 1);
        CHECK(auth.finishPinProof(honest, shown, true).result == AuthManager::Valid);
        CHECK_EQ(auth.failedAttemptCount(honest), 0);

        // A wrong proof is the failure its run began with, not a second one.
        const QString wrong = "198.51.100.41";
        auth.beginPinProof(wrong);
        const auto refused = auth.finishPinProof(wrong, shown, false);
        CHECK(refused.result == AuthManager::InvalidPin);
        CHECK_EQ(refused.remainingAttempts, 2);
        CHECK_EQ(auth.failedAttemptCount(wrong), 1);

        // A run left unfinished stays a failure.
        const QString left = "198.51.100.42";
        auth.beginPinProof(left);
        CHECK_EQ(auth.failedAttemptCount(left), 1);

        // The PIN changed since the run began, or none is shown.
        const QString late = "198.51.100.43";
        auth.beginPinProof(late);
        // Another PIN: one draw in a million gives the same one back.
        while (auth.generatePin() == shown) {}
        CHECK(auth.finishPinProof(late, shown, true).result == AuthManager::InvalidPin);
        auth.clearPin();
        auth.beginPinProof(late);
        CHECK(auth.finishPinProof(late, auth.currentPin(), true).result == AuthManager::InvalidPin);

        // Three runs lock the bucket as three wrong PINs do. The third is still
        // admitted, and its proof still good.
        const QString current = auth.generatePin();
        const QString runs = "198.51.100.44";
        CHECK(auth.beginPinProof(runs).result == AuthManager::Valid);
        CHECK(auth.beginPinProof(runs).result == AuthManager::Valid);
        CHECK(auth.beginPinProof(runs).result == AuthManager::Valid);
        CHECK(auth.beginPinProof(runs).result == AuthManager::RateLimited);
        CHECK(auth.validatePin(runs, current).result == AuthManager::RateLimited);
        CHECK(auth.finishPinProof(runs, current, true).result == AuthManager::Valid);
        CHECK(!auth.isRateLimited(runs));

        // Wrong PINs typed lock the proof out too: one bucket for both ways in.
        const QString typed = "198.51.100.45";
        for (int i = 0; i < 3; ++i)
            auth.validatePin(typed, "wrong");
        CHECK(auth.beginPinProof(typed).result == AuthManager::RateLimited);
        CHECK_EQ(auth.failedAttemptCount(typed), 3);
    }

    // ── Sessions ───────────────────────────────────────────────────────────
    QString token = auth.createSession(SessionOrigin::Unknown, "198.51.100.20", "Laptop");
    CHECK(!token.isEmpty());
    CHECK(auth.validateSession(token));
    CHECK(!auth.validateSession("bogus-token"));
    CHECK_EQ(auth.activeSessionCount(), 1);

    auth.touchSession(token);                                 // sliding expiration bump
    auth.setSessionStreaming(token, true);                    // single active stream flag
    auth.setSessionGeo(token, "Paris", "FR");                 // async geo result
    CHECK(auth.updateSessionAddress(token, "198.51.100.99")); // IP changed → true

    QList<SessionInfo> list = auth.sessions();
    CHECK_EQ(list.size(), 1);
    const QString id = list.first().token; // opaque id == token hash, not the raw cookie
    CHECK(id != token);
    CHECK(auth.renameSession(id, "Renamed"));
    CHECK(!auth.renameSession("unknown-id", "x"));

    // What identifies a session stays out of the log, found or not.
    {
        QTemporaryDir logDir;
        const QString before = Logger::instance()->logFilePath();
        const QString logPath = logDir.path() + "/auth.log";
        Logger::instance()->setLogFile(logPath, false);
        auth.destroySession("canary-session-id");
        auth.renameSession("canary-session-id", "x");
        CHECK(auth.renameSession(id, "Renamed"));
        auth.setSessionGeo(token, "Paris", "FR");
        Logger::instance()->setLogFile(before);

        QFile written(logPath);
        CHECK(written.open(QIODevice::ReadOnly));
        const QByteArray lines = written.readAll();
        CHECK(lines.contains("no such session"));
        CHECK(!lines.contains("canary-session-id"));
        CHECK(!lines.contains(id.toUtf8()));
        CHECK(lines.contains("Geo data stored"));
        CHECK(!lines.contains(token.left(8).toUtf8()));
    }

    // Persistence round-trip.
    auth.saveSessions();
    auth.loadSessions();

    // A session that goes asks for its own streams to end, by its id: the
    // cookie of each slot is matched to it, and no other session's stream is.
    QStringList revoked;
    QObject::connect(&auth, &AuthManager::streamingSessionRevoked,
                     [&revoked](const QString& sessionId) { revoked.append(sessionId); });
    auth.setSessionStreaming(token, true); // re-flag: streaming is runtime-only, reset by load
    auth.destroySession(id);
    CHECK(!auth.validateSession(token)); // gone after destroy by id
    CHECK(revoked == QStringList{id});
    CHECK(AuthManager::isTokenOf(token, id)); // still matched, the session gone
    CHECK(!AuthManager::isTokenOf(QStringLiteral("another-cookie"), id));
    CHECK(!AuthManager::isTokenOf(QString(), id));
    CHECK(!AuthManager::isTokenOf(token, QString()));

    // Whatever the streaming flag says: it follows one stream only, the latest.
    const QString tokenA = auth.createSession(SessionOrigin::Unknown, "198.51.100.23");
    const QString tokenB = auth.createSession(SessionOrigin::Unknown, "198.51.100.24");
    auth.setSessionStreaming(tokenA, true);
    auth.setSessionStreaming(tokenB, true); // A's flag is cleared, A still streams
    revoked.clear();
    auth.destroySession(auth.sessionIdForToken(tokenA));
    CHECK_EQ(revoked.size(), 1);
    CHECK(AuthManager::isTokenOf(tokenA, revoked.value(0)));
    CHECK(!AuthManager::isTokenOf(tokenB, revoked.value(0)));
    // Logging out is the same going.
    revoked.clear();
    CHECK(auth.logoutSession(tokenB));
    CHECK_EQ(revoked.size(), 1);
    CHECK(AuthManager::isTokenOf(tokenB, revoked.value(0)));

    // Every session at once: every stream, with an empty id.
    auth.createSession(SessionOrigin::Unknown, "198.51.100.21");
    revoked.clear();
    auth.destroyAllSessions();
    CHECK_EQ(auth.activeSessionCount(), 0);
    CHECK(revoked == QStringList{QString()});
    // Nothing left to revoke, nothing asked.
    revoked.clear();
    auth.destroyAllSessions();
    CHECK(revoked.isEmpty());
    auth.purgeExpiredSessions(); // no-op, just exercised

    // ── Session listing order and identity ─────────────────────────────────
    // The admin table re-renders on a timer, so the order must come from the
    // data and not from the hash layout: rows that move between two refreshes
    // send a click to the wrong device's Revoke button. Ties (same second) fall
    // back to the id, so the list is fully determined either way.
    const QString tokA = auth.createSession(SessionOrigin::Unknown, "198.51.100.30", "A");
    const QString tokB = auth.createSession(SessionOrigin::Unknown, "198.51.100.31", "B");
    const QString tokC = auth.createSession(SessionOrigin::Unknown, "198.51.100.32", "C");
    QList<SessionInfo> ordered = auth.sessions();
    CHECK_EQ(ordered.size(), 3);
    for (int i = 1; i < ordered.size(); ++i) {
        const bool sorted = ordered[i - 1].createdAt > ordered[i].createdAt ||
                            (ordered[i - 1].createdAt == ordered[i].createdAt &&
                             ordered[i - 1].token < ordered[i].token);
        CHECK(sorted);
    }
    CHECK_EQ(auth.sessions().first().token, ordered.first().token); // same list twice

    // The raw cookie token maps back to the id the admin UI revokes by, so the
    // caller's own row can be marked before it is clicked.
    const QString idB = auth.sessionIdForToken(tokB);
    CHECK(!idB.isEmpty());
    CHECK(idB != tokB); // the id is the hash, never the cookie value
    CHECK(auth.sessionIdForToken("bogus-token").isEmpty());
    CHECK(auth.sessionIdForToken(QString()).isEmpty());
    auth.destroySession(idB);
    CHECK(auth.sessionIdForToken(tokB).isEmpty()); // gone with its session
    auth.destroySession(auth.sessionIdForToken(tokA));
    auth.destroySession(auth.sessionIdForToken(tokC));
    CHECK_EQ(auth.activeSessionCount(), 0);

    // ── Logging yourself out ───────────────────────────────────────────────
    // A visitor holds nothing but the raw cookie, so that is what logoutSession
    // takes — and it can only ever reach their own session. Without this, the
    // only way out is the admin page, which someone in an internet café cannot
    // open: they would walk away leaving a 90-day session on a public machine.
    const QString mine = auth.createSession(SessionOrigin::Unknown, "198.51.100.40", "Bar PC");
    const QString theirs =
        auth.createSession(SessionOrigin::Unknown, "198.51.100.41", "Someone else");
    CHECK(auth.logoutSession(mine));
    CHECK(!auth.validateSession(mine));
    CHECK(auth.validateSession(theirs));   // the neighbour is untouched
    CHECK(!auth.logoutSession(mine));      // already gone
    CHECK(!auth.logoutSession(QString())); // no cookie at all
    CHECK(!auth.logoutSession("bogus-token"));
    CHECK(!auth.logoutSession(auth.sessionIdForToken(theirs))); // the id is not a cookie
    CHECK(auth.validateSession(theirs));                        // …so it revokes nothing
    auth.destroyAllSessions();

    // ── "Remember me" declined ─────────────────────────────────────────────
    // Same session object, two differences: it never reaches the disk, and it
    // expires in hours rather than months.
    const QString temp = auth.createSession(SessionOrigin::Unknown, "198.51.100.42", "Kiosk", false,
                                            /*ephemeral=*/true);
    CHECK(auth.validateSession(temp));
    CHECK(auth.isEphemeralSession(temp));
    CHECK_EQ(auth.sessions().size(), 1);
    CHECK(auth.sessions().first().ephemeral);

    const QString kept = auth.createSession(SessionOrigin::Unknown, "198.51.100.43", "Home PC");
    CHECK(!auth.isEphemeralSession(kept));
    CHECK(!auth.isEphemeralSession("bogus-token"));

    // The round-trip is the assertion: only the remembered session comes back.
    auth.saveSessions();
    auth.loadSessions();
    CHECK_EQ(auth.activeSessionCount(), 1);
    CHECK(!auth.sessions().first().ephemeral);
    CHECK_EQ(auth.sessions().first().machineName, QStringLiteral("Home PC"));
    auth.destroyAllSessions();

    // ── The browser key a session is bound to (MW-BIND-v1) ─────────────────
    // What HttpServer::tunnelKeyRefused holds a tunnel's key against. A session
    // keeps the key it was bound to: the cookie copied out of the browser it
    // lives in cannot be bound again to the copier's own key.
    {
        const QByteArray keyA =
            PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());
        const QByteArray keyB =
            PairingCrypto::publicKeySpkiFromPrivatePem(PairingCrypto::generatePrivateKeyPem());
        const QString session =
            auth.createSession(SessionOrigin::Unknown, "198.51.100.50", "Phone");
        CHECK(auth.sessionPairingKey(session).isEmpty()); // like one older than binding
        CHECK(!auth.bindSessionKey(session, QByteArray("not a key")));
        CHECK(auth.bindSessionKey(session, keyA));
        CHECK(auth.bindSessionKey(session, keyA));  // the same browser, after a reconnect
        CHECK(!auth.bindSessionKey(session, keyB)); // another browser's key
        CHECK(auth.sessionPairingKey(session) == keyA);
        CHECK(!auth.bindSessionKey(QString(), keyA));
        CHECK(!auth.bindSessionKey("bogus-token", keyA));
        CHECK(auth.sessionPairingKey("bogus-token").isEmpty());
        auth.destroyAllSessions();
    }

    // ── Certificate auth ───────────────────────────────────────────────────
    QString cert = auth.generateCertificateToken();
    CHECK(!cert.isEmpty());
    CHECK_EQ(auth.certificateToken(), cert);
    CHECK(auth.validateCertificate(cert));
    CHECK(!auth.validateCertificate("not-the-cert"));
    auth.setCertAuthEnabled(true);
    CHECK(auth.certAuthEnabled());

    // ── Remote admin password ──────────────────────────────────────────────
    // Out of the box the door is SHUT: remote administration is enabled, but no
    // password exists and there is no built-in default to fall back on, so
    // nothing an attacker can type unlocks anything.
    const QString adminIp = "192.168.5.20";
    CHECK(auth.remoteAdminEnabled());
    CHECK(!auth.adminPasswordSet());
    CHECK(settings.adminPasswordDigest().isEmpty()); // nothing stored yet
    CHECK(auth.validateAdminPassword(adminIp, "moonlightweb").result == AuthManager::InvalidPin);
    CHECK(auth.validateAdminPassword(adminIp, "").result == AuthManager::InvalidPin);

    CHECK(!auth.setAdminPassword("short")); // below MIN_ADMIN_PASSWORD_LEN
    CHECK(!auth.adminPasswordSet());

    CHECK(auth.setAdminPassword("correct-horse"));
    CHECK(auth.adminPasswordSet());

    // The plaintext is never stored: what lands in settings is a PBKDF2 digest.
    const QString digest = settings.adminPasswordDigest();
    CHECK(digest.startsWith("pbkdf2-sha256$"));
    CHECK(!digest.contains("correct-horse"));

    CHECK(auth.validateAdminPassword(adminIp, "correct-horse").result == AuthManager::Valid);
    CHECK(auth.validateAdminPassword(adminIp, "wrong").result == AuthManager::InvalidPin);

    // Its lockout counter is its own: hammering the password must not lock the
    // same address out of PIN login.
    const QString pinIp = "192.168.5.21";
    auth.generatePin();
    for (int i = 0; i < 6; ++i)
        auth.validateAdminPassword(pinIp, "wrong");
    CHECK(auth.isRateLimited(pinIp) == false);
    CHECK(auth.validateAdminPassword(pinIp, "correct-horse").result == AuthManager::RateLimited);

    // A session is promoted, not created: the password raises what an
    // already-admitted device may do.
    QString adminToken = auth.createSession(SessionOrigin::Unknown, adminIp, "Living room PC");
    CHECK(!auth.isAdminSession(adminToken));
    CHECK(auth.promoteSessionToAdmin(adminToken));
    CHECK(auth.isAdminSession(adminToken));
    CHECK(!auth.isHostSession(adminToken)); // admin, but not the host machine
    CHECK(!auth.promoteSessionToAdmin("bogus-token"));

    // How a host session was born decides what it is worth afterwards. A host
    // session created on a socket must NOT be honoured on the tunnel — a token
    // minted at the machine says nothing about who holds it at the far end of a
    // relayed connection — and HttpServer asks this question to tell them apart.
    // Only a session created BY a tunnel redemption answers yes.
    {
        const QString onSocket = auth.createSession(SessionOrigin::Unknown, "127.0.0.1",
                                                    "Host machine", /*isHost=*/true);
        CHECK(auth.isHostSession(onSocket));
        CHECK(!auth.isTunnelHostSession(onSocket));

        const QString onTunnel =
            auth.createSession(SessionOrigin::Unknown, "192.168.1.44", "Host machine (remote link)",
                               /*isHost=*/true, /*ephemeral=*/false,
                               /*viaTunnel=*/true);
        CHECK(auth.isHostSession(onTunnel));
        CHECK(auth.isTunnelHostSession(onTunnel));

        // A plain session is neither, whatever it claims.
        CHECK(!auth.isTunnelHostSession(adminToken));
        CHECK(!auth.isTunnelHostSession("bogus-token"));

        // It has to survive a restart, or the owner would silently lose admin
        // the first time the server came back.
        auth.saveSessions();
        auth.loadSessions();
        CHECK(auth.isTunnelHostSession(onTunnel));
        CHECK(!auth.isTunnelHostSession(onSocket));
    }

    // Changing the password revokes every unlock the old one bought.
    CHECK(auth.setAdminPassword("another-password"));
    CHECK(!auth.isAdminSession(adminToken));
    CHECK(auth.validateSession(adminToken)); // still signed in, just not admin

    // Disabling closes the door entirely — the stored password stops being
    // accepted — and a stale flag on disk is not honoured on the way back in.
    auth.promoteSessionToAdmin(adminToken);
    auth.setRemoteAdminEnabled(false);
    CHECK(!auth.isAdminSession(adminToken));
    CHECK(auth.validateAdminPassword("192.168.5.30", "another-password").result ==
          AuthManager::InvalidPin);
    auth.saveSessions();
    auth.loadSessions();
    CHECK(!auth.isAdminSession(adminToken));

    // Re-enabling restores the operator's password: the digest survived the
    // round trip.
    auth.setRemoteAdminEnabled(true);
    CHECK(auth.adminPasswordSet());
    CHECK(auth.validateAdminPassword("192.168.5.32", "another-password").result ==
          AuthManager::Valid);

    // The internet side of the door is shut out of the box, and opening it is
    // persisted like the rest.
    CHECK(!auth.remoteAdminInternet());
    CHECK(!settings.remoteAdminInternet());
    auth.setRemoteAdminInternet(true);
    CHECK(auth.remoteAdminInternet());
    CHECK(settings.remoteAdminInternet());

    // From the internet every address shares one more counter: three wrong
    // guesses from three addresses lock the fourth out as well. The LAN never
    // feeds that counter and keeps its own.
    CHECK(auth.validateAdminPassword("203.0.113.1", "wrong").result == AuthManager::InvalidPin);
    CHECK(auth.validateAdminPassword("203.0.113.2", "wrong").result == AuthManager::InvalidPin);
    {
        const AuthManager::ValidateResult third =
            auth.validateAdminPassword("203.0.113.3", "wrong");
        CHECK(third.result == AuthManager::InvalidPin);
        CHECK(third.lockoutSeconds > 0);
        CHECK_EQ(third.remainingAttempts, 2); // the shared counter's tier: 5 - 3
    }
    CHECK(auth.validateAdminPassword("203.0.113.4", "another-password").result ==
          AuthManager::RateLimited);
    CHECK(auth.validateAdminPassword("192.168.5.33", "another-password").result ==
          AuthManager::Valid);

    // Closing the internet side takes back every unlock: which ones it bought
    // is not recorded, and a LAN unlock is honoured from anywhere afterwards.
    {
        const QString office =
            auth.createSession(SessionOrigin::Unknown, "203.0.113.9", "Office laptop");
        CHECK(auth.promoteSessionToAdmin(office));
        auth.setRemoteAdminInternet(false);
        CHECK(!auth.isAdminSession(office));
        CHECK(auth.validateSession(office)); // still signed in, just not admin
        CHECK(!settings.remoteAdminInternet());
    }

    // Removing the password shuts the door again and takes back every unlock;
    // remote administration itself stays on, waiting for a new password.
    {
        const QString laptop = auth.createSession(SessionOrigin::Unknown, "192.168.5.40", "Laptop");
        CHECK(auth.promoteSessionToAdmin(laptop));
        auth.clearAdminPassword();
        CHECK(!auth.adminPasswordSet());
        CHECK(settings.adminPasswordDigest().isEmpty());
        CHECK(!auth.isAdminSession(laptop));
        CHECK(auth.validateSession(laptop));
        CHECK(auth.remoteAdminEnabled());
        CHECK(auth.validateAdminPassword("192.168.5.41", "another-password").result ==
              AuthManager::InvalidPin);
    }
    auth.destroyAllSessions();

    // ── LAN address classification (gates the unlock) ──────────────────────
    CHECK(AuthManager::isLanAddress("127.0.0.1"));
    CHECK(AuthManager::isLanAddress("::1"));
    CHECK(AuthManager::isLanAddress("192.168.1.5"));
    CHECK(AuthManager::isLanAddress("10.1.2.3"));
    CHECK(AuthManager::isLanAddress("172.20.0.1"));
    CHECK(AuthManager::isLanAddress("::ffff:192.168.1.5")); // IPv4-mapped
    CHECK(AuthManager::isLanAddress("169.254.3.4"));        // link-local
    CHECK(AuthManager::isLanAddress("fe80::1"));            // link-local v6
    CHECK(AuthManager::isLanAddress("fd12:3456::1"));       // unique-local v6
    CHECK(!AuthManager::isLanAddress("8.8.8.8"));
    CHECK(!AuthManager::isLanAddress("172.32.0.1")); // just outside 172.16/12
    CHECK(!AuthManager::isLanAddress("2001:db8::1"));
    CHECK(!AuthManager::isLanAddress(""));

    // ── Who may spend the password ─────────────────────────────────────────
    // On a socket both conditions are required, because either one alone is
    // forgeable in a deployment we support.
    CHECK(AuthManager::canUnlockAdmin("192.168.1.5", /*hostTrusted*/ true, /*viaTunnel*/ false));
    // A TLS-terminating tunnel on this machine forwards the whole internet from
    // loopback under our own name: local peer, trusted Host, still not a LAN
    // machine — which is why the peer check is not the only one…
    CHECK(!AuthManager::canUnlockAdmin("192.168.1.5", false, false));
    // …and a hairpin-NAT LAN client arrives under the public domain, which is
    // why the Host check is not the only one either.
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", true, false));

    // Through the rendezvous the Host header names the introduction server, so
    // hostTrusted is false for every arrival and cannot gate anything. The
    // address is ICE's own conclusion instead: a LAN peer is admitted, the
    // internet is not.
    CHECK(AuthManager::canUnlockAdmin("192.168.1.5", false, /*viaTunnel*/ true));
    CHECK(AuthManager::canUnlockAdmin("fd12:3456::1", false, true));
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", false, true));
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", true, true));
    // A peer with no negotiated candidate pair is labelled, not addressed. It
    // parses as nothing and is refused on both paths.
    CHECK(!AuthManager::canUnlockAdmin("peer-4f2a", false, true));
    CHECK(!AuthManager::canUnlockAdmin("", true, true));

    // Once the owner opens the internet side: a public peer is admitted on
    // the rendezvous tunnel, where it holds a session earned with the PIN
    // over that link — never on a socket, where a proxy on this machine is
    // exactly what forwards the whole internet here.
    CHECK(AuthManager::canUnlockAdmin("8.8.8.8", false, /*viaTunnel*/ true, /*internet*/ true));
    CHECK(AuthManager::canUnlockAdmin("2001:db8::1", false, true, true));
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", true, /*viaTunnel*/ false, true));
    CHECK(!AuthManager::canUnlockAdmin("8.8.8.8", false, true, /*internet*/ false));
    // A label is still nothing, and the LAN's own rules do not loosen.
    CHECK(!AuthManager::canUnlockAdmin("peer-4f2a", false, true, true));
    CHECK(!AuthManager::canUnlockAdmin("", false, true, true));
    CHECK(!AuthManager::canUnlockAdmin("192.168.1.5", false, false, true));
    CHECK(AuthManager::canUnlockAdmin("192.168.1.5", false, true, true));

    // ── Address helpers (static) ───────────────────────────────────────────
    CHECK_EQ(AuthManager::cleanClientAddress("::ffff:192.168.1.5"), QString("192.168.1.5"));
    CHECK_EQ(AuthManager::isPrivateIP("192.168.1.5"), QString("Local"));
    CHECK_EQ(AuthManager::isPrivateIP("10.0.0.1"), QString("Local"));
    CHECK_EQ(AuthManager::isPrivateIP("8.8.8.8"), QString("Remote"));
    CHECK_EQ(AuthManager::rateLimitKey("8.8.8.8"), QString("8.8.8.8"));   // IPv4 = raw
    CHECK(!AuthManager::rateLimitKey("2001:db8:abcd:1234::1").isEmpty()); // IPv6 = /64-ish

    // ── Whose attempt: behind a local reverse proxy, the visitor's ─────────
    {
        using H = QMap<QString, QString>;
        // No proxy headers: the peer, even on loopback.
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1", H{}), QString("127.0.0.1"));
        // Funnel / nginx: the LAST X-Forwarded-For entry is the proxy's own;
        // what a visitor wrote before it is theirs to forge.
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1",
                                             H{{"x-forwarded-for", "6.6.6.6, 203.0.113.9"}}),
                 QString("203.0.113.9"));
        CHECK_EQ(AuthManager::attemptAddress("::1", H{{"x-forwarded-for", "203.0.113.9"}}),
                 QString("203.0.113.9"));
        // cloudflared names the visitor outright, and wins.
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1", H{{"cf-connecting-ip", "198.51.100.7"},
                                                            {"x-forwarded-for", "10.0.0.1"}}),
                 QString("198.51.100.7"));
        // RFC 7239, quoted IPv6 with a port, IPv4 with a port.
        CHECK_EQ(AuthManager::attemptAddress(
                     "127.0.0.1", H{{"forwarded", "for=1.1.1.1, for=\"[2001:db8::7]:4711\""}}),
                 QString("2001:db8::7"));
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1",
                                             H{{"forwarded", "proto=https;for=192.0.2.60:80"}}),
                 QString("192.0.2.60"));
        // A LAN or internet peer is never believed: the header would buy a fresh
        // counter with every guess.
        CHECK_EQ(AuthManager::attemptAddress("192.168.1.20", H{{"x-forwarded-for", "9.9.9.9"}}),
                 QString("192.168.1.20"));
        CHECK_EQ(AuthManager::attemptAddress("8.8.8.8", H{{"cf-connecting-ip", "9.9.9.9"}}),
                 QString("8.8.8.8"));
        // Garbage in the header: the peer.
        CHECK_EQ(AuthManager::attemptAddress("127.0.0.1", H{{"x-forwarded-for", "not-an-ip"}}),
                 QString("127.0.0.1"));
    }

    // ── Home-screen handoff ────────────────────────────────────────────────
    {
        const QString owner =
            auth.createSession(SessionOrigin::Unknown, "198.51.100.40", "iOS Safari");
        const QString key = auth.homeScreenKey(owner);
        CHECK(key.startsWith(AuthManager::HOME_SCREEN_KEY_PREFIX));
        CHECK(key.size() > 40);
        // Every page load reads the manifest; the key stays the same meanwhile.
        CHECK_EQ(auth.homeScreenKey(owner), key);

        const QJsonArray machines{QJsonObject{{"id", "n95"}, {"name", "N95"}, {"url", "u"}}};
        auth.setHomeScreenInstances(owner, machines);

        // A host key, or anything else, is not a home-screen key.
        CHECK(!auth.redeemHomeScreenKey("not-a-key").has_value());
        CHECK(!auth.redeemHomeScreenKey(AuthManager::HOME_SCREEN_KEY_PREFIX + QString("x"))
                   .has_value());

        const auto handoff = auth.redeemHomeScreenKey(key);
        CHECK(handoff.has_value());
        if (handoff) {
            CHECK_EQ(handoff->machineName, QString("iOS Safari"));
            CHECK_EQ(handoff->instances, machines);
        }
        // Single-use.
        CHECK(!auth.redeemHomeScreenKey(key).has_value());

        // Spent, a new one is minted for the next shortcut.
        const QString second = auth.homeScreenKey(owner);
        CHECK(!second.isEmpty() && second != key);

        // Past its lifetime it is refused — and spent by the refusal.
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        const QString late =
            auth.homeScreenKey(auth.createSession(SessionOrigin::Unknown, "198.51.100.41", "Old"),
                               now - AuthManager::HOME_SCREEN_KEY_TTL_SECS - 5);
        CHECK(!auth.redeemHomeScreenKey(late, now).has_value());

        // Past half its life the manifest gets a fresh one.
        const QString aging = auth.createSession(SessionOrigin::Unknown, "198.51.100.42", "Aging");
        const QString first =
            auth.homeScreenKey(aging, now - AuthManager::HOME_SCREEN_KEY_TTL_SECS);
        CHECK(auth.homeScreenKey(aging, now) != first);

        // Revoking the session it came from revokes the key with it.
        auth.destroySession(auth.sessionIdForToken(owner));
        CHECK(!auth.redeemHomeScreenKey(second).has_value());

        // A visitor who declined "remember me" hands out no second session.
        const QString temporary =
            auth.createSession(SessionOrigin::Unknown, "198.51.100.43", "Kiosk", false, true);
        CHECK(auth.homeScreenKey(temporary).isEmpty());
        CHECK(auth.homeScreenKey(QString()).isEmpty());
    }

    // ── Who may know what this machine is called ───────────────────────────
    {
        const auto socket = [](const QString& address, const QString& host) {
            HttpRequest req;
            req.clientAddress = address;
            req.hostTrusted = true;
            req.headers.insert(QStringLiteral("host"), host);
            return req;
        };
        const QString ours = QStringLiteral("192.168.1.10:8443");

        // On this LAN, before any session.
        CHECK(AuthManager::mayKnowMachineName(socket("192.168.1.20", ours), false));
        // From the internet, once signed in only.
        CHECK(!AuthManager::mayKnowMachineName(socket("203.0.113.7", ours), false));
        CHECK(AuthManager::mayKnowMachineName(socket("203.0.113.7", ours), true));
        // A Host this machine does not go by is not this LAN.
        HttpRequest rebound = socket("192.168.1.20", "pc.example.test");
        rebound.hostTrusted = false;
        CHECK(!AuthManager::mayKnowMachineName(rebound, false));
        // The internet a proxy on this machine forwards from loopback.
        HttpRequest proxied = socket("127.0.0.1", "pc.example.test");
        CHECK(!AuthManager::mayKnowMachineName(proxied, false));
        proxied.headers.insert(QStringLiteral("x-forwarded-for"), QStringLiteral("203.0.113.50"));
        CHECK(!AuthManager::mayKnowMachineName(proxied, false));
        // The host machine itself, from loopback or with its host key from anywhere.
        HttpRequest host = socket("203.0.113.7", "pc.example.test");
        host.isHostMachine = true;
        CHECK(AuthManager::mayKnowMachineName(host, false));
        // On a line, by the address ICE settled on.
        HttpRequest line;
        line.viaTunnel = true;
        line.clientAddress = QStringLiteral("192.168.1.30");
        CHECK(AuthManager::mayKnowMachineName(line, false));
        line.clientAddress = QStringLiteral("198.51.100.9");
        CHECK(!AuthManager::mayKnowMachineName(line, false));
        CHECK(AuthManager::mayKnowMachineName(line, true));
    }
    pinProofRouteTests();
    rememberMeDaysTests();
    lanHttpSessionTests();
    purgeTests();
    shortTvTests();
}
