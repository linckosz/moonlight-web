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

#include "server/routes/BoothRoutes.h"

#include "common/Logger.h"
#include "server/BoothPin.h"
#include "server/OwnerProof.h"
#include "server/RestRouter.h"

#include <QCryptographicHash>
#include <QJsonDocument>
#include <QJsonObject>

#include <optional>

namespace {

HttpResponse answer(const QJsonObject& obj, int status = 200)
{
    HttpResponse resp = HttpResponse::json(obj, status);
    // A PIN, a link or a seat: nothing of it may be kept anywhere.
    resp.headers[QStringLiteral("Cache-Control")] = QStringLiteral("no-store");
    return resp;
}

HttpResponse refusal(const QString& error, int status)
{
    return answer(QJsonObject{{QStringLiteral("error"), error}}, status);
}

/// The proof check every PIN route asks for one action on one host's stream:
/// nullopt when it holds, the refusal to answer with otherwise.
std::optional<HttpResponse> proofRefusal(OwnerProof& proofs, const BoothRoutesDeps& deps,
                                         const HttpRequest& req, const QString& token,
                                         const QString& action, const QString& hostUuid,
                                         const QJsonObject& body)
{
    const QByteArray key = deps.pairingKey ? deps.pairingKey(token) : QByteArray();
    const OwnerProof::Result r = proofs.checkBooth(
        token, key, deps.hostKeyId ? deps.hostKeyId() : QString(), action, hostUuid,
        body.value(QStringLiteral("owner_proof")).toObject(), deps.nowMs ? deps.nowMs() : 0);
    switch (r) {
    case OwnerProof::Result::Ok: return std::nullopt;
    case OwnerProof::Result::NoKey: return refusal(QStringLiteral("pairing_key_required"), 403);
    case OwnerProof::Result::Missing: return refusal(QStringLiteral("owner_proof_required"), 403);
    case OwnerProof::Result::Refused:
        if (deps.reportAuthFailure) deps.reportAuthFailure(req.clientAddress);
        Logger::warning(QStringLiteral("[Booth] A '%1' request refused: its key proof does not "
                                       "hold")
                            .arg(action));
        return refusal(QStringLiteral("owner_proof_refused"), 403);
    }
    return refusal(QStringLiteral("owner_proof_refused"), 403);
}

/// The steps every PIN a caller enters goes through, in this order, nothing
/// further once one says no: the caller's buckets, its proof for @p action on
/// @p hostUuid, then the PIN of @p streamKey. A refusal before the PIN never
/// touches the PIN nor its count. nullopt: the PIN was the right one.
std::optional<HttpResponse> pinRefusal(BoothPin& pins, OwnerProof& proofs,
                                       const BoothRoutesDeps& deps, const HttpRequest& req,
                                       const QJsonObject& body, const QString& token,
                                       const QString& action, const QString& hostUuid,
                                       const QString& streamKey)
{
    const QString address = deps.addressKey ? deps.addressKey(req) : req.clientAddress;
    if (const int held = pins.heldFor(token, address); held > 0) {
        QJsonObject obj{{QStringLiteral("error"), QStringLiteral("rate_limited")},
                        {QStringLiteral("lockout_seconds"), held}};
        return answer(obj, 429);
    }
    if (auto refused = proofRefusal(proofs, deps, req, token, action, hostUuid, body))
        return refused;

    pins.retainOnly(deps.liveStreamKeys ? deps.liveStreamKeys() : QSet<QString>{streamKey});
    if (!pins.has(streamKey)) pins.mint(streamKey);
    const BoothPin::Outcome outcome =
        pins.check(streamKey, body.value(QStringLiteral("pin")).toString(), token, address);
    if (outcome.result == BoothPin::Result::RateLimited) {
        QJsonObject obj{{QStringLiteral("error"), QStringLiteral("rate_limited")},
                        {QStringLiteral("lockout_seconds"), outcome.lockoutSeconds}};
        return answer(obj, 429);
    }
    if (outcome.result != BoothPin::Result::Ok) {
        QJsonObject obj{{QStringLiteral("error"), QStringLiteral("booth_pin_invalid")},
                        {QStringLiteral("remaining"), outcome.remainingAttempts},
                        {QStringLiteral("lockout_seconds"), outcome.lockoutSeconds}};
        return answer(obj, 401);
    }
    return std::nullopt;
}

/// The stream a request names, or the refusal to answer with: the booth must
/// have its PINs on, a host be named, and an owner stream be up on it.
std::optional<HttpResponse> streamRefusal(const BoothRoutesDeps& deps, const HttpRequest& req,
                                          const QJsonObject& body, QString& hostUuid,
                                          BoothRoutesDeps::Stream& stream)
{
    // Off a booth, or a booth that did not ask for stream PINs: no such route.
    if (!req.boothPin) return refusal(QStringLiteral("not_found"), 404);
    hostUuid = body.value(QStringLiteral("host_uuid")).toString();
    if (hostUuid.isEmpty()) return refusal(QStringLiteral("host_required"), 400);
    const int appId = body.value(QStringLiteral("app_id")).toInt(-1);
    stream = deps.ownerStream ? deps.ownerStream(hostUuid, appId) : BoothRoutesDeps::Stream{};
    // A host that gives each device its own session never takes one over, and
    // has nothing a PIN would open: no PIN there, said plainly.
    if (!stream.supported) return refusal(QStringLiteral("booth_unsupported"), 409);
    if (!stream.live || stream.key.isEmpty()) return refusal(QStringLiteral("no_stream"), 409);
    return std::nullopt;
}

} // namespace

std::optional<HttpResponse> boothTakeoverRefusal(BoothPin& pins, OwnerProof& proofs,
                                                 const BoothRoutesDeps& deps,
                                                 const HttpRequest& req, const QJsonObject& body,
                                                 const BoothTakeover& target)
{
    // Asked first, and nothing counted: the page learns whose stream is in the
    // way and what it streams, and asks its user for the PIN.
    const auto required = [&target]() {
        QJsonObject obj{{QStringLiteral("error"), QStringLiteral("booth_pin_required")},
                        {QStringLiteral("owner"), target.ownerName},
                        {QStringLiteral("app_id"), target.appId}};
        return answer(obj, 409);
    };
    if (!body.contains(QStringLiteral("booth_pin"))) return required();
    // A stream that is not up (a start on its way, the stream this process
    // runs itself) has no PIN anybody can read: nothing to try, and nothing
    // counted against the caller. Asked again, it is the PIN of the stream
    // once it is up.
    const QSet<QString> live =
        deps.liveStreamKeys ? deps.liveStreamKeys() : QSet<QString>{target.streamKey};
    if (!live.contains(target.streamKey)) return required();
    const QString token = deps.sessionToken ? deps.sessionToken(req) : QString();
    if (token.isEmpty()) return refusal(QStringLiteral("booth_session_required"), 403);
    // The PIN under the name the join route reads it by.
    QJsonObject entered = body;
    entered[QStringLiteral("pin")] = body.value(QStringLiteral("booth_pin")).toString();
    if (auto refused =
            pinRefusal(pins, proofs, deps, req, entered, token, QStringLiteral("booth_takeover"),
                       target.hostUuid, target.streamKey))
        return refused;
    // Used: whoever read it before cannot use it again.
    pins.rotate(target.streamKey, BoothPin::Reason::Used);
    Logger::info(QStringLiteral("[Booth] The stream on host %1 taken over with its PIN")
                     .arg(target.hostUuid));
    return std::nullopt;
}

ShareManager::Permissions boothPermissionsFor(const QString& level)
{
    ShareManager::Permissions p;
    if (level == QLatin1String("gamer")) {
        p.gamepad = true;
    } else if (level == QLatin1String("desktop")) {
        p.keyboardMouse = true;
    } else if (level == QLatin1String("full")) {
        p.gamepad = true;
        p.keyboardMouse = true;
    }
    return p;
}

QString boothHolderOf(const QString& sessionToken)
{
    if (sessionToken.isEmpty()) return QString();
    return QString::fromLatin1(
        QCryptographicHash::hash(QByteArrayLiteral("MW-booth-holder|") + sessionToken.toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex());
}

void registerBoothRoutes(RestRouter& router, ShareManager& share, BoothPin& pins,
                         OwnerProof& proofs, BoothRoutesDeps deps)
{
    // POST /api/booth/join — {host_uuid, app_id?, pin, owner_proof}: a device
    // this booth knows joins the stream someone else is watching, as a guest.
    // In this order, and nothing further once one says no: whose stream it is,
    // the caller's buckets, its proof, then the PIN. A refusal before the PIN
    // never touches the PIN nor its count.
    router.post(
        QStringLiteral("/api/booth/join"), [&share, &pins, &proofs, deps](const HttpRequest& req) {
            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            QString hostUuid;
            BoothRoutesDeps::Stream stream;
            if (auto refused = streamRefusal(deps, req, body, hostUuid, stream)) return *refused;

            const QString token = deps.sessionToken ? deps.sessionToken(req) : QString();
            // A guest seat is held by a session: this machine without one, or a
            // caller with none, has nothing to hold it by.
            if (token.isEmpty()) return refusal(QStringLiteral("booth_session_required"), 403);
            const StreamOwner caller = StreamOwner::of(req.isThisMachine, token);
            if (stream.owner.sameAs(caller)) return refusal(QStringLiteral("own_stream"), 409);

            if (auto refused = pinRefusal(pins, proofs, deps, req, body, token,
                                          QStringLiteral("booth_join"), hostUuid, stream.key))
                return *refused;

            // One seat per session on a stream: the same one again, never a second.
            const QString holder = boothHolderOf(token);
            QString link;
            QString cookie;
            int slot = share.boothSeatOf(hostUuid, holder);
            if (slot >= 0) {
                if (!share.reissueBound(slot, link, cookie))
                    return refusal(QStringLiteral("room_full"), 503);
                Logger::info(QStringLiteral("[Booth] Slot %1 handed back to its holder on host %2")
                                 .arg(slot)
                                 .arg(hostUuid));
            } else {
                slot = share.freeSlot();
                // Room full: the PIN was right but not used, so it stays.
                if (slot < 0) return refusal(QStringLiteral("room_full"), 503);
                const QString level = deps.joinLevel ? deps.joinLevel() : QStringLiteral("viewer");
                QString label = deps.machineName ? deps.machineName(token) : QString();
                if (label.trimmed().isEmpty()) label = QStringLiteral("Guest %1").arg(slot - 1);
                if (!share.activateBound(slot, hostUuid, stream.appId, boothPermissionsFor(level),
                                         label, req.headers.value(QStringLiteral("user-agent")),
                                         link, cookie))
                    return refusal(QStringLiteral("room_full"), 503);
                share.markHolder(slot, holder);
                // Somebody new came in: the PIN they used is spent. A holder taking
                // their own seat back is nobody new, and leaves it as it was.
                pins.rotate(stream.key, BoothPin::Reason::Used);
            }

            QJsonObject obj;
            obj[QStringLiteral("status")] = QStringLiteral("ok");
            obj[QStringLiteral("slot")] = slot;
            obj[QStringLiteral("url")] = QStringLiteral("/p/") + link;
            HttpResponse resp = answer(obj);
            // As a guest's link sets it (ShareRoutes), for as long as this browser
            // runs: the seat itself ends with the stream.
            resp.headers[QStringLiteral("Set-Cookie")] =
                QStringLiteral("%1=%2; HttpOnly; Secure; Path=/; SameSite=Strict")
                    .arg(QLatin1String(ShareManager::kPlayerCookie), cookie);
            return resp;
        });

    // POST /api/booth/pin — {host_uuid, app_id?, action: "read" | "new",
    // owner_proof}: the PIN of the stream the caller is watching, or a new one.
    // The one who started that stream only; a guest reads it through its own
    // presence answer, never here.
    router.post(QStringLiteral("/api/booth/pin"), [&pins, &proofs, deps](const HttpRequest& req) {
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        QString hostUuid;
        BoothRoutesDeps::Stream stream;
        if (auto refused = streamRefusal(deps, req, body, hostUuid, stream)) return *refused;
        const QString action = body.value(QStringLiteral("action")).toString();
        if (action != QLatin1String("read") && action != QLatin1String("new"))
            return refusal(QStringLiteral("action_unknown"), 400);

        const QString token = deps.sessionToken ? deps.sessionToken(req) : QString();
        const StreamOwner caller = StreamOwner::of(req.isThisMachine, token);
        if (!stream.owner.sameAs(caller)) return refusal(QStringLiteral("booth_owner_only"), 403);
        if (caller.kind != StreamOwner::Kind::ThisMachine) {
            if (auto refused =
                    proofRefusal(proofs, deps, req, token,
                                 action == QLatin1String("new") ? QStringLiteral("booth_pin_new")
                                                                : QStringLiteral("booth_pin_read"),
                                 hostUuid, body))
                return *refused;
        }

        pins.retainOnly(deps.liveStreamKeys ? deps.liveStreamKeys() : QSet<QString>{stream.key});
        if (!pins.has(stream.key)) pins.mint(stream.key);
        if (action == QLatin1String("new")) {
            pins.rotate(stream.key, BoothPin::Reason::Asked);
            Logger::info(QStringLiteral("[Booth] A new PIN for the stream of host %1, at its "
                                        "owner's request")
                             .arg(hostUuid));
        }
        QJsonObject obj;
        obj[QStringLiteral("pin")] = pins.read(stream.key);
        obj[QStringLiteral("generation")] = static_cast<qint64>(pins.generation(stream.key));
        obj[QStringLiteral("changed_by_failures")] =
            pins.lastReason(stream.key) == BoothPin::Reason::Failures;
        return answer(obj);
    });
}
