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
 * **Identity — incomplete, read this before relying on it.** Wolf resolves the
 * client from its *TLS certificate* (rest/servers.cpp get_client_if_paired →
 * get_client_cert), and keys a running session on it
 * (get_session_by_client). The uniqueid only ever keys the pairing cache
 * (uniqueid + "@" + client_ip).
 *
 * seatIdFor() therefore pins LaunchRequest::clientUniqueId, which is *not* what
 * Wolf keys sessions on. As long as MoonlightWeb presents one global client
 * certificate (IdentityManager is a single-identity singleton), every seat looks
 * like the same Wolf client and a second player's launch resumes the first
 * player's session instead of starting their own.
 *
 * Real isolation needs one certificate per seat — cheap to pair now that
 * pairing needs no human, and Session.cpp already passes clientCert/clientKey
 * per call. Until that lands, treat this backend as single-player.
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

    /// @param apiUrl   Base URL of the authenticated proxy fronting Wolf's Unix
    ///                 socket, e.g. "http://192.168.1.50:8080".
    /// @param apiToken Bearer token that proxy expects; may be empty when the
    ///                 proxy authenticates some other way.
    WolfBackend(QString hostUuid, HostResolver resolver, NvHTTP* http, QNetworkAccessManager* nam,
                const QString& apiUrl, const QString& apiToken, PairingCommit commit,
                QObject* parent = nullptr);
    ~WolfBackend() override;

    QString type() const override { return QStringLiteral("wolf"); }

    /// Native co-op, but no provisioning: profiles are created by users inside
    /// Wolf UI, not conjured by us.
    ///
    /// multiUser stays false until seats get their own client certificates.
    /// Claiming it now would invite callers to hand out concurrent seats that
    /// Wolf would collapse onto one session — see the identity note above.
    BackendCapabilities capabilities() const override
    {
        return BackendCapabilities{/*multiUser*/ false, /*provisioning*/ false, /*lobbies*/ true};
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

    /// A stable per-device-session identity string. Stable matters: a returning
    /// device must land on its own seat rather than a fresh one.
    ///
    /// This currently only reaches Wolf as the pairing-cache uniqueid, not as
    /// the session key — see the identity note on the class.
    static QString seatIdFor(const QString& deviceSessionId);

    /// Copy of `req` with clientUniqueId forced to the seat identity.
    LaunchRequest withSeatIdentity(const QString& seatId, const LaunchRequest& req) const;

    QString m_HostUuid;
    HostResolver m_ResolveHost;
    PairingCommit m_Commit;

    // Owned through the QObject parent-child relation (parent is this).
    WolfApiClient* m_Api = nullptr;

    // Media half. Owned, and every GameStream call is forwarded to it.
    std::unique_ptr<GameStreamBackend> m_GameStream;

    // Kept alive for the whole handshake: its async callbacks reference it.
    std::unique_ptr<NvPairingManager> m_Pairing;
    bool m_PairingInFlight = false;
};
