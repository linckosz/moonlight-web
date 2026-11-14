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

#include "server/routes/LaunchGate.h"

#include "common/Logger.h"

#include <QJsonDocument>

#include <utility>

namespace {

/// The slot a request names, -1 when it names none: signed as sent.
int namedSlot(const QJsonObject& body)
{
    return body.contains(QStringLiteral("session_slot"))
               ? body.value(QStringLiteral("session_slot")).toInt(-1)
               : -1;
}

HttpResponse refusedAnswer(const QString& code)
{
    QJsonObject obj;
    obj[QStringLiteral("error")] =
        QStringLiteral("This browser is not the one this session was paired with");
    obj[QStringLiteral("code")] = code;
    return HttpResponse::json(obj, 409);
}

} // namespace

LaunchGate::LaunchGate(LaunchGateDeps deps)
    : m_Deps(std::move(deps))
    , m_Launches(QByteArray(kLaunchLabel),
                 OwnerProof::Limits{kProofLifetimeMs, OwnerProof::kPerSession, OwnerProof::kTotal})
    , m_Tickets(QByteArray(kQuitLabel),
                OwnerProof::Limits{kTicketLifetimeMs, kTicketsPerSession, OwnerProof::kTotal})
{}

QList<QByteArray> LaunchGate::startFields(const QString& hostUuid, const QJsonObject& body)
{
    return {QByteArrayLiteral("start"), hostUuid.toUtf8(),
            QByteArray::number(body.value(QStringLiteral("appId")).toInt(0)),
            QByteArray(body.value(QStringLiteral("standby")).toBool(false) ? "1" : "0"),
            QByteArray::number(namedSlot(body))};
}

QList<QByteArray> LaunchGate::quitFields(const QString& hostUuid, const QJsonObject& body)
{
    return {QByteArrayLiteral("quit"), hostUuid.toUtf8(), QByteArray::number(namedSlot(body))};
}

QList<QByteArray> LaunchGate::stopFields(const QString& hostUuid)
{
    return {QByteArrayLiteral("stop"), hostUuid.toUtf8()};
}

QList<QByteArray> LaunchGate::ticketFields(const QString& hostUuid, int slot,
                                           const QString& streamId)
{
    return {hostUuid.toUtf8(), QByteArray::number(slot), streamId.toUtf8()};
}

std::optional<HttpResponse> LaunchGate::refusal(const HttpRequest& req, const char* what,
                                                OwnerProof::Result result, bool badSignature,
                                                bool countStale)
{
    if (result == OwnerProof::Result::Ok) return std::nullopt;
    if (result == OwnerProof::Result::Missing) {
        Logger::warning(
            QStringLiteral("[Launch] %1 refused: launch_proof_required").arg(QLatin1String(what)));
        return refusedAnswer(QStringLiteral("launch_proof_required"));
    }
    if ((badSignature || countStale) && m_Deps.refused) m_Deps.refused(req.clientAddress);
    Logger::warning(QStringLiteral("[Launch] %1 refused: key_refused").arg(QLatin1String(what)));
    return refusedAnswer(QStringLiteral("key_refused"));
}

void LaunchGate::registerRoutes(RestRouter& router)
{
    // POST /api/hosts/:id/launch-proof — a challenge for the next launch, Stop
    // or stop-session of this session, when it has a key to prove:
    // {required:false} otherwise, and the page signs nothing.
    router.post(QStringLiteral("/api/hosts/:id/launch-proof"), [this](const HttpRequest& req) {
        QJsonObject obj;
        const QByteArray key = m_Deps.proofKey ? m_Deps.proofKey(req) : QByteArray();
        obj[QStringLiteral("required")] = !key.isEmpty();
        if (key.isEmpty()) return HttpResponse::json(obj);
        const QByteArray nonce = m_Launches.issue(m_Deps.session(req), m_Deps.now());
        if (nonce.isEmpty()) return HttpResponse::error(503, "Too many requests in flight");
        obj[QStringLiteral("nonce")] = QString::fromLatin1(nonce.toBase64());
        obj[QStringLiteral("host_id")] = m_Deps.hostKeyId ? m_Deps.hostKeyId() : QString();
        return HttpResponse::json(obj);
    });

    // POST /api/hosts/:id/quit-ticket {session_slot, stream_id} — the challenge
    // a page signs as its stream starts, for the Stop its closing sends: good
    // for that one stream, which this session's slot must carry now.
    router.post(QStringLiteral("/api/hosts/:id/quit-ticket"), [this](const HttpRequest& req) {
        QJsonObject obj;
        const QByteArray key = m_Deps.proofKey ? m_Deps.proofKey(req) : QByteArray();
        obj[QStringLiteral("required")] = !key.isEmpty();
        if (key.isEmpty()) return HttpResponse::json(obj);
        const QJsonObject body = QJsonDocument::fromJson(req.body).object();
        const QString session = m_Deps.session(req);
        const int slot = namedSlot(body);
        const QString streamId = body.value(QStringLiteral("stream_id")).toString();
        const QString live =
            slot >= 0 && m_Deps.streamOn
                ? m_Deps.streamOn(req.pathParams.value(QStringLiteral("id")), slot, session)
                : QString();
        if (streamId.isEmpty() || live != streamId) {
            Logger::info(QStringLiteral("[Launch] No quit ticket: that slot carries no such "
                                        "stream of this session"));
            QJsonObject no;
            no[QStringLiteral("error")] = QStringLiteral("No such stream");
            no[QStringLiteral("code")] = QStringLiteral("no_such_stream");
            return HttpResponse::json(no, 409);
        }
        const QByteArray nonce = m_Tickets.issue(session, m_Deps.now(), streamId);
        if (nonce.isEmpty()) {
            Logger::info(QStringLiteral("[Launch] No quit ticket: too many in flight"));
            return HttpResponse::error(503, "Too many requests in flight");
        }
        obj[QStringLiteral("nonce")] = QString::fromLatin1(nonce.toBase64());
        obj[QStringLiteral("host_id")] = m_Deps.hostKeyId ? m_Deps.hostKeyId() : QString();
        return HttpResponse::json(obj);
    });
}

AsyncRouteHandler LaunchGate::guardStart(AsyncRouteHandler inner)
{
    return [this, inner = std::move(inner)](const HttpRequest& req, ResponseCallback respond) {
        const QByteArray key = m_Deps.proofKey ? m_Deps.proofKey(req) : QByteArray();
        if (!key.isEmpty()) {
            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            bool bad = false;
            const OwnerProof::Result r = m_Launches.checkFields(
                m_Deps.session(req), key, m_Deps.hostKeyId ? m_Deps.hostKeyId() : QString(),
                startFields(req.pathParams.value(QStringLiteral("id")), body),
                body.value(QStringLiteral("launch_proof")).toObject(), m_Deps.now(), QString(),
                &bad);
            if (auto refused = refusal(req, "Launch", r, bad, true)) {
                respond(std::move(*refused));
                return;
            }
        }
        inner(req, std::move(respond));
    };
}

AsyncRouteHandler LaunchGate::guardStop(AsyncRouteHandler inner)
{
    return [this, inner = std::move(inner)](const HttpRequest& req, ResponseCallback respond) {
        const QByteArray key = m_Deps.proofKey ? m_Deps.proofKey(req) : QByteArray();
        if (!key.isEmpty()) {
            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            bool bad = false;
            const OwnerProof::Result r = m_Launches.checkFields(
                m_Deps.session(req), key, m_Deps.hostKeyId ? m_Deps.hostKeyId() : QString(),
                stopFields(req.pathParams.value(QStringLiteral("id"))),
                body.value(QStringLiteral("launch_proof")).toObject(), m_Deps.now(), QString(),
                &bad);
            if (auto refused = refusal(req, "Stop of the host's session", r, bad, true)) {
                respond(std::move(*refused));
                return;
            }
        }
        inner(req, std::move(respond));
    };
}

AsyncRouteHandler LaunchGate::guardQuit(AsyncRouteHandler inner)
{
    return [this, inner = std::move(inner)](const HttpRequest& req, ResponseCallback respond) {
        const QByteArray key = m_Deps.proofKey ? m_Deps.proofKey(req) : QByteArray();
        if (!key.isEmpty()) {
            const QJsonObject body = QJsonDocument::fromJson(req.body).object();
            const QString session = m_Deps.session(req);
            const QString hostUuid = req.pathParams.value(QStringLiteral("id"));
            const QString hostId = m_Deps.hostKeyId ? m_Deps.hostKeyId() : QString();
            const QJsonObject ticket = body.value(QStringLiteral("quit_ticket")).toObject();
            bool bad = false;
            OwnerProof::Result r;
            if (!ticket.isEmpty()) {
                // The closing page's ticket: good for the stream it was drawn
                // for, while this session's slot still carries it.
                const int slot = namedSlot(body);
                const QString live = slot >= 0 && m_Deps.streamOn
                                         ? m_Deps.streamOn(hostUuid, slot, session)
                                         : QString();
                r = m_Tickets.checkFields(session, key, hostId, ticketFields(hostUuid, slot, live),
                                          ticket, m_Deps.now(), live, &bad);
                // A ticket with no stream to end left was either spent or
                // never good: refused like any other, never matched to ''.
                if (live.isEmpty() && r == OwnerProof::Result::Ok) r = OwnerProof::Result::Refused;
            } else {
                r = m_Launches.checkFields(session, key, hostId, quitFields(hostUuid, body),
                                           body.value(QStringLiteral("launch_proof")).toObject(),
                                           m_Deps.now(), QString(), &bad);
            }
            // A closing page and its own Stop can race for one stream: only a
            // signature that does not verify counts against the caller.
            if (auto refused = refusal(req, "Stop", r, bad, false)) {
                respond(std::move(*refused));
                return;
            }
        }
        inner(req, std::move(respond));
    };
}

void LaunchGate::streamEnded(const QString& streamId)
{
    m_Tickets.dropScope(streamId);
}
