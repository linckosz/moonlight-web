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

#include <cstdint>
#include <sstream>
#include <string>
#include <vector>

/// ICE priorities for the UPnP copy of a host candidate (RelayBase::emitLocalCandidate).
///
/// The copy names the router's public address and the media slot's hole, but it
/// went out with the priority of the LAN host candidate it copies. A same-LAN
/// browser then held two pairs of equal priority, the direct one and one through
/// the router's hairpin, and ICE took whichever answered first: one session in
/// nine on 09/10/2026 (DualRTX → UM790Pro by cable) nominated the hairpin, the
/// sync round trip 0.5 → 4.7 ms, ~2 ms more capture → draw. A reflexive
/// address's priority makes the direct pair win, while a remote peer, which
/// only gets the copy, still finds the hole.
namespace mw::ice {

/// RFC 8445 §5.1.2.2 recommended type preferences.
inline constexpr uint32_t kHostTypePreference = 126;
inline constexpr uint32_t kSrflxTypePreference = 100;

/// The same priority with a server-reflexive type preference: the local
/// preference and component (the low 24 bits) are kept, so the copies of
/// several interfaces keep their order among themselves.
inline constexpr uint32_t srflxPriority(uint32_t priority)
{
    return (kSrflxTypePreference << 24) | (priority & 0x00FFFFFFu);
}

/// The router-side port the copy of a host candidate is announced on, 0 when
/// the candidate has no copy at the public address.
///
/// A stream with a socket of its own has one hole, and the hole leads to that
/// socket whatever its number: `ownHole`, for any `localPort`. A stream on the
/// server's shared ports (`onSharedPorts`) sends from several, each forwarded
/// to itself when it is forwarded at all: only a candidate on one of
/// `sharedHoles` has a copy, on the same number. A shared port the router does
/// not forward must not be announced at the public address: nothing there
/// leads to this machine.
inline uint16_t publicPortFor(uint16_t localPort, bool onSharedPorts, uint16_t ownHole,
                              const std::vector<uint16_t>& sharedHoles)
{
    if (!onSharedPorts) return ownHole;
    if (localPort == 0) return 0;
    for (const uint16_t port : sharedHoles)
        if (port == localPort) return port;
    return 0;
}

/// The router-side port of the second copy of a passive TCP candidate, where
/// the router leads its TCP 443 to the port the candidate listens on
/// (settings.json ice_tcp_443).
inline constexpr uint16_t kTcp443 = 443;
/// Below a reflexive address's, above a relay's: the last candidate of the
/// connection, whatever its interface.
inline constexpr uint32_t kLastResortTypePreference = 50;

/// The same priority with that type preference: the copy on TCP 443 is below
/// the copy on the port's own number, and below every other candidate.
inline constexpr uint32_t lastResortPriority(uint32_t priority)
{
    return (kLastResortTypePreference << 24) | (priority & 0x00FFFFFFu);
}

/// Whether a candidate that has its copy at the public address on
/// `publicPort` has a second one on TCP 443: a passive TCP candidate, on the
/// very port the router's 443 leads to (`leadsTo`, 0 for none). An active one
/// is never dialled, and a UDP one has nothing on 443.
inline constexpr bool has443Copy(bool tcpPassive, uint16_t publicPort, uint16_t leadsTo)
{
    return tcpPassive && leadsTo != 0 && publicPort == leadsTo;
}

/// The candidate line (RFC 8839: "candidate:<foundation> <component>
/// <transport> <priority> <address> <port> typ <type> ...") with its priority
/// replaced. A line that does not parse that far is returned unchanged.
inline std::string withPriority(const std::string& line, uint32_t priority)
{
    // The priority is the fourth field: skip three fields and their spaces.
    size_t start = 0;
    for (int field = 0; field < 3; ++field) {
        start = line.find(' ', start);
        if (start == std::string::npos) return line;
        start = line.find_first_not_of(' ', start);
        if (start == std::string::npos) return line;
    }
    const size_t end = line.find(' ', start);
    if (end == std::string::npos) return line;
    for (size_t i = start; i < end; ++i)
        if (line[i] < '0' || line[i] > '9') return line;

    std::ostringstream out;
    out << line.substr(0, start) << priority << line.substr(end);
    return out.str();
}

} // namespace mw::ice
