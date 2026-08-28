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
 *  - Sunshine and Apollo carry a management REST API of their own, and it names
 *    itself without credentials: an unauthenticated GET answers 401 with
 *    `WWW-Authenticate: Basic realm="Sunshine Gamestream Host"`. Verified live
 *    against two independent Sunshine hosts. It sits one port above the
 *    GameStream HTTP port, and BOTH families derive their ports from the same
 *    base, so it is looked for at `httpPort + 1` rather than at a fixed 47990 —
 *    an operator who moved the base port keeps working.
 *
 *  - Wolf cannot be probed positively. Its control API listens on a Unix socket,
 *    reachable over TCP only through a reverse proxy the operator sets up
 *    themselves, at an address only they know. There is nothing to knock on.
 *
 * Which is why the Sunshine probe matters beyond Sunshine: on a host that speaks
 * GameStream, is not MJOLNIR, and has NO Sunshine REST API, the one thing we
 * know of that fits is Wolf. That inference is worth acting on but is NOT proof
 * — a fourth GameStream server would match it too — so it may be used to OFFER
 * to set a control API up, never to label the host. The type still comes from
 * what answers the URL its admin supplies, and is never typed in by hand.
 */
namespace BackendProbe {

/// MultiSeat's control API (MultiSeat.Shared Constants: DefaultApiPort).
constexpr quint16 kMultiSeatApiPort = 9550;

/// What a credential-free look at a host is able to prove. Deliberately coarse:
/// each value is something we can actually stand behind.
enum class Detected
{
    Unknown,    ///< nothing usable answered
    GameStream, ///< speaks GameStream — Sunshine, Apollo or Wolf, see the note above
    NvidiaGfe,  ///< the original GeForce Experience (state says MJOLNIR)
};

QString toString(Detected d);

/// Classify a GameStream serverinfo document. Pure; no network.
Detected classifyServerInfo(const QString& serverInfo);

/// True when @p body is what MultiSeat's public auth endpoint answers. Split
/// out from the request so the shape can be pinned by a test.
bool looksLikeMultiSeatAuth(const QByteArray& body);

/// Whether a host carries the Sunshine-family management REST API.
///
/// Three values rather than a bool because the difference between "it is not
/// there" and "we could not tell" is the whole safety of the inference drawn
/// from a negative: a Sunshine host behind a firewall must land on Unknown, or
/// it would be offered a control-API setup it has no use for.
enum class SunshineRest
{
    Unknown, ///< no usable answer — assume nothing
    Present, ///< answered as Sunshine/Apollo
    Absent,  ///< definitely answered, and it is not that API
};

/// Port the Sunshine-family web/REST API sits on for a host whose GameStream
/// HTTP port is @p httpPort. Both families lay their ports out from one base, so
/// this follows the base instead of hard-coding 47990.
constexpr quint16 sunshineRestPort(quint16 httpPort)
{
    return static_cast<quint16>(httpPort + 1);
}

/// How far a probe got. The distinction between a refusal and silence is the
/// whole correctness of this: a refused connection is an ANSWER — nothing is
/// listening there — while silence could be a firewall in front of a host that
/// has the API after all.
///
/// Measured against a live Wolf host: its GameStream ports connect in ~2 ms and
/// httpPort + 1 comes back ConnectionRefused. Reading that refusal as silence
/// would leave the one host this exists for classified as Unknown.
enum class Reach
{
    NoAnswer, ///< timed out, unresolvable, TLS never completed
    Refused,  ///< the port actively refused — definitely nothing there
    Answered, ///< an HTTP response came back
};

/// Classify an unauthenticated answer from that API. Pure; no network.
SunshineRest classifySunshineRest(Reach reach, int httpStatus, const QByteArray& wwwAuthenticate);

/// Knock on <address>:<sunshineRestPort(httpPort)>. Never reports an error: a
/// host that is not Sunshine is a normal answer, not a fault to log every poll.
/// Uses a socket of its own rather than the shared network manager: the three
/// outcomes above are a transport-level distinction, and QNAM collapses all of
/// them into one timeout.
void probeSunshineRest(const QString& address, quint16 httpPort,
                       std::function<void(SunshineRest result)> cb);

/// Work out which backend answers at an address an admin supplied, by asking it.
/// Reports the type name, or an empty string when nothing we know of answered.
///
/// This is what keeps the promise that a type is never typed in by hand: for a
/// backend we cannot find on our own, the admin gives the one thing only they
/// know — where it is — and the product identifies itself.
void identifyControlApi(QNetworkAccessManager* nam, const QString& apiUrl, const QString& apiToken,
                        std::function<void(QString type)> cb);

/// Knock on <address>:9550 and report whether a MultiSeat control API is there.
/// Never reports an error: absence and unreachability are the same answer to
/// the only question being asked, and a host that is simply not MultiSeat must
/// not produce a warning every poll.
void probeMultiSeat(QNetworkAccessManager* nam, const QString& address,
                    std::function<void(bool present)> cb);

} // namespace BackendProbe
