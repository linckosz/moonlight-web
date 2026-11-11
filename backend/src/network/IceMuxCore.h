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

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

/// What the ICE mux decides, apart from every socket and every thread: which
/// stream worker a datagram that came in on a shared UDP port belongs to.
/// IceMux (the sockets, the thread) calls it; the tests and the fuzz target
/// call it alone. docs/design/ice-mux.md
namespace mw::icemux {

/// What a datagram is, as far as the mux needs to know.
enum class Kind : uint8_t
{
    Other,          ///< DTLS, SRTP, anything that is not STUN
    BindingRequest, ///< STUN Binding request: routed by its USERNAME
    StunResponse,   ///< STUN success or error response
    StunOther,      ///< STUN indication, or a request of another method: dropped
};

struct StunView
{
    Kind kind = Kind::Other;
    /// Binding request only: the part of USERNAME before the colon, which is
    /// the ICE user fragment of the agent the request is for. Points into the
    /// datagram. Empty when the request carries none.
    std::string_view localUfrag;
};

/// Reads the STUN header and, for a Binding request, walks the attributes up
/// to USERNAME. Nothing is allocated and nothing is read past `size`. A
/// datagram that only looks like STUN at its first byte is `Other`.
StunView readStun(const uint8_t* data, size_t size) noexcept;

/// A peer as the mux tells them apart: its address, and the shared socket the
/// datagram came in on.
struct Peer
{
    std::array<uint8_t, 16> address{}; ///< IPv4 in the first four bytes
    uint32_t scope = 0;                ///< IPv6 scope, 0 otherwise
    uint16_t port = 0;                 ///< host order
    uint8_t family = 0;                ///< 4 or 6
    uint8_t via = 0;                   ///< index of the shared socket

    bool operator==(const Peer& o) const noexcept
    {
        return family == o.family && via == o.via && port == o.port && scope == o.scope &&
               address == o.address;
    }
};

struct PeerHash
{
    size_t operator()(const Peer& p) const noexcept;
};

/// The header a datagram travels behind from the mux to its agent: who sent
/// it, on which shared socket, and when the mux read it. The layout is
/// libjuice's (juice_feed_write_header, JUICE_FEED_HEADER_SIZE), written here
/// so that the mux needs none of the library.
inline constexpr size_t kFeedHeaderSize = 36;
void writeFeedHeader(uint8_t* out, const Peer& from, uint64_t timestamp) noexcept;

/// Where a datagram goes. `feeds` is empty for a drop; it holds several only
/// for a STUN response nobody is known to wait for, which every agent still
/// negotiating gets (an agent ignores a transaction that is not its own).
struct Route
{
    std::vector<uint32_t> feeds;
};

/// The routing table. Not thread-safe: IceMux holds one lock around it.
///
/// Two levels. A Binding request for a known user fragment makes its sender a
/// *provisional* peer of that agent: few of them, short-lived, the oldest
/// gives way, and a request naming another agent does not move one that has
/// not lapsed. The agent itself, once it has checked the peer's credentials,
/// *confirms* it: such a peer is never displaced by anything unverified.
class Router
{
public:
    static constexpr size_t kProvisionalMax = 256;
    static constexpr size_t kConfirmedPerFeedMax = 32;
    static constexpr uint64_t kProvisionalLifeMs = 30'000;
    /// How long after it registers an agent still gets unclaimed STUN responses
    static constexpr uint64_t kNegotiationMs = 60'000;
    /// Unclaimed STUN responses passed on per second, all sources together
    static constexpr uint32_t kUnclaimedPerSecond = 200;

    /// An agent, known by its ICE user fragment. False when the fragment is
    /// already another agent's, is not kUfragMin to kUfragMax long, or holds a
    /// character ICE does not allow in one.
    bool addAgent(uint32_t feed, std::string_view ufrag, uint64_t nowMs);
    void removeAgent(uint32_t feed);
    bool hasAgents() const noexcept { return !m_Agents.empty(); }

    /// The agent says: this peer passed my integrity check.
    void confirm(uint32_t feed, const Peer& peer);

    Route route(const Peer& from, const uint8_t* data, size_t size, uint64_t nowMs);

    size_t provisionalCount() const noexcept { return m_Provisional.size(); }
    size_t confirmedCount() const noexcept { return m_Confirmed.size(); }

private:
    struct Agent
    {
        std::string ufrag;
        uint64_t sinceMs = 0;
        size_t confirmed = 0;
    };
    struct Provisional
    {
        uint32_t feed = 0;
        uint64_t seenMs = 0;
    };

    void expire(uint64_t nowMs);

    std::unordered_map<uint32_t, Agent> m_Agents;
    std::unordered_map<std::string, uint32_t> m_ByUfrag;
    std::unordered_map<Peer, uint32_t, PeerHash> m_Confirmed;
    std::unordered_map<Peer, Provisional, PeerHash> m_Provisional;
    uint64_t m_UnclaimedWindowMs = 0;
    uint32_t m_UnclaimedInWindow = 0;
};

/// The least an ICE user fragment of a muxed agent must hold: 8 characters of
/// the base64 alphabet, 48 bits, so that guessing one is not a way in.
inline constexpr size_t kUfragMin = 8;
/// And the most: short enough for a std::string to hold without the heap.
inline constexpr size_t kUfragMax = 15;

} // namespace mw::icemux
