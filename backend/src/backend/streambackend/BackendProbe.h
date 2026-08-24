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

#include <QString>
#include <functional>

class QNetworkAccessManager;

/**
 * @brief What a host is, worked out without asking the user.
 *
 * Nobody should have to tell MoonlightWeb whether their machine runs Sunshine,
 * Apollo or Wolf. This is what can be established on our own — and, just as
 * importantly, what cannot.
 *
 * Why serverinfo cannot name the server
 * -------------------------------------
 * It is tempting to read the GameStream `/serverinfo` and branch on the version
 * strings. That does not work, and the reason is structural rather than a gap
 * we could close: every GameStream server impersonates GeForce Experience so
 * that Moonlight clients accept it, so they all report the SAME constants.
 *
 *   Sunshine   appversion 7.1.431.-1  GfeVersion 3.23.0.74  state SUNSHINE_SERVER_FREE
 *   Wolf       appversion 7.1.431.-1  GfeVersion 3.23.0.74  state SUNSHINE_SERVER_FREE
 *
 * Wolf's are literals in its source (moonlight-protocol/moonlight/protocol.hpp,
 * M_VERSION / M_GFE_VERSION, and moonlight.cpp emits "SUNSHINE_SERVER_FREE"),
 * not a coincidence of one build. Apollo is a Sunshine fork and reports
 * Sunshine's. Two live Sunshine hosts here returned those exact strings.
 *
 * So `serverinfo` answers "does this speak GameStream", and the only further
 * distinction it can make honestly is the original NVIDIA software, whose state
 * says MJOLNIR. Anyone tempted to add a version comparison here should know it
 * would be matching a constant three different projects deliberately share.
 *
 * What does work
 * --------------
 * The control plane, not the stream plane:
 *
 *  - MultiSeat runs an HTTP API on port 9550 whose GET /api/system/auth is
 *    explicitly public (ApiServer.cs exempts it so a dashboard can show the
 *    auth toggle before it holds a key). Reaching it needs no credential, which
 *    makes it the one detection that can run unprompted.
 *
 *  - Wolf cannot be probed at all. Its control API listens on a Unix socket and
 *    is only reachable over TCP through a reverse proxy the operator sets up
 *    themselves. There is nothing to knock on, so Wolf is established from the
 *    URL its admin supplies rather than discovered — the type follows from what
 *    answers that URL, and is still never typed in by hand.
 */
namespace BackendProbe {

/// MultiSeat's control API (MultiSeat.Shared Constants: DefaultApiPort).
constexpr quint16 kMultiSeatApiPort = 9550;

/// What a credential-free look at a host is able to prove. Deliberately coarse:
/// each value is something we can actually stand behind.
enum class Detected
{
    Unknown,     ///< nothing usable answered
    GameStream,  ///< speaks GameStream — Sunshine, Apollo or Wolf, see the note above
    NvidiaGfe,   ///< the original GeForce Experience (state says MJOLNIR)
};

QString toString(Detected d);

/// Classify a GameStream serverinfo document. Pure; no network.
Detected classifyServerInfo(const QString& serverInfo);

/// True when @p body is what MultiSeat's public auth endpoint answers. Split
/// out from the request so the shape can be pinned by a test.
bool looksLikeMultiSeatAuth(const QByteArray& body);

/// Knock on <address>:9550 and report whether a MultiSeat control API is there.
/// Never reports an error: absence and unreachability are the same answer to
/// the only question being asked, and a host that is simply not MultiSeat must
/// not produce a warning every poll.
void probeMultiSeat(QNetworkAccessManager* nam, const QString& address,
                    std::function<void(bool present)> cb);

} // namespace BackendProbe
