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
#include "server/RestRouter.h"
#include "server/routes/PinProofRoutes.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QList>
#include <QTemporaryDir>

#include <openssl/bn.h>
#include <openssl/ec.h>
#include <openssl/obj_mac.h>

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
    QString token = auth.createSession("198.51.100.20", "Laptop");
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
    const QString tokenA = auth.createSession("198.51.100.23");
    const QString tokenB = auth.createSession("198.51.100.24");
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
    auth.createSession("198.51.100.21");
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
    const QString tokA = auth.createSession("198.51.100.30", "A");
    const QString tokB = auth.createSession("198.51.100.31", "B");
    const QString tokC = auth.createSession("198.51.100.32", "C");
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
    const QString mine = auth.createSession("198.51.100.40", "Bar PC");
    const QString theirs = auth.createSession("198.51.100.41", "Someone else");
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
    const QString temp = auth.createSession("198.51.100.42", "Kiosk", false, /*ephemeral=*/true);
    CHECK(auth.validateSession(temp));
    CHECK(auth.isEphemeralSession(temp));
    CHECK_EQ(auth.sessions().size(), 1);
    CHECK(auth.sessions().first().ephemeral);

    const QString kept = auth.createSession("198.51.100.43", "Home PC");
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
        const QString session = auth.createSession("198.51.100.50", "Phone");
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
    QString adminToken = auth.createSession(adminIp, "Living room PC");
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
        const QString onSocket = auth.createSession("127.0.0.1", "Host machine", /*isHost=*/true);
        CHECK(auth.isHostSession(onSocket));
        CHECK(!auth.isTunnelHostSession(onSocket));

        const QString onTunnel = auth.createSession("192.168.1.44", "Host machine (remote link)",
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
        const QString office = auth.createSession("203.0.113.9", "Office laptop");
        CHECK(auth.promoteSessionToAdmin(office));
        auth.setRemoteAdminInternet(false);
        CHECK(!auth.isAdminSession(office));
        CHECK(auth.validateSession(office)); // still signed in, just not admin
        CHECK(!settings.remoteAdminInternet());
    }

    // Removing the password shuts the door again and takes back every unlock;
    // remote administration itself stays on, waiting for a new password.
    {
        const QString laptop = auth.createSession("192.168.5.40", "Laptop");
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
        const QString owner = auth.createSession("198.51.100.40", "iOS Safari");
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
        const QString late = auth.homeScreenKey(auth.createSession("198.51.100.41", "Old"),
                                                now - AuthManager::HOME_SCREEN_KEY_TTL_SECS - 5);
        CHECK(!auth.redeemHomeScreenKey(late, now).has_value());

        // Past half its life the manifest gets a fresh one.
        const QString aging = auth.createSession("198.51.100.42", "Aging");
        const QString first =
            auth.homeScreenKey(aging, now - AuthManager::HOME_SCREEN_KEY_TTL_SECS);
        CHECK(auth.homeScreenKey(aging, now) != first);

        // Revoking the session it came from revokes the key with it.
        auth.destroySession(auth.sessionIdForToken(owner));
        CHECK(!auth.redeemHomeScreenKey(second).has_value());

        // A visitor who declined "remember me" hands out no second session.
        const QString temporary = auth.createSession("198.51.100.43", "Kiosk", false, true);
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
}
