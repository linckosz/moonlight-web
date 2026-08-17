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
#include "../IdentityManager.h"
#include "../MultiSeatApiClient.h"
#include "../NvAddress.h"
#include "../NvComputer.h"
#include "../NvHTTP.h"
#include "../NvPairingManager.h"
#include "../PairingChain.h"
#include "../SunshineRestClient.h"
#include "GameStreamBackend.h"

#include <QSettings>

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
                                   const QString& apiKey, const QString& pairUser,
                                   const QString& pairPassword, QObject* parent)
    : QObject(parent)
    , m_HostUuid(std::move(hostUuid))
    , m_ResolveHost(std::move(resolver))
    , m_Http(http)
    , m_Api(new MultiSeatApiClient(apiUrl, apiKey, nam, this))
    , m_PairUser(pairUser)
    , m_PairPassword(pairPassword)
    , m_SeatHttp(http)
    , m_Nam(nam)
    , m_PinPusher(new SunshineRestClient(this))
{
}

MultiSeatBackend::~MultiSeatBackend() = default;

QString MultiSeatBackend::seatPairingKey(const QString& seatId, const char* field) const
{
    return QStringLiteral("multiSeatPairings/%1/%2/%3")
        .arg(m_HostUuid, seatId, QLatin1String(field));
}

NvComputer* MultiSeatBackend::seatHost(const MultiSeatSeat& seat, const QString& address)
{
    NvComputer* host = &m_SeatHosts[seat.id];

    // A seat is an ordinary GameStream host that happens to share a machine
    // with its siblings — only the ports differ.
    host->uuid = seat.id;
    host->name = seat.accountName;
    host->activeAddress = NvAddress(address, seat.gfeHttpPort());
    host->activeHttpsPort = seat.gfeHttpsPort();
    host->state = NvComputer::CS_ONLINE;

    QSettings settings;
    host->serverCertPem = settings.value(seatPairingKey(seat.id, "serverCert")).toByteArray();
    host->pairState =
        host->serverCertPem.isEmpty() ? NvComputer::PS_NOT_PAIRED : NvComputer::PS_PAIRED;

    return host;
}

void MultiSeatBackend::pairSeat(const MultiSeatSeat& seat, const QString& address,
                                BackendVoidCallback cb)
{
    if (m_PairUser.isEmpty() || m_PairPassword.isEmpty()) {
        cb(false,
           BackendError::make(
               BackendError::NotPaired,
               QStringLiteral("Seat %1 is not paired and no seat admin credentials are "
                              "configured. MultiSeat cannot pair a seat through its own API, "
                              "so the PIN has to reach that seat via its Apollo web UI.")
                   .arg(seat.accountName)));
        return;
    }

    // Its own certificate, so seats stay distinct clients to their Apollo.
    m_Pairing = std::make_unique<NvPairingManager>(
        QString(), address, seat.gfeHttpPort(), seat.gfeHttpsPort(),
        IdentityManager::get()->identityForSeat(seat.id), seat.id);

    const QString pin = PairingChain::generatePin();
    const QString seatId = seat.id;
    const QString accountName = seat.accountName;
    const quint16 webUiPort = static_cast<quint16>(seat.portBase + 1);

    PairingChain::run(
        m_Pairing.get(), pin,
        [this, address, webUiPort, accountName](const QString& announced) {
            // Stage 1 is parked on the host waiting for a PIN, and the seat
            // Apollo web UI is the only way to deliver it.
            Logger::info(QStringLiteral("MultiSeat: pushing PIN to the Apollo web UI of seat %1")
                             .arg(accountName));
            m_PinPusher->sendPin(announced, m_PairUser, m_PairPassword,
                                 QStringLiteral("moonlightweb"), webUiPort, address);
        },
        [this, seatId, accountName, cb](const PairingChain::Result& result) {
            if (result.outcome == PairingChain::Outcome::Paired) {
                QSettings().setValue(seatPairingKey(seatId, "serverCert"), result.serverCertPem);
                Logger::info(QStringLiteral("MultiSeat: paired seat %1 with no user interaction")
                                 .arg(accountName));
                cb(true, BackendError{});
                return;
            }

            // Unlike Sunshine, nobody is typing here: a refused PIN means the
            // credentials are wrong, not that a human is slow.
            cb(false, BackendError::make(
                          BackendError::NotPaired,
                          QStringLiteral("Could not pair seat %1. Check the seat admin "
                                         "credentials, which are what authorise the PIN.")
                              .arg(accountName)));
        });
}

void MultiSeatBackend::withSeatBackend(const QString& seatId, SeatBackendCallback cb)
{
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        cb(nullptr, BackendError::make(BackendError::NotFound, QStringLiteral("Host not found")));
        return;
    }
    const QVector<NvAddress> addrs = host->uniqueAddresses();
    if (addrs.isEmpty()) {
        cb(nullptr, BackendError::make(BackendError::NoAddress,
                                       QStringLiteral("Host has no reachable address")));
        return;
    }
    const QString address = addrs.first().address();

    // Always re-read the seat: its portBase and status live in the service, and
    // a seat can be torn down between two launches.
    m_Api->getSeat(seatId, [this, address, cb](bool ok, const MultiSeatApiError& err,
                                               const MultiSeatSeat& seat) {
        if (!ok) {
            cb(nullptr, toBackendError(err));
            return;
        }
        if (!seat.isUsable()) {
            QString why = seat.errorMessage;
            if (why.isEmpty()) why = QStringLiteral("Seat is in state %1").arg(seat.status);
            cb(nullptr, BackendError::make(BackendError::NotFound, why));
            return;
        }

        NvComputer* synthetic = seatHost(seat, address);

        auto ready = [this, id = seat.id, cb]() {
            GameStreamBackend*& slot = m_SeatBackends[id];
            if (!slot) {
                slot = new GameStreamBackend(
                    id,
                    [this, id]() -> NvComputer* {
                        auto it = m_SeatHosts.find(id);
                        return it == m_SeatHosts.end() ? nullptr : &it.value();
                    },
                    m_SeatHttp, m_Nam, this);
            }
            cb(slot, BackendError{});
        };

        if (synthetic->pairState == NvComputer::PS_PAIRED) {
            ready();
            return;
        }

        pairSeat(seat, address,
                 [this, seat, address, ready, cb](bool paired, const BackendError& perr) {
                     if (!paired) {
                         cb(nullptr, perr);
                         return;
                     }
                     seatHost(seat, address); // pick up the certificate just stored
                     ready();
                 });
    });
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

        // Ownership is durable: a user keeps their seat across restarts, so the
        // assignment is read from settings rather than from memory alone.
        const QString remembered = ownedSeat(deviceSessionId);
        if (!remembered.isEmpty()) {
            for (const SeatRef& seat : seats) {
                if (seat.id == remembered) {
                    cb(true, BackendError{}, seat);
                    return;
                }
            }
            // The seat was torn down while they were away; fall through and
            // give them a fresh one rather than failing.
            releaseOwnership(deviceSessionId);
        }

        const QSet<QString> taken = ownedSeats();
        for (const SeatRef& seat : seats) {
            if (seat.busy || taken.contains(seat.id)) continue;
            claimOwnership(deviceSessionId, seat.id);
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
    // Deliberately does nothing. Ownership is durable: a user keeps their seat,
    // and its Windows account keeps their saves and settings. Closing a tab is
    // not a reason to hand that account to somebody else — reassigning is an
    // admin decision, made by tearing the seat down.
    Q_UNUSED(seatId);
}

QString MultiSeatBackend::ownershipKey(const QString& deviceSessionId) const
{
    return QStringLiteral("multiSeatOwners/%1/%2").arg(m_HostUuid, deviceSessionId);
}

QString MultiSeatBackend::ownedSeat(const QString& deviceSessionId) const
{
    if (deviceSessionId.isEmpty()) return QString();
    return QSettings().value(ownershipKey(deviceSessionId)).toString();
}

void MultiSeatBackend::claimOwnership(const QString& deviceSessionId, const QString& seatId)
{
    if (deviceSessionId.isEmpty()) return;
    QSettings().setValue(ownershipKey(deviceSessionId), seatId);
    Logger::info(QStringLiteral("MultiSeat: seat %1 now belongs to %2").arg(seatId, deviceSessionId));
}

void MultiSeatBackend::releaseOwnership(const QString& deviceSessionId)
{
    if (deviceSessionId.isEmpty()) return;
    QSettings().remove(ownershipKey(deviceSessionId));
}

QSet<QString> MultiSeatBackend::ownedSeats() const
{
    // Every seat already spoken for, so a newcomer never takes one that has an
    // owner — even an owner who is currently away.
    QSettings settings;
    settings.beginGroup(QStringLiteral("multiSeatOwners/%1").arg(m_HostUuid));
    QSet<QString> owned;
    for (const QString& key : settings.childKeys()) {
        const QString seatId = settings.value(key).toString();
        if (!seatId.isEmpty()) owned.insert(seatId);
    }
    settings.endGroup();
    return owned;
}

void MultiSeatBackend::getAppList(const QString& seatId, BackendAppListCallback cb)
{
    withSeatBackend(seatId, [seatId, cb](GameStreamBackend* backend, const BackendError& err) {
        if (!backend) {
            cb(false, err, {});
            return;
        }
        backend->getAppList(seatId, cb);
    });
}

void MultiSeatBackend::launch(const QString& seatId, const LaunchRequest& req,
                              BackendMediaCallback cb)
{
    withSeatBackend(seatId, [seatId, req, cb](GameStreamBackend* backend, const BackendError& err) {
        if (!backend) {
            cb(false, err, MediaDescriptor{});
            return;
        }
        // Present the seat certificate: each Apollo keeps its own client list,
        // so seats must not look like one another.
        LaunchRequest scoped = req;
        scoped.clientIdentitySeat = seatId;
        backend->launch(seatId, scoped, cb);
    });
}

void MultiSeatBackend::resume(const QString& seatId, const LaunchRequest& req,
                              BackendMediaCallback cb)
{
    withSeatBackend(seatId, [seatId, req, cb](GameStreamBackend* backend, const BackendError& err) {
        if (!backend) {
            cb(false, err, MediaDescriptor{});
            return;
        }
        LaunchRequest scoped = req;
        scoped.clientIdentitySeat = seatId;
        backend->resume(seatId, scoped, cb);
    });
}

void MultiSeatBackend::quit(const QString& seatId, const QString& clientUniqueId,
                            BackendVoidCallback cb)
{
    withSeatBackend(seatId, [seatId, clientUniqueId, cb](GameStreamBackend* backend,
                                                         const BackendError& err) {
        if (!backend) {
            cb(false, err);
            return;
        }
        backend->quit(seatId, clientUniqueId, cb);
    });
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

void MultiSeatBackend::releaseSeatOwner(const QString& seatId, BackendVoidCallback cb)
{
    // Find whoever holds this seat and forget them. The seat itself is left
    // provisioned: the point is to hand a working seat to somebody else, not to
    // destroy the Windows account and the saves inside it.
    QSettings settings;
    const QString group = QStringLiteral("multiSeatOwners/%1").arg(m_HostUuid);
    settings.beginGroup(group);
    QStringList freed;
    for (const QString& key : settings.childKeys()) {
        if (settings.value(key).toString() == seatId) freed.append(key);
    }
    for (const QString& key : freed) {
        settings.remove(key);
    }
    settings.endGroup();

    if (freed.isEmpty()) {
        cb(false, BackendError::make(BackendError::NotFound,
                                     QStringLiteral("That seat has no owner to release")));
        return;
    }

    Logger::info(QStringLiteral("MultiSeat: seat %1 released, it can be claimed again").arg(seatId));
    cb(true, BackendError{});
}

void MultiSeatBackend::teardownSeat(const QString& seatId, BackendVoidCallback cb)
{
    m_Api->teardownSeat(seatId, [this, seatId, cb = std::move(cb)](bool ok,
                                                                  const MultiSeatApiError& err) {
        if (ok) releaseSeat(seatId);
        cb(ok, toBackendError(err));
    });
}
