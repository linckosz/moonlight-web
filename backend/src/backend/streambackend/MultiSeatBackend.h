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

#include "../MultiSeatApiClient.h"
#include "../NvComputer.h"
#include "IStreamBackend.h"

#include <QMap>
#include <QSet>
#include <QObject>
#include <QString>
#include <memory>

class GameStreamBackend;
class NvPairingManager;
class SunshineRestClient;
class NvHTTP;
class QNetworkAccessManager;

/**
 * @brief A MultiSeat host: many seats on one Windows machine.
 *
 * MultiSeat splits one Windows box into several independent GameStream hosts.
 * A seat is a Windows account with a virtual display, its own audio device and
 * **its own Apollo instance**, reachable at `<host>:<portBase>` — so the seats
 * are ordinary GameStream endpoints and nothing new is needed to stream them.
 *
 * This class therefore does the part NvHTTP cannot: enumerate seats, provision
 * and tear them down, and pick which seat a device session gets. It is the
 * first backend with provisioning, and unlike Wolf it needs no pairing of its
 * own — the control API authenticates with a key, so ensurePaired() only has to
 * prove that key works.
 *
 * **Per-seat GameStream.** getAppList/launch/resume/quit must reach a *seat's*
 * Apollo rather than the machine's own address, so each seat gets a synthetic
 * host record (the machine's address, the seat's GFE ports) and its own
 * GameStreamBackend over it. Each Apollo keeps its own client list, so each
 * seat is paired separately — with its own certificate, so seats stay distinct
 * clients, and with the PIN pushed to that seat's Apollo web UI, since
 * MultiSeat's API can unpair a client but never pair one.
 *
 * ⚠️ Written against MultiSeat's source and NOT exercised: provisioning a seat
 * needs a free Windows session, which the bench cannot offer (Windows Pro
 * allows one session, console or remote). Treat the per-seat stream path as
 * unverified until a seat has actually reached Ready.
 */
class MultiSeatBackend : public QObject, public IStreamBackend
{
    Q_OBJECT

public:
    using HostResolver = std::function<NvComputer*()>;

    /// @param apiUrl Base URL of the MultiSeat service, e.g.
    ///               "http://192.168.1.9:9550".
    /// @param apiKey Value for the X-MultiSeat-Key header.
    /// @param pairUser/pairPassword Credentials for each seat's Apollo web UI.
    ///        MultiSeat's own API can read and unpair clients but never pair
    ///        one, so a seat is paired by pushing the PIN there — and MultiSeat
    ///        does not generate those credentials, it expects an admin to have
    ///        set them. One pair covers every seat, since they share a
    ///        credentials file.
    MultiSeatBackend(QString hostUuid, HostResolver resolver, NvHTTP* http,
                     QNetworkAccessManager* nam, const QString& apiUrl, const QString& apiKey,
                     const QString& pairUser, const QString& pairPassword,
                     QObject* parent = nullptr);
    ~MultiSeatBackend() override;

    QString type() const override { return QStringLiteral("multiseat"); }

    /// Independent seats, created on demand. No native co-op: co-op on
    /// MultiSeat is MoonlightWeb's own ShareManager, since seats are separate
    /// Windows sessions rather than one shared instance.
    BackendCapabilities capabilities() const override
    {
        return BackendCapabilities{/*multiUser*/ true, /*provisioning*/ true, /*lobbies*/ false};
    }

    /// No handshake: MultiSeat's API is key-authenticated. This just proves the
    /// key is accepted, which is the only thing that can be wrong at setup.
    void ensurePaired(BackendVoidCallback cb) override;

    void listSeats(BackendSeatListCallback cb) override;
    void allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb) override;
    void releaseSeat(const QString& seatId) override;

    void getAppList(const QString& seatId, BackendAppListCallback cb) override;

    void launch(const QString& seatId, const LaunchRequest& req, BackendMediaCallback cb) override;
    void resume(const QString& seatId, const LaunchRequest& req, BackendMediaCallback cb) override;
    void quit(const QString& seatId, const QString& clientUniqueId,
              BackendVoidCallback cb) override;

    void provisionSeat(const QJsonObject& params, BackendSeatCallback cb) override;
    void teardownSeat(const QString& seatId, BackendVoidCallback cb) override;
    void releaseSeatOwner(const QString& seatId, BackendVoidCallback cb) override;

private:
    /// Resolve a seat, then hand back a GameStreamBackend bound to its Apollo,
    /// pairing it first if we have never paired that seat. `cb` gets nullptr on
    /// failure, with the reason.
    using SeatBackendCallback =
        std::function<void(GameStreamBackend* backend, const BackendError& err)>;
    void withSeatBackend(const QString& seatId, SeatBackendCallback cb);

    /// The synthetic host for a seat: the machine's address with the seat's own
    /// GFE ports, plus whatever pairing we have recorded for it.
    NvComputer* seatHost(const MultiSeatSeat& seat, const QString& address);

    void pairSeat(const MultiSeatSeat& seat, const QString& address, BackendVoidCallback cb);

    QString seatPairingKey(const QString& seatId, const char* field) const;

    /// Seat ownership, persisted so it survives a restart: a user keeps their
    /// seat, and with it the Windows account holding their saves.
    QString ownershipKey(const QString& deviceSessionId) const;
    QString ownedSeat(const QString& deviceSessionId) const;
    void claimOwnership(const QString& deviceSessionId, const QString& seatId);
    void releaseOwnership(const QString& deviceSessionId);
    QSet<QString> ownedSeats() const;

    QString m_HostUuid;
    HostResolver m_ResolveHost;
    NvHTTP* m_Http = nullptr;

    // Owned through the QObject parent-child relation (parent is this).
    MultiSeatApiClient* m_Api = nullptr;

    QString m_PairUser;
    QString m_PairPassword;

    NvHTTP* m_SeatHttp = nullptr;
    QNetworkAccessManager* m_Nam = nullptr;

    // One synthetic host and one provider per seat, kept for the life of this
    // backend: their async callbacks reference them. Hosts are held by value —
    // Qt 6 backs QMap with std::map, so references to values stay valid across
    // inserts, which the resolver closures depend on. Backends are QObjects
    // parented to this, so Qt owns them.
    QMap<QString, NvComputer> m_SeatHosts;
    QMap<QString, GameStreamBackend*> m_SeatBackends;

    // Kept alive for the whole handshake, like WolfBackend does.
    std::unique_ptr<NvPairingManager> m_Pairing;
    SunshineRestClient* m_PinPusher = nullptr;


};
