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
#include "../IdentityManager.h"
#include "../WolfApiClient.h"
#include "WolfCoop.h"

#include <QSettings>

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
        // The raw Qt string here is "Operation canceled" (we abort on our own
        // short deadline), which reads as a bug rather than what it is. Say plainly
        // that the control API never answered — the usual cause is a wrong or
        // unreachable URL.
        return BackendError::make(
            BackendError::Timeout,
            QStringLiteral("Control API did not respond — is the URL reachable?"),
            err.httpStatus);
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
                         QNetworkAccessManager* nam, const QString& apiUrl,
                         const QString& apiToken, PairingCommit commit, QObject* parent)
    : QObject(parent)
    , m_HostUuid(std::move(hostUuid))
    , m_ResolveHost(std::move(resolver))
    , m_Commit(std::move(commit))
    , m_Api(new WolfApiClient(apiUrl, apiToken, nam, this))
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

void WolfBackend::pairIdentity(const QString& seatId, BackendVoidCallback cb)
{
    NvComputer* host = m_ResolveHost ? m_ResolveHost() : nullptr;
    if (!host) {
        cb(false, BackendError::make(BackendError::NotFound, QStringLiteral("Host not found")));
        return;
    }

    // Idempotent. The default identity is judged by the host's own pair state;
    // a seat by our record of it, since Wolf gives us no way to ask "is this
    // certificate known?" without deriving its client id.
    if (seatId.isEmpty()) {
        if (host->pairState == NvComputer::PS_PAIRED && !host->serverCertPem.isEmpty()) {
            // Already paired for streaming — but this is also the admin gesture that
            // (re)points us at a control-API URL, and a prior pairing says nothing
            // about whether THAT URL answers. Probe the control API before reporting
            // success: a bad URL must fail here (dialog stays open, stored config
            // untouched) instead of being committed on the strength of an old
            // pairing. A Wolf backend whose control API is unreachable is
            // non-functional anyway — no seat list, no player auto-pair — even when
            // the stream certificate is still valid. This runs only on the
            // admin-initiated ensurePaired() path, never on the launch hot path.
            m_Api->pairedClients(
                [cb = std::move(cb)](bool ok, const WolfApiError& err,
                                     const QVector<WolfPairedClient>&) mutable {
                    if (ok)
                        cb(true, BackendError{});
                    else
                        cb(false, toBackendError(err));
                });
            return;
        }
    } else if (isSeatPaired(seatId)) {
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
    m_Api->pendingPairRequests([this, host, addr, seatId, cb = std::move(cb)](
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

        // The seat's own certificate, and its id as the uniqueid: Wolf keys its
        // pairing cache on uniqueid + "@" + client_ip, so seats pairing from one
        // machine must not collide there either.
        m_Pairing = std::make_unique<NvPairingManager>(
            host->appVersion, addr.address(), addr.port(),
            host->activeHttpsPort > 0 ? host->activeHttpsPort : MW_HTTPS_PORT,
            IdentityManager::get()->identityForSeat(seatId), seatId);
        m_PairingInFlight = true;

        const QString pin = PairingChain::generatePin();

        PairingChain::run(
            m_Pairing.get(), pin,
            [this, knownSecrets](const QString& announced) {
                announcePin(announced, knownSecrets, kPendingPollAttempts);
            },
            [this, seatId, cb = std::move(cb)](const PairingChain::Result& result) {
                m_PairingInFlight = false;

                switch (result.outcome) {
                case PairingChain::Outcome::Paired:
                    if (seatId.isEmpty()) {
                        // Only the default identity's pairing describes the host.
                        if (m_Commit) m_Commit(result.serverCertPem);
                    } else {
                        markSeatPaired(seatId);
                    }
                    Logger::info(
                        QStringLiteral("Wolf auto-pair: paired %1 with no user interaction")
                            .arg(seatId.isEmpty() ? QStringLiteral("the default identity")
                                                  : QStringLiteral("seat ") + seatId));
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

void WolfBackend::ensurePaired(BackendVoidCallback cb)
{
    pairIdentity(QString(), std::move(cb));
}

QString WolfBackend::seatPairingKey(const QString& seatId) const
{
    return QStringLiteral("wolfSeatPairings/%1/%2").arg(m_HostUuid, seatId);
}

bool WolfBackend::isSeatPaired(const QString& seatId) const
{
    return QSettings().value(seatPairingKey(seatId), false).toBool();
}

void WolfBackend::markSeatPaired(const QString& seatId)
{
    // Persisted per host: the same seat facing a different Wolf is a different
    // pairing, and replaying a handshake Wolf already accepted would leave a
    // duplicate client behind.
    QSettings().setValue(seatPairingKey(seatId), true);
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
    Q_UNUSED(seatId);
    withPairedSeat(req, [this, cb](bool ok, const BackendError& err, const LaunchRequest& scoped) {
        if (!ok) {
            cb(false, err, MediaDescriptor{});
            return;
        }
        m_GameStream->launch(m_GameStream->seatId(), scoped, cb);
    });
}

void WolfBackend::resume(const QString& seatId, const LaunchRequest& req, BackendMediaCallback cb)
{
    Q_UNUSED(seatId);
    withPairedSeat(req, [this, cb](bool ok, const BackendError& err, const LaunchRequest& scoped) {
        if (!ok) {
            cb(false, err, MediaDescriptor{});
            return;
        }
        m_GameStream->resume(m_GameStream->seatId(), scoped, cb);
    });
}

void WolfBackend::withPairedSeat(const LaunchRequest& req, PreparedLaunchCallback cb)
{
    // The seat is derived from the device's own id, which is what distinguishes
    // one player from another all the way from the browser.
    const QString seat = seatIdFor(req.clientUniqueId);
    if (seat.isEmpty()) {
        // No device id: nothing to isolate, so present the default identity and
        // behave exactly as before.
        cb(true, BackendError{}, req);
        return;
    }

    pairIdentity(seat, [this, req, seat, cb](bool ok, const BackendError& err) {
        if (!ok) {
            cb(false, err, req);
            return;
        }
        LaunchRequest scoped = req;
        scoped.clientIdentitySeat = seat;
        scoped.clientUniqueId = seat;
        cb(true, BackendError{}, scoped);
    });
}

void WolfBackend::quit(const QString& seatId, const QString& clientUniqueId,
                       BackendVoidCallback cb)
{
    Q_UNUSED(clientUniqueId);
    // The seat IS the identity here, so it is what scopes the quit.
    m_GameStream->quit(m_GameStream->seatId(), seatId, std::move(cb));
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

void WolfBackend::listProfiles(BackendJsonCallback cb)
{
    // Straight from Wolf. A profile is the user-facing world; which one a player
    // uses stays their choice inside Wolf UI, so this only ever informs the
    // overlay — it never picks one for them.
    m_Api->listProfiles([cb](bool ok, const WolfApiError& err, const QJsonArray& items) {
        cb(ok, ok ? BackendError{} : toBackendError(err), items);
    });
}

void WolfBackend::listLobbies(BackendJsonCallback cb)
{
    m_Api->listLobbies([cb](bool ok, const WolfApiError& err, const QJsonArray& items) {
        cb(ok, ok ? BackendError{} : toBackendError(err), items);
    });
}

// --- Native co-op ------------------------------------------------------------

void WolfBackend::resolveCoopSessionId(const QByteArray& launchKey, BackendStringCallback cb)
{
    if (launchKey.isEmpty()) {
        cb(false,
           BackendError::make(BackendError::Protocol,
                              QStringLiteral("No launch key to match a session against")),
           QString());
        return;
    }

    m_Api->listSessions([cb, launchKey](bool ok, const WolfApiError& err,
                                        const QVector<WolfStreamSession>& sessions) {
        if (!ok) {
            cb(false, toBackendError(err), QString());
            return;
        }
        // Answering with an empty id is not an error: a session that has already
        // ended legitimately has no row.
        cb(true, BackendError{}, WolfCoop::matchSessionByLaunchKey(sessions, launchKey));
    });
}

void WolfBackend::endCoopSession(const QString& sessionId, BackendVoidCallback cb)
{
    if (sessionId.isEmpty()) {
        cb(true, BackendError{});
        return;
    }
    m_Api->stopSession(sessionId, [cb](bool ok, const WolfApiError& err) {
        cb(ok, ok ? BackendError{} : toBackendError(err));
    });
}

void WolfBackend::teardownSeat(const QString& seatId, BackendVoidCallback cb)
{
    Q_UNUSED(seatId);
    cb(false, BackendError::make(BackendError::Unsupported,
                                 QStringLiteral("Wolf seats cannot be torn down remotely")));
}
