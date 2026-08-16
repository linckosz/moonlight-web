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

#include "GameStreamBackend.h"

#include "../../common/Logger.h"
#include "../IdentityManager.h"
#include "../NvAddress.h"
#include "../NvComputer.h"
#include "../NvHTTP.h"

#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>

GameStreamBackend::GameStreamBackend(QString hostUuid, HostResolver resolver, NvHTTP* http,
                                     QNetworkAccessManager* nam, QObject* parent)
    : QObject(parent)
    , m_HostUuid(std::move(hostUuid))
    , m_ResolveHost(std::move(resolver))
    , m_Http(http)
    , m_Nam(nam)
{
}

NvComputer* GameStreamBackend::requireReadyHost(BackendError& err) const
{
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        err = BackendError::make(BackendError::NotFound, QStringLiteral("Host not found"));
        return nullptr;
    }

    if (host->pairState != NvComputer::PS_PAIRED || host->serverCertPem.isEmpty()) {
        err = BackendError::make(BackendError::NotPaired, QStringLiteral("Host not paired"));
        return nullptr;
    }

    if (host->uniqueAddresses().isEmpty()) {
        err = BackendError::make(BackendError::NoAddress,
                                 QStringLiteral("Host has no reachable address"));
        return nullptr;
    }

    return host;
}

void GameStreamBackend::ensurePaired(BackendVoidCallback cb)
{
    // A GameStream host is paired through ComputerManager's pairing chain, which
    // is driven by the user typing our PIN into Sunshine. Nothing to automate
    // here — just report the current state.
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        cb(false, BackendError::make(BackendError::NotFound, QStringLiteral("Host not found")));
        return;
    }

    if (host->pairState == NvComputer::PS_PAIRED && !host->serverCertPem.isEmpty()) {
        cb(true, BackendError{});
    } else {
        cb(false, BackendError::make(BackendError::NotPaired, QStringLiteral("Host not paired")));
    }
}

void GameStreamBackend::listSeats(BackendSeatListCallback cb)
{
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        cb(false, BackendError::make(BackendError::NotFound, QStringLiteral("Host not found")), {});
        return;
    }

    SeatRef seat;
    seat.id = m_HostUuid;
    seat.name = host->name;
    QVector<NvAddress> addrs = host->uniqueAddresses();
    if (!addrs.isEmpty()) seat.address = addrs.first().address();
    seat.httpsPort = host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT;
    seat.httpPort = MW_HTTP_PORT;
    seat.busy = host->currentGameId != 0;

    cb(true, BackendError{}, QVector<SeatRef>{seat});
}

void GameStreamBackend::allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb)
{
    // Single seat, shared by every device session: a plain GameStream host has
    // no notion of per-user isolation. Multi-seat backends override this.
    Q_UNUSED(deviceSessionId);

    listSeats([cb = std::move(cb)](bool ok, const BackendError& err, const QVector<SeatRef>& seats) {
        if (!ok || seats.isEmpty()) {
            cb(false,
               ok ? BackendError::make(BackendError::NotFound, QStringLiteral("No seat available"))
                  : err,
               SeatRef{});
            return;
        }
        cb(true, BackendError{}, seats.first());
    });
}

void GameStreamBackend::releaseSeat(const QString& seatId)
{
    // Nothing to release: the seat is the host itself.
    Q_UNUSED(seatId);
}

void GameStreamBackend::getAppList(const QString& seatId, BackendAppListCallback cb)
{
    Q_UNUSED(seatId);

    BackendError err;
    NvComputer* host = requireReadyHost(err);
    if (!host) {
        cb(false, err, {});
        return;
    }

    IdentityManager* im = IdentityManager::get();
    quint16 httpsPort = host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT;
    const NvAddress addr = host->uniqueAddresses().first();

    QNetworkReply* reply =
        m_Http->getAppListAsync(addr, httpsPort, im->getCertificate(), im->getPrivateKey());

    // One-shot guard: the timeout and finished() race each other.
    auto answered = std::make_shared<bool>(false);
    auto answer = [answered, cb](bool ok, const BackendError& e, const QVector<NvApp>& apps) {
        if (*answered) return;
        *answered = true;
        cb(ok, e, apps);
    };

    QTimer::singleShot(NvHTTP::REQUEST_TIMEOUT_MS + 2000, reply, [answer]() {
        answer(false,
               BackendError::make(BackendError::Timeout,
                                  QStringLiteral("App list request timed out")),
               {});
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, answer]() {
        // Force-evict the pooled TLS socket. Qt keeps a finished TLS socket alive
        // ~120s; that socket holds Sunshine's single-threaded HTTPS server, and
        // any other client polling the same host times out for the whole window.
        m_Nam->clearConnectionCache();

        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
            // 401 on a host we believed paired means the host dropped us. Report
            // it as NotPaired and pass the raw transport message through; the
            // caller owns both the host-state bookkeeping and the user-facing
            // wording.
            if (httpStatus == 401) {
                answer(false,
                       BackendError::make(BackendError::NotPaired, reply->errorString(),
                                          httpStatus),
                       {});
            } else {
                answer(false,
                       BackendError::make(BackendError::Unreachable, reply->errorString(),
                                          httpStatus),
                       {});
            }
            reply->deleteLater();
            return;
        }

        const QString xml = QString::fromUtf8(reply->readAll());

        try {
            NvHTTP::verifyResponseStatus(xml);
        } catch (const std::exception& e) {
            answer(false,
                   BackendError::make(BackendError::Protocol, QString::fromUtf8(e.what()),
                                      httpStatus),
                   {});
            reply->deleteLater();
            return;
        }

        answer(true, BackendError{}, NvHTTP::parseAppList(xml));
        reply->deleteLater();
    });
}

void GameStreamBackend::launch(const QString& seatId, const LaunchRequest& req,
                               BackendMediaCallback cb)
{
    Q_UNUSED(seatId);

    BackendError err;
    NvComputer* host = requireReadyHost(err);
    if (!host) {
        cb(false, err, MediaDescriptor{});
        return;
    }

    IdentityManager* im = IdentityManager::get();
    const QString uniqueId =
        req.clientUniqueId.isEmpty() ? im->getUniqueId() : req.clientUniqueId;

    QNetworkReply* reply = m_Http->launchAppAsync(
        host->activeAddress, host->activeHttpsPort, req.appId, uniqueId, req.rikey, req.rikeyid,
        req.width, req.height, req.fps, req.bitrateKbps, im->getCertificate(), im->getPrivateKey(),
        req.hdrEnabled ? 1 : 0, req.muteHostAudio ? 0 : 1);

    finishLaunchReply(reply, req, std::move(cb));
}

void GameStreamBackend::resume(const QString& seatId, const LaunchRequest& req,
                               BackendMediaCallback cb)
{
    Q_UNUSED(seatId);

    BackendError err;
    NvComputer* host = requireReadyHost(err);
    if (!host) {
        cb(false, err, MediaDescriptor{});
        return;
    }

    IdentityManager* im = IdentityManager::get();
    const QString uniqueId =
        req.clientUniqueId.isEmpty() ? im->getUniqueId() : req.clientUniqueId;

    QNetworkReply* reply = m_Http->resumeAppAsync(
        host->activeAddress, host->activeHttpsPort, uniqueId, req.rikey, req.rikeyid,
        im->getCertificate(), im->getPrivateKey(), req.muteHostAudio ? 0 : 1);

    finishLaunchReply(reply, req, std::move(cb));
}

void GameStreamBackend::finishLaunchReply(QNetworkReply* reply, const LaunchRequest& req,
                                          BackendMediaCallback cb)
{
    auto answered = std::make_shared<bool>(false);
    auto answer = [answered, cb](bool ok, const BackendError& e, const MediaDescriptor& media) {
        if (*answered) return;
        *answered = true;
        cb(ok, e, media);
    };

    QTimer::singleShot(NvHTTP::LAUNCH_TIMEOUT_MS + 2000, reply, [answer]() {
        answer(false,
               BackendError::make(BackendError::Timeout, QStringLiteral("Launch request timed out")),
               MediaDescriptor{});
    });

    connect(reply, &QNetworkReply::finished, this, [this, reply, req, answer]() {
        const int httpStatus = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

        if (reply->error() != QNetworkReply::NoError) {
            answer(false,
                   BackendError::make(BackendError::Unreachable, reply->errorString(), httpStatus),
                   MediaDescriptor{});
            reply->deleteLater();
            return;
        }

        const QString xml = QString::fromUtf8(reply->readAll());

        try {
            NvHTTP::verifyResponseStatus(xml);
        } catch (const std::exception& e) {
            answer(false,
                   BackendError::make(BackendError::Protocol, QString::fromUtf8(e.what()),
                                      httpStatus),
                   MediaDescriptor{});
            reply->deleteLater();
            return;
        }

        const QString sessionUrl = NvHTTP::parseSessionUrl(xml);
        if (sessionUrl.isEmpty()) {
            answer(false,
                   BackendError::make(BackendError::Protocol,
                                      QStringLiteral("No session URL in launch response")),
                   MediaDescriptor{});
            reply->deleteLater();
            return;
        }

        // The host may have gone away during the request.
        NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
        if (!host) {
            answer(false,
                   BackendError::make(BackendError::NotFound,
                                      QStringLiteral("Host removed while launching")),
                   MediaDescriptor{});
            reply->deleteLater();
            return;
        }

        MediaDescriptor media;
        media.type = MediaType::GameStreamRtsp;
        media.gameStream.rtspSessionUrl = sessionUrl;
        media.gameStream.hostAddress = host->activeAddress.address();
        media.gameStream.appVersion = host->appVersion;
        media.gameStream.gfeVersion = host->gfeVersion;
        media.gameStream.serverCodecModeSupport = host->serverCodecModeSupport;
        media.gameStream.aesKey = req.rikey;
        media.gameStream.rikeyid = req.rikeyid;

        answer(true, BackendError{}, media);
        reply->deleteLater();
    });
}

void GameStreamBackend::quit(const QString& seatId, const QString& clientUniqueId,
                             BackendVoidCallback cb)
{
    Q_UNUSED(seatId);

    BackendError err;
    NvComputer* host = requireReadyHost(err);
    if (!host) {
        cb(false, err);
        return;
    }

    IdentityManager* im = IdentityManager::get();
    quint16 httpsPort = host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT;

    // Scoped to the caller's identity: an unscoped /cancel targets whatever
    // session the default identity owns, which may belong to another player.
    const QString uniqueId = clientUniqueId.isEmpty() ? im->getUniqueId() : clientUniqueId;

    QNetworkReply* reply =
        m_Http->quitAppAsync(host->uniqueAddresses().first(), httpsPort, im->getCertificate(),
                             im->getPrivateKey(), uniqueId);

    auto answered = std::make_shared<bool>(false);
    auto answer = [answered, cb](bool ok, const BackendError& e) {
        if (*answered) return;
        *answered = true;
        cb(ok, e);
    };

    QTimer::singleShot(NvHTTP::REQUEST_TIMEOUT_MS + 2000, reply, [answer]() {
        answer(false,
               BackendError::make(BackendError::Timeout, QStringLiteral("Quit request timed out")));
    });

    connect(reply, &QNetworkReply::finished, this, [reply, answer]() {
        if (reply->error() != QNetworkReply::NoError) {
            answer(false,
                   BackendError::make(
                       BackendError::Unreachable, reply->errorString(),
                       reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt()));
        } else {
            answer(true, BackendError{});
        }
        reply->deleteLater();
    });
}

void GameStreamBackend::provisionSeat(const QJsonObject& params, BackendSeatCallback cb)
{
    Q_UNUSED(params);
    cb(false,
       BackendError::make(BackendError::Unsupported,
                          QStringLiteral("This backend cannot provision seats")),
       SeatRef{});
}

void GameStreamBackend::teardownSeat(const QString& seatId, BackendVoidCallback cb)
{
    Q_UNUSED(seatId);
    cb(false, BackendError::make(BackendError::Unsupported,
                                 QStringLiteral("This backend cannot tear down seats")));
}
