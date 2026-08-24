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

#include "MediaDescriptor.h"
#include "../NvApp.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QString>
#include <QVector>
#include <functional>

// Everything a launch/resume needs, flattened so the interface owns no
// dependency on StreamConfig (which only carries static defaults — the real
// per-session dimensions live in StreamSession).
//
// clientUniqueId is the identity to present upstream. Empty means "the
// provider's default identity". Wolf needs a distinct identity per device
// session, because it keys a running session by client certificate and would
// otherwise hand player B the session of player A.
struct LaunchRequest
{
    int appId = 0;
    int width = 0;
    int height = 0;
    int fps = 0;
    int bitrateKbps = 0;
    bool hdrEnabled = false;
    bool muteHostAudio = true;
    QByteArray rikey;
    int rikeyid = 0;
    QString clientUniqueId;

    // Whose certificate to present. Empty means the default identity, which is
    // what every plain GameStream host uses and must keep using.
    //
    // It is separate from clientUniqueId because the two are not interchangeable:
    // Sunshine keys sessions by uniqueid, Wolf by client certificate. Only a
    // backend that keys on the certificate has any reason to set this.
    QString clientIdentitySeat;
};

// One streamable slot on a backend.
//
// For a plain Sunshine/Apollo host there is exactly one seat (the host itself).
// For MultiSeat a seat is a provisioned Windows account + its dedicated Apollo
// instance (address:PortBase). For Wolf a seat is a client identity, since Wolf
// keys a session by client certificate.
struct SeatRef
{
    QString id;             // stable, backend-scoped
    QString name;           // shown to the user
    QString address;
    quint16 httpPort = 0;
    quint16 httpsPort = 0;
    bool busy = false;      // already streaming
};

// What a backend can do beyond the baseline. The frontend keys UI off these:
// a Sunshine host must keep exactly today's experience, so all three are false
// and nothing extra is rendered.
struct BackendCapabilities
{
    bool multiUser = false;     // independent concurrent seats/sessions
    bool provisioning = false;  // seats can be created/destroyed on demand
    bool lobbies = false;       // native co-op (Wolf) — otherwise ShareManager
};

// Why a call failed. Callers map this to HTTP; keeping the kind distinct from
// the message is what lets the existing routes answer exactly as they do today
// (notably: an upstream 401 on a paired host must surface as "pair again", not
// as a generic 502).
struct BackendError
{
    enum Kind {
        None,
        NotFound,     // no such host/seat/app
        NotPaired,    // upstream rejected our client cert
        NoAddress,    // nothing to dial (host known, but no usable address)
        Unreachable,  // transport failure while dialing
        Timeout,
        Protocol,     // reached it, but the answer made no sense
        Unsupported,  // capability the backend doesn't have
    };

    Kind kind = None;
    int httpStatus = 0;  // upstream status when there was one, else 0
    QString message;     // user-facing

    static BackendError make(Kind k, const QString& msg, int status = 0)
    {
        return BackendError{k, status, msg};
    }
};

// Async results. Every call reports (ok, error) so a provider never throws
// across the interface.
using BackendVoidCallback = std::function<void(bool ok, const BackendError& error)>;
using BackendSeatListCallback =
    std::function<void(bool ok, const BackendError& error, const QVector<SeatRef>& seats)>;
using BackendSeatCallback =
    std::function<void(bool ok, const BackendError& error, const SeatRef& seat)>;
using BackendAppListCallback =
    std::function<void(bool ok, const BackendError& error, const QVector<NvApp>& apps)>;
using BackendMediaCallback =
    std::function<void(bool ok, const BackendError& error, const MediaDescriptor& media)>;
using BackendJsonCallback =
    std::function<void(bool ok, const BackendError& error, const QJsonArray& items)>;
// One backend-assigned identifier. `value` is empty when the backend answered
// but has nothing to name — an unambiguous "no", distinct from ok == false.
using BackendStringCallback =
    std::function<void(bool ok, const BackendError& error, const QString& value)>;

// A source of streamable seats and apps.
//
// Contract notes that matter:
//  - ensurePaired() is the ONLY step allowed to need a human. It runs once per
//    backend registration; every per-seat pairing underneath must be automated
//    (MultiSeat: push the PIN to each seat's Apollo web UI; Wolf: POST the PIN
//    to /api/v1/pair/client). Players never type a pairing PIN.
//  - getAppList() returns what the backend actually advertises. For Wolf that
//    is Wolf UI itself — do NOT substitute the per-profile app lists, or the
//    profile/PIN/catalogue/lobby experience users value is bypassed.
//  - launch() yields a MediaDescriptor; the caller must not assume GameStream.
class IStreamBackend
{
public:
    virtual ~IStreamBackend() = default;

    virtual QString type() const = 0;
    virtual BackendCapabilities capabilities() const = 0;

    // One-time, admin-initiated. Idempotent: a paired backend answers ok.
    virtual void ensurePaired(BackendVoidCallback cb) = 0;

    virtual void listSeats(BackendSeatListCallback cb) = 0;

    // Sticky assignment: the same deviceSessionId should get the same seat back
    // so the player returns to their own world (saves, settings).
    virtual void allocateSeat(const QString& deviceSessionId, BackendSeatCallback cb) = 0;
    virtual void releaseSeat(const QString& seatId) = 0;

    virtual void getAppList(const QString& seatId, BackendAppListCallback cb) = 0;

    virtual void launch(const QString& seatId, const LaunchRequest& req,
                        BackendMediaCallback cb) = 0;
    virtual void resume(const QString& seatId, const LaunchRequest& req,
                        BackendMediaCallback cb) = 0;
    // `clientUniqueId` scopes the quit to one client's session, exactly as it
    // scopes a launch. Empty means the provider's default identity.
    //
    // It is not optional in practice: a GameStream /cancel without it targets
    // whatever session the default identity owns, so an unscoped quit can tear
    // down a *different* player's stream. StreamSession has always passed it.
    virtual void quit(const QString& seatId, const QString& clientUniqueId,
                      BackendVoidCallback cb) = 0;

    // Only meaningful when capabilities().provisioning is true; others answer
    // ok == false with an explanatory error.
    virtual void provisionSeat(const QJsonObject& params, BackendSeatCallback cb) = 0;
    virtual void teardownSeat(const QString& seatId, BackendVoidCallback cb) = 0;

    // Break the durable association between a seat and whoever owns it, without
    // destroying the seat. Ownership deliberately survives a disconnect, which
    // means a device that never comes back — a reinstalled browser, cleared
    // storage — would otherwise hold its seat forever. On a backend with a
    // finite number of seats that is a slow leak with no way out, so an admin
    // needs this. Backends whose seats are not a finite resource say Unsupported.
    virtual void releaseSeatOwner(const QString& seatId, BackendVoidCallback cb)
    {
        Q_UNUSED(seatId);
        cb(false, BackendError::make(BackendError::Unsupported,
                                     QStringLiteral("This backend does not own seats")));
    }

    // Wolf's console concepts, passed through as the provider reports them.
    // Not pure: most backends have neither, and the default says so rather than
    // making every provider write the same refusal.
    //
    // These exist for the overlay to offer what the browser does better than a
    // streamed UI — switching profile, joining a lobby — never to replace the
    // console itself.
    virtual void listProfiles(BackendJsonCallback cb)
    {
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("This backend has no profiles")),
           QJsonArray());
    }

    virtual void listLobbies(BackendJsonCallback cb)
    {
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("This backend has no lobbies")),
           QJsonArray());
    }

    // --- Native co-op ------------------------------------------------------
    //
    // Only backends whose capabilities().lobbies is true implement these. They
    // exist because sharing a screen on such a backend is NOT the ShareManager
    // mechanism: a second GameStream launch there opens a second, private
    // desktop rather than a second view of the first one. What makes two
    // players see the same thing is joining them to one lobby, which owns the
    // compositor and runs the app exactly once.
    //
    // The identifier they trade in is the backend's own session id. We cannot
    // compute it — Wolf derives it from the client certificate with a hash whose
    // value is an implementation detail of its standard library — so it has to
    // be read back, which is what resolveCoopSessionId is for.

    /// The session id the backend gave the session THIS launch key created.
    /// `launchKey` is the per-session `rikey`; it is the only field that cannot
    /// collide between two of our own concurrent sessions (address, resolution
    /// and app all can). Empty `value` means no session matched.
    virtual void resolveCoopSessionId(const QByteArray& launchKey, BackendStringCallback cb)
    {
        Q_UNUSED(launchKey);
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("This backend has no co-op sessions")),
           QString());
    }

    /// Id of the lobby `peerSessionId` is currently on. Empty `value` with
    /// ok == true is the normal answer when the owner simply has not started
    /// co-op yet — the caller turns that into an instruction, not an error.
    virtual void findCoopLobby(const QString& peerSessionId, BackendStringCallback cb)
    {
        Q_UNUSED(peerSessionId);
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("This backend has no lobbies")),
           QString());
    }

    /// Put `sessionId` onto `lobbyId`'s shared compositor. Must not be called
    /// before that session is actually streaming — see WolfApiClient::joinLobby
    /// for why an early join is lost without any error.
    virtual void joinCoopLobby(const QString& lobbyId, const QString& sessionId,
                               BackendVoidCallback cb)
    {
        Q_UNUSED(lobbyId);
        Q_UNUSED(sessionId);
        cb(false, BackendError::make(BackendError::Unsupported,
                                     QStringLiteral("This backend has no lobbies")));
    }

    /// End one session host-side. Called by the supervisor when a stream ends,
    /// including when its worker died without a word — which is exactly when a
    /// client-side teardown cannot happen and sessions would otherwise leak.
    /// Leaves any lobby as a side effect.
    virtual void endCoopSession(const QString& sessionId, BackendVoidCallback cb)
    {
        Q_UNUSED(sessionId);
        cb(false,
           BackendError::make(BackendError::Unsupported,
                              QStringLiteral("This backend has no co-op sessions")));
    }
};
