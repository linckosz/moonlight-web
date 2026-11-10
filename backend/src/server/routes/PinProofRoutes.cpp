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

#include "server/routes/PinProofRoutes.h"

#include "server/AuthManager.h"
#include "server/RequestGuard.h"
#include "server/RestRouter.h"
#include "server/SessionCookie.h"
#include "network/GeoIpService.h"
#include "common/PairingCrypto.h"
#include "common/PinBinding.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>

#include <memory>
#include <optional>
#include <utility>

namespace {

/// A PIN refused, by the shape /api/auth/validate has always answered.
HttpResponse refusedPin(const AuthManager::ValidateResult& result)
{
    QJsonObject obj;
    obj["status"] = "error";
    if (result.result == AuthManager::RateLimited) {
        obj["error"] = "rate_limited";
        obj["lockout_seconds"] = result.lockoutSeconds;
        return HttpResponse::json(obj, 429);
    }
    obj["error"] = "invalid_pin";
    obj["remaining"] = result.remainingAttempts;
    obj["lockout_seconds"] = result.lockoutSeconds;
    return HttpResponse::json(obj, 401);
}

/// Whether the client sees the TLS key this socket was served: a TLS socket, not
/// the tunnel, and no proxy in between. A peer on this machine (Tailscale Funnel,
/// cloudflared) or a request a proxy forwarded came through something that ends
/// TLS with a key of its own: a PIN bound to this socket's key could never match
/// the client's, and a proof of it would vouch for a key the client never saw.
/// Such a client is answered as an older host answers it, 404. A machine in the
/// middle gains nothing by writing those headers: it can answer 404 itself.
bool servedKeyIsSeen(const HttpRequest& req)
{
    if (req.viaTunnel || req.tlsSpki.isEmpty()) return false;
    if (QHostAddress(AuthManager::cleanClientAddress(req.clientAddress)).isLoopback()) return false;
    return !RequestGuard::forwardedByProxy(req.headers);
}

/// The SHA-256 of a TLS key's SubjectPublicKeyInfo.
QByteArray tlsKeyDigest(const QByteArray& spki)
{
    return QCryptographicHash::hash(spki, QCryptographicHash::Sha256);
}

/// How both routes name a TLS key to the client: base64url, unpadded.
QString tlsKeyText(const QByteArray& digest)
{
    return QString::fromLatin1(
        digest.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

/// The longest nonce MW-TLS-v1 signs: room for any client's, and no more.
constexpr int MAX_TLS_PROOF_NONCE_BYTES = 64;

/// The TLS proofs one caller may ask per minute. A client asks once per key
/// that changed; the budget is there so that nobody makes this host sign in a
/// loop.
constexpr int TLS_PROOFS_PER_MINUTE = 10;

} // namespace

void registerPinProofRoutes(RestRouter& router, AuthManager& authManager,
                            GeoIpService& geoIpService, const QByteArray& hostKeyPem)
{
    // The host key, read once: these routes are public, and they read no file
    // of their own on a call. /finish answers with the same identity as /start.
    const QByteArray hostPem = hostKeyPem;
    const QByteArray hostKey = PairingCrypto::publicKeySpkiFromPrivatePem(hostPem);
    const QString hostId = PairingCrypto::keyId(hostKey);

    // GET /api/auth/tls-proof?nonce=<base64url> — the TLS key this connection was
    // served, vouched for by the host key.
    //
    // A client that pinned this host's TLS key on first contact (the Android TV
    // app) asks it when the key it is shown is not the one it pinned: a deleted
    // local-key.pem, an owner's certificate renewed with a new key. With the host
    // key it was given at pairing, it checks the signature over its own nonce and
    // the key of this very connection, then follows the new key without a PIN.
    // Public like the PIN route: the client sends nothing of its session to a key
    // it does not recognise yet. A few proofs per caller and per minute, then 429
    // (TLS_PROOFS_PER_MINUTE).
    //
    // Never a key the caller names: what is signed is req.tlsSpki, set by
    // HttpServer from the certificate the socket was served. A machine in the
    // middle that relays the question gets this host's key signed, not its own.
    // Nothing to prove on the tunnel, in plain HTTP or behind a proxy, hence the
    // 404 there (servedKeyIsSeen).
    auto proofBudget = std::make_shared<PinBinding::CallBudget>(TLS_PROOFS_PER_MINUTE);
    router.get("/api/auth/tls-proof", [hostPem, hostKey, hostId,
                                       proofBudget](const HttpRequest& req) {
        if (!servedKeyIsSeen(req))
            return HttpResponse::error(404, "No TLS key of this host on this connection");
        const QByteArray nonce =
            QByteArray::fromBase64(req.queryParams.value(QStringLiteral("nonce")).toLatin1(),
                                   QByteArray::Base64UrlEncoding);
        if (nonce.size() < PairingCrypto::MIN_NONCE_BYTES ||
            nonce.size() > MAX_TLS_PROOF_NONCE_BYTES)
            return HttpResponse::error(400, "A fresh nonce of 16 to 64 bytes is needed");
        const QString bucket =
            AuthManager::rateLimitKey(AuthManager::attemptAddress(req.clientAddress, req.headers));
        if (!proofBudget->spend(bucket, QDateTime::currentSecsSinceEpoch()))
            return HttpResponse::error(429, "Too many proofs asked: try again in a minute");

        const QByteArray tlsKey = tlsKeyDigest(req.tlsSpki);
        const QByteArray sig =
            PairingCrypto::sign(hostPem, PairingCrypto::tlsProofInput(hostId, nonce, tlsKey));
        if (hostKey.isEmpty() || sig.isEmpty()) return HttpResponse::error(500, "Cannot sign");

        QJsonObject obj;
        obj["protocol"] = QString::fromLatin1(PairingCrypto::TLS_PROOF_PROTOCOL);
        obj["host_id"] = hostId;
        obj["host_public_key"] = QString::fromLatin1(hostKey.toBase64());
        obj["tls_key"] = tlsKeyText(tlsKey);
        obj["sig"] = QString::fromLatin1(sig.toBase64());
        return HttpResponse::json(obj);
    });

    // The runs between the PIN proof's two rounds: in memory, single use.
    auto pinRuns = std::make_shared<PinBinding::Exchanges>();

    // POST /api/auth/pin/start — MW-PIN-v1, first round: body {protocol,
    // public_key, pa}.
    //
    // The PIN proved by SPAKE2 (RFC 9382) and never sent, bound to the channel the
    // client is on as this end sees it: the TLS key this socket was served, or
    // through the tunnel the host key that opened the line. The client binds the
    // TLS key it sees: a machine in the middle relaying both rounds leaves the two
    // ends with confirmations that do not match, and the run proves nothing.
    //
    // The run is the attempt, counted here on the typed PIN's bucket and tiers
    // (AuthManager::beginPinProof), and /finish only clears it on a proof. Nothing
    // this round answers may depend on the PIN: for pA = w·M with the right w the
    // shared element falls on the identity, and a different answer there would name
    // the PIN in a million uncounted requests. Such a run is answered like any
    // other and can never prove. A connection with nothing to bind to (plain HTTP,
    // a proxy in front: servedKeyIsSeen) answers 404, as a host without this route
    // does. A full table of runs answers 503 before anything is counted, and no
    // run already kept gives way to a new caller's. The answer names the TLS key
    // this socket served (tls_key): a client that sees another key stops there.
    router.post("/api/auth/pin/start", [&authManager, hostKey, hostId,
                                        pinRuns](const HttpRequest& req) {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString protocol = body.value(QStringLiteral("protocol")).toString();
        if (protocol != QLatin1String(PinBinding::PROTOCOL))
            return HttpResponse::error(400, "Unknown protocol");
        const QByteArray clientKey =
            QByteArray::fromBase64(body.value(QStringLiteral("public_key")).toString().toLatin1());
        if (!PairingCrypto::isValidP256Spki(clientKey))
            return HttpResponse::error(400, "Missing or invalid 'public_key'");
        // Through the tunnel the line's hello already named the client's key: the
        // pairing is for that key, or the session would be refused on that line.
        if (req.viaTunnel && !req.tunnelKey.isEmpty() && req.tunnelKey != clientKey)
            return HttpResponse::error(400, "'public_key' is not this line's key");
        // Whether pA is a point of the group does not depend on the PIN.
        const QByteArray pA =
            QByteArray::fromBase64(body.value(QStringLiteral("pa")).toString().toLatin1());
        if (!PinBinding::isElement(pA)) return HttpResponse::error(400, "Missing or invalid 'pa'");

        QByteArray channel;
        if (req.viaTunnel)
            channel = PinBinding::lineChannel(hostKey);
        else if (servedKeyIsSeen(req))
            channel = PinBinding::tlsChannel(req.tlsSpki);
        if (channel.isEmpty()) return HttpResponse::error(404, "No channel here to bind a PIN to");
        if (hostKey.isEmpty()) return HttpResponse::error(500, "No host key");

        const QString ip = AuthManager::attemptAddress(req.clientAddress, req.headers);
        const QString bucket = AuthManager::rateLimitKey(ip);
        if (!pinRuns->hasRoom(bucket, QDateTime::currentSecsSinceEpoch()))
            return HttpResponse::error(503, "Too many PIN proofs under way: try again shortly");
        const AuthManager::ValidateResult admitted = authManager.beginPinProof(ip);
        if (admitted.result == AuthManager::RateLimited) return refusedPin(admitted);

        // The PIN as it stands; /finish refuses the run if it has changed since.
        const QString pin = authManager.currentPin();
        const QByteArray w = PinBinding::pinScalar(pin);
        const std::optional<PinBinding::Share> share = PinBinding::shareB(w);
        if (!share) return HttpResponse::error(500, "Internal error");
        const std::optional<PinBinding::Keys> keys = PinBinding::finishB(
            w, *share, pA, clientKey, hostKey, PinBinding::associatedData(channel));

        PinBinding::Exchanges::Pending run;
        run.bucket = bucket;
        run.pin = pin;
        run.clientKey = clientKey;
        if (keys) {
            run.confirmA = keys->confirmA;
            run.confirmB = keys->confirmB;
        } else {
            // K on the identity: the same answer as any run, and no proof ever.
            run.provable = false;
        }
        const QString id = pinRuns->add(std::move(run), QDateTime::currentSecsSinceEpoch());
        if (id.isEmpty()) return HttpResponse::error(500, "Internal error");

        QJsonObject obj;
        obj["protocol"] = QString::fromLatin1(PinBinding::PROTOCOL);
        obj["exchange"] = id;
        obj["pb"] = QString::fromLatin1(share->element.toBase64());
        obj["host_public_key"] = QString::fromLatin1(hostKey.toBase64());
        obj["host_id"] = hostId;
        // As /api/auth/tls-proof names it. The tunnel has no TLS key of this host.
        if (!req.viaTunnel) obj["tls_key"] = tlsKeyText(tlsKeyDigest(req.tlsSpki));
        return HttpResponse::json(obj);
    });

    // POST /api/auth/pin/finish — MW-PIN-v1, second round: body {exchange,
    // confirm, machine_name, remember}.
    //
    // The client's confirmation. A good one clears the count its run began with
    // and creates the session exactly as /api/auth/validate does — the key of the
    // run bound to it, the PIN spent — and answers this host's confirmation, by
    // which the client checks that it was this host, on its channel, that knew the
    // PIN. A wrong one is 401 like a wrong PIN, not counted twice. A run unknown,
    // spent, expired or begun from another address is 410: the client starts
    // again.
    router.post("/api/auth/pin/finish", [&authManager, &geoIpService, hostKey, hostId,
                                         pinRuns](const HttpRequest& req) {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString ip = AuthManager::attemptAddress(req.clientAddress, req.headers);
        const std::optional<PinBinding::Exchanges::Pending> run =
            pinRuns->take(body.value(QStringLiteral("exchange")).toString(),
                          AuthManager::rateLimitKey(ip), QDateTime::currentSecsSinceEpoch());
        if (!run) {
            QJsonObject obj;
            obj["status"] = "error";
            obj["error"] = "exchange_unknown";
            return HttpResponse::json(obj, 410);
        }

        const QByteArray confirm =
            QByteArray::fromBase64(body.value(QStringLiteral("confirm")).toString().toLatin1());
        const bool confirmed = run->provable && !run->confirmA.isEmpty() &&
                               PinBinding::sameBytes(confirm, run->confirmA);
        const AuthManager::ValidateResult result =
            authManager.finishPinProof(ip, run->pin, confirmed);
        if (result.result != AuthManager::Valid) return refusedPin(result);

        const bool remember = wantsRemember(body);
        const QString machineName = body.value(QStringLiteral("machine_name")).toString();
        const QString token =
            authManager.createSession(req.clientAddress, machineName, false, !remember);
        geoIpService.lookupIp(req.clientAddress,
                              [&authManager, token](const QString& city, const QString& country) {
                                  authManager.setSessionGeo(token, city, country);
                              });
        authManager.autoRegeneratePin();

        QJsonObject obj;
        obj["status"] = "ok";
        obj["protocol"] = QString::fromLatin1(PinBinding::PROTOCOL);
        obj["pin_regenerated"] = true;
        obj["remembered"] = remember;
        obj["confirm"] = QString::fromLatin1(run->confirmB.toBase64());
        // The key the run was made with, never one this body could name, under the
        // host identity /start answered with.
        if (authManager.bindSessionKey(token, run->clientKey)) {
            obj["pairing_key"] = "bound";
            obj["host_public_key"] = QString::fromLatin1(hostKey.toBase64());
            obj["host_id"] = hostId;
        } else {
            obj["pairing_key"] = "rejected";
        }
        HttpResponse resp = HttpResponse::json(obj);
        resp.headers["Set-Cookie"] = sessionCookie(token, remember);
        return resp;
    });
}
