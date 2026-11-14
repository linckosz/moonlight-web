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

#pragma once

#include "common/Types.h"
#include "server/OwnerProof.h"
#include "server/RestRouter.h"

#include <QByteArray>
#include <QJsonObject>
#include <QList>
#include <QString>

#include <functional>
#include <optional>

/// What the gate takes from the server around it, as functions: the tests stand
/// in for the sessions, the pool and the clock.
struct LaunchGateDeps
{
    /// The session a request comes from (its mw_session cookie), empty when none.
    std::function<QString(const HttpRequest& req)> session;
    /// The pairing key a request must prove it holds: the session's own, off the
    /// tunnel and off a TV's plain HTTP. Empty when there is nothing to prove —
    /// the tunnel's line proved it already, a TV has a gate of its own, and a
    /// session paired before MW-BIND-v1 has no key.
    std::function<QByteArray(const HttpRequest& req)> proofKey;
    /// This host's MW-BIND-v1 identity, as PairingCrypto::keyId gives it.
    std::function<QString()> hostKeyId;
    /// A proof that does not hold came from @p address (HttpServer::reportAuthFailure).
    std::function<void(const QString& address)> refused;
    /// The stream slot @p slot carries on @p hostUuid for @p session right now
    /// (its StreamEnds id), empty when it carries none of that session's there.
    std::function<QString(const QString& hostUuid, int slot, const QString& session)> streamOn;
    /// Milliseconds since the epoch.
    std::function<qint64()> now;
};

/**
 * A stream is started or ended only by the browser its session was paired with.
 *
 * The cookie is all a request carries off the tunnel; a launch, a Stop, and the
 * host's escape hatch (stop-session) therefore carry a signature too, by the
 * session's pairing key, over a single-use challenge the host drew a moment
 * before (POST /api/hosts/:id/launch-proof) and over what the request does. The
 * page that closes cannot wait for a challenge: as its stream starts, it signs a
 * quit ticket for that one stream (POST /api/hosts/:id/quit-ticket), which its
 * closing sends; the ticket goes with its stream.
 *
 * The gate wraps each route's whole handler: a request it refuses never reaches
 * it, so nothing of the host is touched — no stream ended, no app launched.
 *
 * Main thread only, like the routes it wraps.
 */
class LaunchGate
{
public:
    static constexpr char kLaunchLabel[] = "MW-BIND-v1|launch";
    static constexpr char kQuitLabel[] = "MW-BIND-v1|quit";
    /// The page asks right before its request.
    static constexpr qint64 kProofLifetimeMs = 30 * 1000;
    /// A ticket goes with its stream; past this, with a stream forgotten too.
    static constexpr qint64 kTicketLifetimeMs = 12LL * 3600 * 1000;
    /// A page's two legs, and a stream of this device in an app slot.
    static constexpr int kTicketsPerSession = 4;

    explicit LaunchGate(LaunchGateDeps deps);

    /// POST /api/hosts/:id/launch-proof and POST /api/hosts/:id/quit-ticket.
    void registerRoutes(RestRouter& router);

    /// The handlers of /api/hosts/:id/start, /quit and /stop-session, behind
    /// the gate.
    AsyncRouteHandler guardStart(AsyncRouteHandler inner);
    AsyncRouteHandler guardQuit(AsyncRouteHandler inner);
    AsyncRouteHandler guardStop(AsyncRouteHandler inner);

    /// The stream @p streamId ended, however it did: its quit ticket goes too.
    void streamEnded(const QString& streamId);

    /// What each request signs after the label, the host's identity and the
    /// nonce: frontend/js/util/pairingCrypto.js builds the same, field for field.
    static QList<QByteArray> startFields(const QString& hostUuid, const QJsonObject& body);
    static QList<QByteArray> quitFields(const QString& hostUuid, const QJsonObject& body);
    static QList<QByteArray> stopFields(const QString& hostUuid);
    static QList<QByteArray> ticketFields(const QString& hostUuid, int slot,
                                          const QString& streamId);

    int pendingProofs() const { return m_Launches.pendingCount(); }
    int pendingTickets() const { return m_Tickets.pendingCount(); }

private:
    /// nullopt when the request may go on, else the refusal to answer with.
    /// @p countStale: whether a challenge the store does not hold counts against
    /// the caller like a signature that does not verify.
    std::optional<HttpResponse> refusal(const HttpRequest& req, const char* what,
                                        OwnerProof::Result result, bool badSignature,
                                        bool countStale);

    LaunchGateDeps m_Deps;
    OwnerProof m_Launches;
    OwnerProof m_Tickets;
};
