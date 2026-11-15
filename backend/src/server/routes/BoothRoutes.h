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

#include "common/Types.h"
#include "server/ShareManager.h"
#include "server/StreamOwner.h"

#include <QByteArray>
#include <QSet>
#include <QString>

#include <functional>

class BoothPin;
class OwnerProof;
class RestRouter;

/// What a booth's stream PIN routes take from the server around them, as
/// functions: the tests stand in for the server.
struct BoothRoutesDeps
{
    /// The raw session cookie a request carries (HttpServer::sessionTokenFromRequest).
    std::function<QString(const HttpRequest&)> sessionToken;
    /// The pairing key bound to a session, empty when it has none. Read for
    /// every request, through the tunnel too: a booth proof is always asked.
    std::function<QByteArray(const QString& token)> pairingKey;
    /// The name a session's machine goes by (AuthManager::sessionMachineName).
    std::function<QString(const QString& token)> machineName;
    /// This host's MW-BIND-v1 identity, part of what a proof signs.
    std::function<QString()> hostKeyId;
    /// The address bucket a request counts in: AuthManager::rateLimitKey on
    /// the address the server trusts for it (AuthManager::attemptAddress), so
    /// a request carried in by a local proxy or the tunnel counts as its real
    /// caller, never as 127.0.0.1 for everybody.
    std::function<QString(const HttpRequest&)> addressKey;
    /// A failed proof, for ConnectionGuard. A wrong PIN is never reported.
    std::function<void(const QString& clientAddress)> reportAuthFailure;

    /// The stream someone is watching on a host, as the PIN routes need it.
    struct Stream
    {
        bool live = false;     ///< an owner stream of that host is up
        bool supported = true; ///< false on a host that gives each device its own session
        QString key;           ///< what its PIN is kept under (stable across quality switches)
        int appId = -1;
        StreamOwner owner; ///< who started it
    };
    /// The owner stream of @p hostUuid, on @p appId when one is named (>0).
    std::function<Stream(const QString& hostUuid, int appId)> ownerStream;
    /// The keys of every owner stream up right now: the PINs of the others go.
    std::function<QSet<QString>()> liveStreamKeys;
    /// The level a guest who joins by the PIN gets (AppSettings::boothJoinLevel).
    std::function<QString()> joinLevel;
    /// Milliseconds since the epoch, for the proofs' challenges.
    std::function<qint64()> nowMs;
};

/// The permissions a level names ("viewer", "gamer", "desktop", "full");
/// anything else is a viewer's.
ShareManager::Permissions boothPermissionsFor(const QString& level);

/// The opaque digest a booth seat's holder goes by: never the session's token.
QString boothHolderOf(const QString& sessionToken);

/// POST /api/booth/join and POST /api/booth/pin (issue #68). Both answer only
/// when the server runs as a booth with booth_stream_pin on (HttpRequest::boothPin),
/// to a signed-in session that proves its pairing key for that one action on
/// that one host's stream (this machine itself reads its own stream's PIN
/// without one: it has no key and cannot be anybody else). The PIN only ever
/// travels in a POST body or its answer, never in a URL, a log or an error.
void registerBoothRoutes(RestRouter& router, ShareManager& share, BoothPin& pins,
                         OwnerProof& proofs, BoothRoutesDeps deps);
