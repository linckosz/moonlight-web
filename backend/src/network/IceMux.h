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

#include "IceMuxCore.h"
#include "IceMuxTcp.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// The UDP ports every stream shares (settings.json ice_udp_mux). This
/// process binds them and keeps them; each stream worker's ICE agent sends on
/// them directly, through handles of its own, and this class reads them: one
/// thread, which hands every datagram to the agent it is for, over the
/// loopback, behind a header saying where it came from (IceMuxCore).
///
/// It never answers anything itself, and it reads nothing of a datagram while
/// no agent is registered. docs/design/ice-mux.md
///
/// Two ports serve at a time, one in each role, and an agent is given both in
/// the order it prefers them. Which numbers they are is not decided here
/// (IceMuxPorts, with the router): a port is first held, then, once it is
/// known to be the one, made to serve. A port that stops serving because
/// another took its role stays bound and read for the streams that started on
/// it: for as long as a worker it was shared with lives (shareWith(),
/// release()) or an agent is registered on it. Then it is closed.
class IceMux
{
public:
    /// The roles, in the order an agent prefers them: the conferencing port,
    /// then the stream block's. RouterPortCore's kMuxStandard and kMuxBlock
    /// are these numbers.
    static constexpr int kStandard = 0;
    static constexpr int kBlock = 1;
    static constexpr int kRoles = 2;
    /// Sockets bound at once: the two that serve and the ones they replaced
    /// that a stream is still on. Reached, no further port is held until one
    /// of those is let go.
    static constexpr size_t kSocketsMax = 8;
    /// The most sockets one agent is given (libjuice's JUICE_SHARED_SOCKETS_MAX).
    static constexpr size_t kAgentSocketsMax = 4;

    IceMux() = default;
    ~IceMux();
    IceMux(const IceMux&) = delete;
    IceMux& operator=(const IceMux&) = delete;

    /// The one the server process runs, when it runs one (main.cpp sets it,
    /// StreamWorkerHost reads it; both on the main thread).
    static IceMux* shared() { return s_Shared; }
    static void setShared(IceMux* mux) { s_Shared = mux; }

    /// The loopback socket the datagrams leave from, and the reading thread.
    /// No port yet: hold() and serve() bring them.
    bool start(std::string* why);
    void stop();
    bool running() const { return m_Running.load(); }

    /// What became of a port hold() was asked for.
    enum class Held
    {
        /// Not bound: another program on this machine has it, or the mux
        /// holds all the sockets it may.
        Failed,
        /// Bound by this call. Not read and not given to any agent until it
        /// serves; drop() lets it go.
        NewlyBound,
        /// The mux had it already, serving or not.
        AlreadyHeld,
    };
    /// Binds `port` to this process alone (IPv4 and IPv6 at once) and keeps
    /// it. From any thread.
    Held hold(uint16_t port, std::string* why = nullptr);
    /// Lets go of a port hold() bound that never served. Nothing for any
    /// other: a port that has served goes when nobody has it (release()).
    /// From any thread.
    void drop(uint16_t port);
    /// The held port `port` serves `role` from now on: it is read, and the
    /// workers started from now on are given it. The port that served the
    /// role before stays bound and read for the workers that have it, and is
    /// closed when the last of them is gone. False when the port is not held,
    /// or serves the other role.
    bool serve(int role, uint16_t port);
    /// A port is being asked for, from this call to endAsking(): hold(), then
    /// the serve() or the drop() its answer brings. No replaced port is let
    /// go meanwhile, since the answer may be that one of them serves again.
    /// Whoever asks says so before its hold(). From any thread; they nest.
    void beginAsking();
    void endAsking();

    /// The port that serves `role`, 0 when none does.
    uint16_t port(int role) const;
    /// Whether the mux has `port` bound, serving or not. From any thread.
    bool holds(uint16_t port) const;
    /// The sockets that serve, the preferred first: what a new agent is given
    /// (see shareWith()), by the numbers addAgent() takes.
    std::vector<uint8_t> serving() const;
    /// False while a port that serves keeps failing to read (see run()): a
    /// stream that starts meanwhile is better off on a socket of its own.
    bool healthy() const;
    /// Where what is said of such a port goes: that it keeps failing, then
    /// that it reads again; that a replaced port was let go; and, once, that
    /// the mux holds every socket it may. Called from the reading thread, and
    /// from the thread that asked for one port too many; set before start().
    void setReporter(std::function<void(const std::string&)> report)
    {
        m_Report = std::move(report);
    }

    /// The 127.0.0.1 port every datagram leaves from towards the agents. An
    /// agent's feed socket is connect()ed to it, and so hears nobody else.
    uint16_t hopPort() const { return m_HopPort; }

    /// A stream's agent, by the ICE user fragment its peer will name it by,
    /// and the sockets it sends on, in its own order (the place of each is
    /// the number its datagrams reach it under). Returns its feed number, or
    /// 0 when the fragment or the sockets cannot be taken.
    uint32_t addAgent(const std::string& ufrag, const std::vector<uint8_t>& sockets);
    /// The 127.0.0.1 port of the agent's feed socket, once it has one.
    void setFeedPort(uint32_t feed, uint16_t port);
    /// The agent verified this peer (address in text form, as it reports it).
    /// It does not say on which of its sockets: the peer is the agent's on
    /// each, which gives it nothing it had not.
    void confirm(uint32_t feed, const std::string& address, uint16_t port);
    void removeAgent(uint32_t feed);

    /// TCP on these ports (IceMuxTcp): what listens there while an agent
    /// expects a connection, and keeps each connection for its worker.
    /// Started and stopped with the mux.
    IceMuxTcp& tcp() { return m_Tcp; }
    /// The agent `feed` expects a TCP connection from now on, on the ports
    /// it was registered with and under the fragment it was registered by:
    /// both are this object's word, not its worker's. `owner` says whose
    /// worker the agent is (IceMuxTcp::open()). False when it is not
    /// registered, or none of its ports can be listened on. It expects none
    /// any more once it is removed, or tcp().close() says so.
    bool expectTcp(uint32_t feed, uint32_t owner);
    /// Whether a stream that has to connect over TCP can be reached on the
    /// ports that serve: one of them at least can be listened on.
    bool tcpReady();

    /// What a stream worker gets: the sockets that serve, the preferred first.
    struct Share
    {
        /// What the worker holds of the mux from now to release(): these
        /// sockets stay bound and read for it, whatever replaces them. 0 when
        /// nothing was shared.
        uint32_t lease = 0;
        /// Their numbers here, for addAgent()
        std::vector<uint8_t> sockets;
        std::vector<uint16_t> ports;
        /// shareWith(): one WSAPROTOCOL_INFOW each, as the worker's process
        /// and no other can open it
        std::vector<std::vector<uint8_t>> descriptions;
        /// shareByHandle(): this process's own handle of each
        std::vector<uint64_t> handles;
    };
#ifdef _WIN32
    /// For a worker this process hands the sockets to: its plain child, of
    /// its own account, or a worker it put in the console session on the
    /// owner's account. Empty when nothing serves, or on failure.
    Share shareWith(unsigned long pid, std::string* why);
#endif
    /// For a worker this process may not open (one started above it, by the
    /// launcher service or the elevated task): that worker opens this process
    /// and copies the sockets out itself, by these numbers
    /// (streaming/IceMuxHandles). They are no secret and give nothing: only a
    /// process that may already open this one can use them. Empty when
    /// nothing serves.
    Share shareByHandle();
    /// The worker a share went to is gone, or never got it: `lease` is that
    /// share's. A replaced port no other worker has and no agent is
    /// registered on is closed then, by the reading thread, and its number is
    /// free for another port. Nothing for a lease that is not held (given
    /// back already, or of before a stop()). From any thread.
    void release(uint32_t lease);

    struct Counters
    {
        uint64_t read = 0;    ///< datagrams read off the shared ports
        uint64_t passed = 0;  ///< copies handed to an agent
        uint64_t dropped = 0; ///< datagrams nobody was found for
        uint64_t lost = 0;    ///< copies the loopback did not take
        uint64_t faults = 0;  ///< reads that failed for no datagram's fault
    };
    Counters counters() const;

private:
    friend struct IceMuxTestAccess; // tests/test_ice_mux_sockets.cpp

    enum class State : uint8_t
    {
        Free,
        Pending, ///< bound, waiting to serve or to be dropped: not read
        Serving,
        Retired, ///< served once: read, for the workers that have it
    };
    struct Slot
    {
        uintptr_t socket = ~uintptr_t(0);
        uint16_t port = 0;
        State state = State::Free;
        /// The shares it is part of that were not given back
        uint32_t leases = 0;
    };
    /// "Not one of this agent's sockets"
    static constexpr uint8_t kNotIts = 0xFF;
    struct Feed
    {
        uint16_t port = 0;
        /// For each socket here, its place among the agent's own
        std::array<uint8_t, kSocketsMax> via{};
    };

    void run();
    void handle(uint8_t socket, const void* sender, uint8_t* packet, size_t size);

    /// The sockets, which serve, and who was given them. Taken before m_Lock
    /// when both are, never after it. While the mux runs, a socket that
    /// serves or has served is closed by the reading thread alone, between
    /// two reads, and only one nobody has: that is what lets that thread use
    /// one outside the lock, and shareWith() one it holds a lease on.
    mutable std::mutex m_PortsLock;
    std::array<Slot, kSocketsMax> m_Slots;
    std::array<int, kRoles> m_Serving{{-1, -1}};
    /// The shares not given back: the sockets of each
    std::unordered_map<uint32_t, std::vector<uint8_t>> m_Leases;
    uint32_t m_NextLease = 1;
    /// Moves when the set of sockets to read does, or may
    std::atomic<uint32_t> m_PortsVersion{0};
    /// Ports being asked for (beginAsking()): nothing is let go meanwhile
    std::atomic<int> m_Asking{0};
    /// One bit per socket whose reads keep failing
    std::atomic<uint32_t> m_Ailing{0};
    /// That no socket is left to hold a port with has been said
    std::atomic<bool> m_FullSaid{false};

    uintptr_t m_Hop = ~uintptr_t(0);
    uint16_t m_HopPort = 0;
    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::atomic<bool> m_Stop{false};

    mutable std::mutex m_Lock; // the router, the feeds, the next number
    mw::icemux::Router m_Router;
    std::unordered_map<uint32_t, Feed> m_Feeds;
    uint32_t m_NextFeed = 1;

    /// Its own lock and its own thread; never called with one of the locks
    /// above held
    IceMuxTcp m_Tcp;

    std::atomic<uint64_t> m_Read{0}, m_Passed{0}, m_Dropped{0}, m_Lost{0}, m_Faults{0};
    std::function<void(const std::string&)> m_Report;

    static inline IceMux* s_Shared = nullptr;
};
