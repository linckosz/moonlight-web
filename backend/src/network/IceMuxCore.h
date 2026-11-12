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

/// Where a datagram goes. `feeds` is empty for a drop. It holds several for
/// what a provisional peer of several agents sends, and for a STUN response
/// that is not a confirmed peer's, which agents still negotiating get too (an
/// agent ignores a transaction that is not its own).
struct Route
{
    std::vector<uint32_t> feeds;
};

/// The routing table. Not thread-safe: IceMux holds one lock around it.
///
/// Two levels. A Binding request for a known user fragment makes its sender a
/// *provisional* peer of that agent, for as long as such requests keep
/// coming. An agent has few of them and makes room among its own, the oldest
/// first: nothing an agent hears touches what another holds. An address can
/// be the provisional peer of several agents, each of which it named, and
/// what it sends then goes to all of them. The agent itself, once it has
/// checked the peer's credentials, *confirms* it: the peer is that agent's
/// alone from then on, and nothing unverified displaces it.
class Router
{
public:
    /// Agents at once: one for each stream, and more than a server runs streams
    static constexpr size_t kAgentsMax = 32;
    /// An agent's provisional peers, and its confirmed ones
    static constexpr size_t kProvisionalPerFeedMax = 32;
    static constexpr size_t kConfirmedPerFeedMax = 32;
    /// How long after its last request a provisional peer stays one
    static constexpr uint64_t kProvisionalLifeMs = 30'000;
    /// How long after it registers an agent still gets STUN responses it is
    /// not known to wait for
    static constexpr uint64_t kNegotiationMs = 60'000;
    /// Such responses passed on per second: from nobody's peer, all sources
    /// together; from one provisional peer, to the agents it has not named
    static constexpr uint32_t kUnclaimedPerSecond = 200;
    static constexpr uint32_t kSharedResponsesPerSecond = 50;
    /// How long a confirmed peer must have sent its agent nothing before
    /// another agent's word can make it that one's. Well above the longest
    /// gap of a stream that runs: on a still picture with no sound, what a
    /// peer sends can thin out to its consent checks, four to six seconds
    /// apart, and some are lost.
    static constexpr uint64_t kConfirmedQuietMs = 15'000;

    /// An agent, known by its ICE user fragment. False when the fragment is
    /// already another agent's, is not kUfragMin to kUfragMax long, or holds a
    /// character ICE does not allow in one, and when kAgentsMax are known.
    bool addAgent(uint32_t feed, std::string_view ufrag, uint64_t nowMs);
    void removeAgent(uint32_t feed);
    bool hasAgents() const noexcept { return !m_Agents.empty(); }

    /// The agent says: this peer passed my integrity check. A peer that
    /// another agent confirmed, and that still sends that agent something
    /// (within kConfirmedQuietMs), stays that agent's: one agent's word does
    /// not take a peer from a stream that is running.
    void confirm(uint32_t feed, const Peer& peer, uint64_t nowMs);

    Route route(const Peer& from, const uint8_t* data, size_t size, uint64_t nowMs);

    /// The addresses that are somebody's provisional peer
    size_t provisionalCount() const noexcept { return m_Provisional.size(); }
    /// The provisional peers of this agent
    size_t provisionalCount(uint32_t feed) const noexcept;
    size_t confirmedCount() const noexcept { return m_Confirmed.size(); }

private:
    struct Agent
    {
        std::string ufrag;
        uint64_t sinceMs = 0;
        size_t confirmed = 0;
        size_t provisional = 0;
    };
    /// An agent the address named, and when it last did
    struct Follower
    {
        uint32_t feed = 0;
        uint64_t seenMs = 0;
    };
    struct Provisional
    {
        std::vector<Follower> followers; // never empty, an agent once at most
        uint64_t sharedWindowMs = 0;     // its responses shared in the current second
        uint32_t sharedInWindow = 0;
    };
    using ProvisionalMap = std::unordered_map<Peer, Provisional, PeerHash>;
    /// A confirmed peer's agent, and when it last handed that agent something
    struct Confirmed
    {
        uint32_t feed = 0;
        uint64_t heardMs = 0;
    };

    void follow(const Peer& peer, uint32_t feed, uint64_t nowMs);
    bool leave(ProvisionalMap::iterator entry, size_t index);
    void leaveOldest(uint32_t feed);
    void addNegotiating(Route& out, uint64_t nowMs) const;

    std::unordered_map<uint32_t, Agent> m_Agents;
    std::unordered_map<std::string, uint32_t> m_ByUfrag;
    std::unordered_map<Peer, Confirmed, PeerHash> m_Confirmed;
    ProvisionalMap m_Provisional;
    uint64_t m_UnclaimedWindowMs = 0;
    uint32_t m_UnclaimedInWindow = 0;
};

/// The least an ICE user fragment of a muxed agent must hold: 8 characters of
/// the base64 alphabet, 48 bits, so that guessing one is not a way in.
inline constexpr size_t kUfragMin = 8;
/// And the most: short enough for a std::string to hold without the heap.
inline constexpr size_t kUfragMax = 15;

} // namespace mw::icemux
