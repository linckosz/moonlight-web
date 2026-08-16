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

#include <QObject>
#include <functional>

class NvComputer;
class NvHTTP;
class QNetworkAccessManager;

// The baseline provider: one GameStream host (Sunshine, Apollo, a punktfunk
// host in --gamestream mode, or a single MultiSeat seat) exposed as a backend
// with exactly one seat.
//
// It owns the NvHTTP orchestration only. Host bookkeeping that belongs to
// ComputerManager — flipping pairState on a 401, caching appList, kicking off
// box-art prefetch — stays there; this class reports NotPaired and lets the
// caller decide. That split is what keeps /api/hosts/* answering byte-identically.
//
// The host is resolved through a callback rather than held as a pointer: an
// NvComputer can be deleted mid-request (handleDeleteHost), and capturing it
// raw would be a use-after-free.
class GameStreamBackend : public QObject, public IStreamBackend
{
    Q_OBJECT

public:
    using HostResolver = std::function<NvComputer*()>;

    GameStreamBackend(QString hostUuid, HostResolver resolver, NvHTTP* http,
                      QNetworkAccessManager* nam, QObject* parent = nullptr);

    QString type() const override { return QStringLiteral("gamestream"); }

    // A plain GameStream host is single-user, cannot provision seats, and has
    // no native co-op — so the UI renders exactly what it renders today.
    BackendCapabilities capabilities() const override { return BackendCapabilities{}; }

    void ensurePaired(BackendVoidCallback cb) override;
    void listSeats(BackendSeatListCallback cb) override;
    void allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb) override;
    void releaseSeat(const QString& seatId) override;

    void getAppList(const QString& seatId, BackendAppListCallback cb) override;

    void launch(const QString& seatId, const LaunchRequest& req,
                BackendMediaCallback cb) override;
    void resume(const QString& seatId, const LaunchRequest& req,
                BackendMediaCallback cb) override;
    void quit(const QString& seatId, const QString& clientUniqueId,
              BackendVoidCallback cb) override;

    void provisionSeat(const QJsonObject& params, BackendSeatCallback cb) override;
    void teardownSeat(const QString& seatId, BackendVoidCallback cb) override;

    // The single seat's id — the host uuid.
    QString seatId() const { return m_HostUuid; }

private:
    // Shared prologue: resolve the host, check it's paired and reachable.
    // Returns nullptr and fills `err` when the caller should bail out.
    NvComputer* requireReadyHost(BackendError& err) const;

    // Turns a finished launch/resume reply into a MediaDescriptor.
    void finishLaunchReply(class QNetworkReply* reply, const LaunchRequest& req,
                           BackendMediaCallback cb);

    QString m_HostUuid;
    HostResolver m_ResolveHost;
    NvHTTP* m_Http = nullptr;
    QNetworkAccessManager* m_Nam = nullptr;
};
