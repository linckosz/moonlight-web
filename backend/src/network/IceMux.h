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

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

/// The UDP ports every stream shares (settings.json ice_udp_mux). This
/// process binds them once and keeps them; each stream worker's ICE agent
/// sends on them directly, through handles of its own, and this class reads
/// them: one thread, which hands every datagram to the agent it is for, over
/// the loopback, behind a header saying where it came from (IceMuxCore).
///
/// It never answers anything itself, and it reads nothing of a datagram while
/// no agent is registered. docs/design/ice-mux.md
class IceMux
{
public:
    IceMux() = default;
    ~IceMux();
    IceMux(const IceMux&) = delete;
    IceMux& operator=(const IceMux&) = delete;

    /// The one the server process runs, when it runs one (main.cpp sets it,
    /// StreamWorkerHost reads it; both on the main thread).
    static IceMux* shared() { return s_Shared; }
    static void setShared(IceMux* mux) { s_Shared = mux; }

    /// Binds every port, in this order of preference (IPv4 and IPv6 at once,
    /// to this process alone), and starts reading. All of them or none.
    bool start(const std::vector<uint16_t>& ports, std::string* why);
    void stop();
    bool running() const { return m_Thread.joinable(); }
    /// False while a shared port keeps failing to read (see run()): a stream
    /// that starts meanwhile is better off on a socket of its own.
    bool healthy() const { return m_Ailing.load() == 0; }
    /// Where the one line about such a port goes. Called from the reading
    /// thread; set before start().
    void setReporter(std::function<void(const std::string&)> report)
    {
        m_Report = std::move(report);
    }
    const std::vector<uint16_t>& ports() const { return m_Ports; }

    /// The 127.0.0.1 port every datagram leaves from towards the agents. An
    /// agent's feed socket is connect()ed to it, and so hears nobody else.
    uint16_t hopPort() const { return m_HopPort; }

    /// A stream's agent, by the ICE user fragment its peer will name it by.
    /// Returns its feed number, or 0 when the fragment cannot be taken.
    uint32_t addAgent(const std::string& ufrag);
    /// The 127.0.0.1 port of the agent's feed socket, once it has one.
    void setFeedPort(uint32_t feed, uint16_t port);
    /// The agent verified this peer (address in text form, as it reports it).
    void confirm(uint32_t feed, const std::string& address, uint16_t port, int via);
    void removeAgent(uint32_t feed);

#ifdef _WIN32
    /// The shared sockets, as process `pid` and no other can open them: one
    /// WSAPROTOCOL_INFOW each, in the order of ports(). Empty on failure.
    std::vector<std::vector<uint8_t>> shareWith(unsigned long pid, std::string* why);
#endif

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

    void run();
    void handle(int via, const void* sender, uint8_t* packet, size_t size);

    std::vector<uint16_t> m_Ports;
    std::vector<uintptr_t> m_Sockets;
    uintptr_t m_Hop = ~uintptr_t(0);
    uint16_t m_HopPort = 0;
    std::thread m_Thread;
    std::atomic<bool> m_Stop{false};

    mutable std::mutex m_Lock; // the router, the feed ports, the next number
    mw::icemux::Router m_Router;
    std::unordered_map<uint32_t, uint16_t> m_FeedPorts;
    uint32_t m_NextFeed = 1;

    std::atomic<uint64_t> m_Read{0}, m_Passed{0}, m_Dropped{0}, m_Lost{0}, m_Faults{0};
    std::atomic<uint32_t> m_Ailing{0}; // shared sockets that keep failing to read
    std::function<void(const std::string&)> m_Report;

    static inline IceMux* s_Shared = nullptr;
};
