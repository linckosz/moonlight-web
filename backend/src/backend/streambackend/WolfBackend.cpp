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

#include "WolfBackend.h"

#include "../../common/Logger.h"
#include "../NvAddress.h"
#include "../NvComputer.h"
#include "../NvHTTP.h"
#include "../NvPairingManager.h"
#include "../PairingChain.h"
#include "../WolfApiClient.h"

#include <QCryptographicHash>
#include <QSet>
#include <QTimer>

namespace {

// Wolf registers the pairing request when it *receives* phase 1, so our first
// look at /pair/pending can legitimately arrive first. Poll briefly rather than
// give up on the race.
constexpr int kPendingPollIntervalMs = 250;
constexpr int kPendingPollAttempts = 40; // ~10s

BackendError toBackendError(const WolfApiError& err)
{
    switch (err.kind) {
    case WolfApiError::Unreachable:
        return BackendError::make(BackendError::Unreachable, err.message, err.httpStatus);
    case WolfApiError::Timeout:
        return BackendError::make(BackendError::Timeout, err.message, err.httpStatus);
    case WolfApiError::HttpError:
    case WolfApiError::Protocol:
        return BackendError::make(BackendError::Protocol, err.message, err.httpStatus);
    case WolfApiError::None:
    default:
        return BackendError{};
    }
}

} // namespace

WolfBackend::WolfBackend(QString hostUuid, HostResolver resolver, NvHTTP* http,
                         QNetworkAccessManager* nam, WolfApiClient* api, PairingCommit commit,
                         QObject* parent)
    : QObject(parent)
    , m_HostUuid(std::move(hostUuid))
    , m_ResolveHost(std::move(resolver))
    , m_Api(api)
    , m_Commit(std::move(commit))
    , m_GameStream(std::make_unique<GameStreamBackend>(m_HostUuid, m_ResolveHost, http, nam))
{
}

WolfBackend::~WolfBackend() = default;

QString WolfBackend::seatIdFor(const QString& deviceSessionId)
{
    // No device session means no per-player identity to pin: fall back to the
    // provider default rather than inventing a shared one, which is what an
    // empty clientUniqueId asks for.
    if (deviceSessionId.isEmpty()) return QString();

    // Wolf's cache key is uniqueid + "@" + client_ip and the uniqueid travels in
    // GameStream query strings, so keep it short and hex-only.
    const QByteArray digest =
        QCryptographicHash::hash(deviceSessionId.toUtf8(), QCryptographicHash::Sha256);
    return QString::fromLatin1(digest.toHex().left(16));
}

LaunchRequest WolfBackend::withSeatIdentity(const QString& seatId, const LaunchRequest& req) const
{
    LaunchRequest scoped = req;
    scoped.clientUniqueId = seatId;
    return scoped;
}

// --- Pairing -----------------------------------------------------------------

void WolfBackend::announcePin(const QString& pin, const QSet<QString>& knownSecrets,
                              int attemptsLeft)
{
    if (attemptsLeft <= 0) {
        Logger::warning(
            QStringLiteral("Wolf auto-pair: no pairing request appeared on the host; "
                           "is MoonlightWeb reaching the same Wolf it is pairing with?"));
        return;
    }

    m_Api->pendingPairRequests([this, pin, knownSecrets, attemptsLeft](
                                   bool ok, const WolfApiError& err,
                                   const QVector<WolfPendingPair>& pending) {
        if (!ok) {
            Logger::warning(
                QStringLiteral("Wolf auto-pair: /pair/pending failed (%1)").arg(err.message));
            return;
        }

        // Pick the request that was not already parked before we started: Wolf
        // reports only an IP per entry, and several MoonlightWeb devices behind
        // one NAT would otherwise be indistinguishable.
        for (const WolfPendingPair& entry : pending) {
            if (knownSecrets.contains(entry.pairSecret)) continue;

            Logger::info(QStringLiteral("Wolf auto-pair: answering pairing request from %1")
                             .arg(entry.clientIp));
            m_Api->submitPin(entry.pairSecret, pin, [](bool, const WolfApiError&) {});
            return;
        }

        QTimer::singleShot(kPendingPollIntervalMs, this,
                           [this, pin, knownSecrets, attemptsLeft]() {
                               announcePin(pin, knownSecrets, attemptsLeft - 1);
                           });
    });
}

void WolfBackend::ensurePaired(BackendVoidCallback cb)
{
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        cb(false, BackendError::make(BackendError::NotFound, QStringLiteral("Host not found")));
        return;
    }

    // Idempotent, like every other provider's ensurePaired().
    if (host->pairState == NvComputer::PS_PAIRED && !host->serverCertPem.isEmpty()) {
        cb(true, BackendError{});
        return;
    }

    if (m_PairingInFlight) {
        cb(false, BackendError::make(BackendError::Protocol,
                                     QStringLiteral("Pairing already in progress")));
        return;
    }

    const QVector<NvAddress> addrs = host->uniqueAddresses();
    if (addrs.isEmpty()) {
        cb(false, BackendError::make(BackendError::NoAddress,
                                     QStringLiteral("Host has no reachable address")));
        return;
    }
    const NvAddress& addr = addrs.first();

    // Snapshot what is already parked on the host. Anything appearing after
    // this that we did not know about is ours — see announcePin().
    m_Api->pendingPairRequests([this, host, addr, cb = std::move(cb)](
                                   bool ok, const WolfApiError& err,
                                   const QVector<WolfPendingPair>& pending) mutable {
        if (!ok) {
            // Failing here means the control API is unusable, and without it the
            // handshake could never be completed — better to say so now than to
            // park a request on the host that nobody will ever answer.
            cb(false, toBackendError(err));
            return;
        }

        QSet<QString> knownSecrets;
        for (const WolfPendingPair& entry : pending) {
            knownSecrets.insert(entry.pairSecret);
        }

        m_Pairing = std::make_unique<NvPairingManager>(
            host->appVersion, addr.address(), addr.port(),
            host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT);
        m_PairingInFlight = true;

        const QString pin = PairingChain::generatePin();

        PairingChain::run(
            m_Pairing.get(), pin,
            [this, knownSecrets](const QString& announced) {
                announcePin(announced, knownSecrets, kPendingPollAttempts);
            },
            [this, cb = std::move(cb)](const PairingChain::Result& result) {
                m_PairingInFlight = false;

                switch (result.outcome) {
                case PairingChain::Outcome::Paired:
                    if (m_Commit) m_Commit(result.serverCertPem);
                    Logger::info(QStringLiteral("Wolf auto-pair: paired with no user interaction"));
                    cb(true, BackendError{});
                    break;

                case PairingChain::Outcome::HostBusy:
                    cb(false, BackendError::make(
                                  BackendError::Protocol,
                                  QStringLiteral("Wolf is already pairing with another client")));
                    break;

                case PairingChain::Outcome::Retry:
                    // On Sunshine this means "the human has not typed it yet".
                    // Here nobody is typing, so it is a real failure: either the
                    // handshake never reached Wolf, or the PIN we posted was not
                    // the one it was waiting for.
                case PairingChain::Outcome::Failed:
                default:
                    cb(false, BackendError::make(BackendError::Protocol,
                                                 QStringLiteral("Wolf pairing failed")));
                    break;
                }
            });
    });
}

// --- Seats -------------------------------------------------------------------

void WolfBackend::listSeats(BackendSeatListCallback cb)
{
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        cb(false, BackendError::make(BackendError::NotFound, QStringLiteral("Host not found")), {});
        return;
    }

    const QVector<NvAddress> addrs = host->uniqueAddresses();
    const QString address = addrs.isEmpty() ? QString() : addrs.first().address();
    const quint16 httpsPort = host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT;

    // The identities Wolf already knows. These are what exists server-side; a
    // seat for a brand-new device session is minted by allocateSeat() and only
    // shows up here once it has paired.
    m_Api->pairedClients([cb = std::move(cb), address, httpsPort](
                             bool ok, const WolfApiError& err,
                             const QVector<WolfPairedClient>& clients) {
        if (!ok) {
            cb(false, toBackendError(err), {});
            return;
        }

        QVector<SeatRef> seats;
        seats.reserve(clients.size());
        for (const WolfPairedClient& client : clients) {
            SeatRef seat;
            seat.id = client.clientId;
            seat.name = client.clientId;
            seat.address = address;
            seat.httpPort = MW_HTTP_PORT;
            seat.httpsPort = httpsPort;
            seats.append(seat);
        }

        cb(true, BackendError{}, seats);
    });
}

void WolfBackend::allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb)
{
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        cb(false, BackendError::make(BackendError::NotFound, QStringLiteral("Host not found")),
           SeatRef{});
        return;
    }

    const QVector<NvAddress> addrs = host->uniqueAddresses();
    if (addrs.isEmpty()) {
        cb(false,
           BackendError::make(BackendError::NoAddress,
                              QStringLiteral("Host has no reachable address")),
           SeatRef{});
        return;
    }

    // Nothing to reserve: the seat *is* the identity, derived deterministically
    // so the same device session always returns to its own Wolf session.
    SeatRef seat;
    seat.id = seatIdFor(deviceSessionId);
    seat.name = host->name;
    seat.address = addrs.first().address();
    seat.httpPort = MW_HTTP_PORT;
    seat.httpsPort = host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT;

    cb(true, BackendError{}, seat);
}

void WolfBackend::releaseSeat(const QString& seatId)
{
    // A Wolf identity is not a finite resource — nothing is held, so nothing is
    // returned. Unpairing on disconnect would throw away the player's world.
    Q_UNUSED(seatId);
}

// --- GameStream half ---------------------------------------------------------

void WolfBackend::getAppList(const QString& seatId, BackendAppListCallback cb)
{
    // Intentionally the GameStream list, not /api/v1/apps: what Wolf advertises
    // here is Wolf UI, which carries profiles, PINs, the catalogue and lobbies.
    Q_UNUSED(seatId);
    m_GameStream->getAppList(m_GameStream->seatId(), std::move(cb));
}

void WolfBackend::launch(const QString& seatId, const LaunchRequest& req, BackendMediaCallback cb)
{
    m_GameStream->launch(m_GameStream->seatId(), withSeatIdentity(seatId, req), std::move(cb));
}

void WolfBackend::resume(const QString& seatId, const LaunchRequest& req, BackendMediaCallback cb)
{
    m_GameStream->resume(m_GameStream->seatId(), withSeatIdentity(seatId, req), std::move(cb));
}

void WolfBackend::quit(const QString& seatId, BackendVoidCallback cb)
{
    Q_UNUSED(seatId);
    m_GameStream->quit(m_GameStream->seatId(), std::move(cb));
}

// --- Provisioning ------------------------------------------------------------

void WolfBackend::provisionSeat(const QJsonObject& params, BackendSeatCallback cb)
{
    Q_UNUSED(params);
    cb(false,
       BackendError::make(BackendError::Unsupported,
                          QStringLiteral("Wolf profiles are created from Wolf UI, not provisioned")),
       SeatRef{});
}

void WolfBackend::teardownSeat(const QString& seatId, BackendVoidCallback cb)
{
    Q_UNUSED(seatId);
    cb(false, BackendError::make(BackendError::Unsupported,
                                 QStringLiteral("Wolf seats cannot be torn down remotely")));
}
