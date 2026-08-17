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

#include "IStreamBackend.h"

#include <QMap>
#include <QObject>
#include <QString>

class MultiSeatApiClient;
class NvComputer;
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
 * **What is not implemented yet, and why.** getAppList/launch/resume/quit are
 * GameStream calls that must go to a *seat's* Apollo, not to the machine's own
 * address — a different port per seat, and a pairing per seat, since each
 * Apollo instance holds its own client list. That needs a host record per seat
 * (address + ports + certificate + pair state), which is exactly what
 * SeatManager is meant to own. Rather than fake it here, those calls report
 * Unsupported with that explanation. listSeats/allocateSeat/provisionSeat/
 * teardownSeat are complete and usable today.
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

private:
    /// The one place that says why the per-seat GameStream half is missing, so
    /// the four calls that share the limitation cannot drift apart.
    static BackendError perSeatGameStreamUnsupported();

    QString m_HostUuid;
    HostResolver m_ResolveHost;
    NvHTTP* m_Http = nullptr;

    // Owned through the QObject parent-child relation (parent is this).
    MultiSeatApiClient* m_Api = nullptr;

    QString m_PairUser;
    QString m_PairPassword;

    /// deviceSessionId → seat id. Sticky so a returning player lands back on
    /// their own Windows account, with their saves and settings.
    QMap<QString, QString> m_Assignments;
};
