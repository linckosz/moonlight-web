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

#include "GameStreamBackend.h"
#include "IStreamBackend.h"

#include <QByteArray>
#include <QMap>
#include <QObject>
#include <QString>
#include <memory>

class NvPairingManager;
class WolfApiClient;

/**
 * @brief A Wolf host: GameStream media, Wolf's `/api/v1` for everything else.
 *
 * Wolf speaks standard GameStream once paired, so the media half is not
 * reimplemented — this composes a GameStreamBackend and forwards to it. What
 * Wolf changes is *identity* and *pairing*.
 *
 * **Identity.** Wolf keys a running session by client certificate, so two
 * players arriving under one identity would land in the same session: player B
 * would take over player A's screen. A seat here is therefore a client
 * identity, derived from the device session id and pinned into
 * LaunchRequest::clientUniqueId, which GameStreamBackend already honours.
 *
 * **Pairing.** Wolf parks phase 1 on an unresolved `user_pin` promise and never
 * answers until someone supplies the PIN. Since MoonlightWeb is the Moonlight
 * client, it knows the PIN it would have shown and posts it itself through
 * WolfApiClient — so ensurePaired() completes with no human in the loop, which
 * is the entire point of the integration.
 *
 * The app list deliberately stays the GameStream one. What Wolf advertises
 * there is Wolf UI, its console experience: profile picking, optional per
 * profile PIN, the Docker catalogue and lobbies. Substituting `/api/v1/apps`
 * would render a flat grid and silently drop all four.
 */
class WolfBackend : public QObject, public IStreamBackend
{
    Q_OBJECT

public:
    using HostResolver = GameStreamBackend::HostResolver;

    /// Persists a completed pairing. WolfBackend runs the handshake but owns no
    /// host bookkeeping: writing the certificate, flipping pairState and saving
    /// belong to ComputerManager, exactly as they do on the Sunshine path.
    using PairingCommit = std::function<void(const QByteArray& serverCertPem)>;

    WolfBackend(QString hostUuid, HostResolver resolver, NvHTTP* http, QNetworkAccessManager* nam,
                WolfApiClient* api, PairingCommit commit, QObject* parent = nullptr);
    ~WolfBackend() override;

    QString type() const override { return QStringLiteral("wolf"); }

    /// Concurrent independent sessions and native co-op, but no provisioning:
    /// profiles are created by users inside Wolf UI, not conjured by us.
    BackendCapabilities capabilities() const override
    {
        return BackendCapabilities{/*multiUser*/ true, /*provisioning*/ false, /*lobbies*/ true};
    }

    void ensurePaired(BackendVoidCallback cb) override;
    void listSeats(BackendSeatListCallback cb) override;
    void allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb) override;
    void releaseSeat(const QString& seatId) override;

    void getAppList(const QString& seatId, BackendAppListCallback cb) override;

    void launch(const QString& seatId, const LaunchRequest& req, BackendMediaCallback cb) override;
    void resume(const QString& seatId, const LaunchRequest& req, BackendMediaCallback cb) override;
    void quit(const QString& seatId, BackendVoidCallback cb) override;

    void provisionSeat(const QJsonObject& params, BackendSeatCallback cb) override;
    void teardownSeat(const QString& seatId, BackendVoidCallback cb) override;

private:
    /// Resolve the PIN into Wolf once stage 1 is parked on it. Polls
    /// /pair/pending because our GET can outrun Wolf registering the request,
    /// and picks the secret that was not already pending when we started.
    void announcePin(const QString& pin, const QSet<QString>& knownSecrets, int attemptsLeft);

    /// Wolf keys sessions by certificate, so a seat is an identity string. It
    /// must be stable for a given device session or the player loses their
    /// running session on reconnect.
    static QString seatIdFor(const QString& deviceSessionId);

    /// Copy of `req` with clientUniqueId forced to the seat identity.
    LaunchRequest withSeatIdentity(const QString& seatId, const LaunchRequest& req) const;

    QString m_HostUuid;
    HostResolver m_ResolveHost;
    WolfApiClient* m_Api = nullptr;
    PairingCommit m_Commit;

    // Media half. Owned, and every GameStream call is forwarded to it.
    std::unique_ptr<GameStreamBackend> m_GameStream;

    // Kept alive for the whole handshake: its async callbacks reference it.
    std::unique_ptr<NvPairingManager> m_Pairing;
    bool m_PairingInFlight = false;
};
