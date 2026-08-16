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

#include "MultiSeatBackend.h"

#include "../../common/Logger.h"
#include "../MultiSeatApiClient.h"
#include "../NvAddress.h"
#include "../NvComputer.h"

namespace {

BackendError toBackendError(const MultiSeatApiError& err)
{
    switch (err.kind) {
    case MultiSeatApiError::Unreachable:
        return BackendError::make(BackendError::Unreachable, err.message, err.httpStatus);
    case MultiSeatApiError::Timeout:
        return BackendError::make(BackendError::Timeout, err.message, err.httpStatus);
    case MultiSeatApiError::Unauthorized:
        // NotPaired is how the interface says "your credentials were refused";
        // for MultiSeat that is a wrong API key, not a lost GameStream pairing,
        // and err.message says so.
        return BackendError::make(BackendError::NotPaired, err.message, err.httpStatus);
    case MultiSeatApiError::HttpError:
    case MultiSeatApiError::Protocol:
        return BackendError::make(BackendError::Protocol, err.message, err.httpStatus);
    case MultiSeatApiError::None:
    default:
        return BackendError{};
    }
}

/// Turn a seat into the interface's view of it. `address` is the MultiSeat
/// machine: every seat lives on it, only the ports differ.
SeatRef toSeatRef(const MultiSeatSeat& seat, const QString& address)
{
    SeatRef ref;
    ref.id = seat.id;
    // The Windows account is what the admin recognises; the guid means nothing
    // to them.
    ref.name = seat.accountName;
    ref.address = address;
    ref.httpPort = seat.gfeHttpPort();
    ref.httpsPort = seat.gfeHttpsPort();
    ref.busy = seat.status == QStringLiteral("Streaming");
    return ref;
}

} // namespace

MultiSeatBackend::MultiSeatBackend(QString hostUuid, HostResolver resolver, NvHTTP* http,
                                   QNetworkAccessManager* nam, const QString& apiUrl,
                                   const QString& apiKey, QObject* parent)
    : QObject(parent)
    , m_HostUuid(std::move(hostUuid))
    , m_ResolveHost(std::move(resolver))
    , m_Http(http)
    , m_Api(new MultiSeatApiClient(apiUrl, apiKey, nam, this))
{
}

MultiSeatBackend::~MultiSeatBackend() = default;

BackendError MultiSeatBackend::perSeatGameStreamUnsupported()
{
    return BackendError::make(
        BackendError::Unsupported,
        QStringLiteral("MultiSeat seats stream through their own Apollo instance, which needs a "
                       "per-seat host record (ports, certificate, pair state). That is "
                       "SeatManager's job and is not wired yet."));
}

void MultiSeatBackend::ensurePaired(BackendVoidCallback cb)
{
    // Nothing to pair: the control API authenticates with a key. Prove the key
    // is accepted, which is the only thing an admin can get wrong at setup —
    // and the one failure worth reporting differently from "host is down".
    m_Api->listSeats([cb = std::move(cb)](bool ok, const MultiSeatApiError& err,
                                          const QVector<MultiSeatSeat>&) {
        if (!ok) {
            cb(false, toBackendError(err));
            return;
        }
        cb(true, BackendError{});
    });
}

void MultiSeatBackend::listSeats(BackendSeatListCallback cb)
{
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        cb(false, BackendError::make(BackendError::NotFound, QStringLiteral("Host not found")), {});
        return;
    }

    const QVector<NvAddress> addrs = host->uniqueAddresses();
    if (addrs.isEmpty()) {
        cb(false,
           BackendError::make(BackendError::NoAddress,
                              QStringLiteral("Host has no reachable address")),
           {});
        return;
    }
    const QString address = addrs.first().address();

    m_Api->listSeats([cb = std::move(cb), address](bool ok, const MultiSeatApiError& err,
                                                   const QVector<MultiSeatSeat>& seats) {
        if (!ok) {
            cb(false, toBackendError(err), {});
            return;
        }

        QVector<SeatRef> refs;
        refs.reserve(seats.size());
        for (const MultiSeatSeat& seat : seats) {
            // Skip what cannot be streamed. A seat that failed to provision
            // still carries a portBase, so filtering on the port alone would
            // offer a dead Apollo as if it were ready.
            if (!seat.isUsable()) {
                if (!seat.errorMessage.isEmpty()) {
                    Logger::info(QStringLiteral("MultiSeat: seat %1 unusable at step %2 — %3")
                                     .arg(seat.accountName, seat.provisioningStep,
                                          seat.errorMessage));
                }
                continue;
            }
            refs.append(toSeatRef(seat, address));
        }

        cb(true, BackendError{}, refs);
    });
}

void MultiSeatBackend::allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb)
{
    listSeats([this, deviceSessionId, cb = std::move(cb)](bool ok, const BackendError& err,
                                                          const QVector<SeatRef>& seats) {
        if (!ok) {
            cb(false, err, SeatRef{});
            return;
        }

        // Sticky first: a returning device must land back on its own Windows
        // account, or the player loses their saves and settings.
        const QString remembered = m_Assignments.value(deviceSessionId);
        if (!remembered.isEmpty()) {
            for (const SeatRef& seat : seats) {
                if (seat.id == remembered) {
                    cb(true, BackendError{}, seat);
                    return;
                }
            }
            // The seat was torn down while they were away; fall through and
            // give them a fresh one rather than failing.
            m_Assignments.remove(deviceSessionId);
        }

        const QSet<QString> taken(m_Assignments.cbegin(), m_Assignments.cend());
        for (const SeatRef& seat : seats) {
            if (seat.busy || taken.contains(seat.id)) continue;
            m_Assignments.insert(deviceSessionId, seat.id);
            cb(true, BackendError{}, seat);
            return;
        }

        cb(false,
           BackendError::make(BackendError::NotFound,
                              QStringLiteral("No free seat. Provision one, or wait for a player "
                                             "to disconnect.")),
           SeatRef{});
    });
}

void MultiSeatBackend::releaseSeat(const QString& seatId)
{
    // Drop the assignment but leave the seat provisioned: tearing it down would
    // destroy the player's Windows session, and they are likely to come back.
    // Reclaiming idle seats is a policy decision for SeatManager, not a
    // side effect of one player closing a tab.
    const QList<QString> owners = m_Assignments.keys(seatId);
    for (const QString& owner : owners) {
        m_Assignments.remove(owner);
    }
}

void MultiSeatBackend::getAppList(const QString& seatId, BackendAppListCallback cb)
{
    Q_UNUSED(seatId);
    cb(false, perSeatGameStreamUnsupported(), {});
}

void MultiSeatBackend::launch(const QString& seatId, const LaunchRequest& req,
                              BackendMediaCallback cb)
{
    Q_UNUSED(seatId);
    Q_UNUSED(req);
    cb(false, perSeatGameStreamUnsupported(), MediaDescriptor{});
}

void MultiSeatBackend::resume(const QString& seatId, const LaunchRequest& req,
                              BackendMediaCallback cb)
{
    Q_UNUSED(seatId);
    Q_UNUSED(req);
    cb(false, perSeatGameStreamUnsupported(), MediaDescriptor{});
}

void MultiSeatBackend::quit(const QString& seatId, BackendVoidCallback cb)
{
    Q_UNUSED(seatId);
    cb(false, perSeatGameStreamUnsupported());
}

void MultiSeatBackend::provisionSeat(const QJsonObject& params, BackendSeatCallback cb)
{
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        cb(false, BackendError::make(BackendError::NotFound, QStringLiteral("Host not found")),
           SeatRef{});
        return;
    }

    const QVector<NvAddress> addrs = host->uniqueAddresses();
    const QString address = addrs.isEmpty() ? QString() : addrs.first().address();

    m_Api->provisionSeat(params, [cb = std::move(cb), address](bool ok,
                                                               const MultiSeatApiError& err,
                                                               const MultiSeatSeat& seat) {
        if (!ok) {
            cb(false, toBackendError(err), SeatRef{});
            return;
        }

        // Provisioning can answer 201 with a seat that ended in Error — it
        // creates a Windows session, a virtual display, audio routing and an
        // Apollo process, and any of those can fail. Report the reason instead
        // of handing back a seat nobody can stream.
        if (!seat.isUsable()) {
            QString why = seat.errorMessage;
            if (why.isEmpty()) {
                why = QStringLiteral("Seat ended in state '%1' at step '%2'")
                          .arg(seat.status, seat.provisioningStep);
            }
            cb(false, BackendError::make(BackendError::Protocol, why), SeatRef{});
            return;
        }

        cb(true, BackendError{}, toSeatRef(seat, address));
    });
}

void MultiSeatBackend::teardownSeat(const QString& seatId, BackendVoidCallback cb)
{
    m_Api->teardownSeat(seatId, [this, seatId, cb = std::move(cb)](bool ok,
                                                                  const MultiSeatApiError& err) {
        if (ok) releaseSeat(seatId);
        cb(ok, toBackendError(err));
    });
}
