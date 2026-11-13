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

#include "IceMuxTcpCore.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

/// TCP on the ports every stream shares (settings.json ice_udp_mux). A stream
/// that has to connect over TCP has its browser dial one of them; this class
/// listens there, in the server process, for as long as an agent expects such
/// a connection and no longer. It reads the first frame of each connection,
/// finds from it which agent the connection names, and keeps the connection
/// for that agent's worker to be handed (use()). The worker's agent then
/// decides, on that same frame, whether the connection is its peer's.
///
/// It never writes on a connection and never reads past that first frame. It
/// keeps its end of one it handed over until the worker says it has its own
/// (answered()), or two seconds; and one it named to a worker that copies it
/// out itself (nameFor()) until that worker answers or is gone, however long
/// that takes. What it decides is IceMuxTcpCore's; here are the sockets and
/// the one thread that reads them. docs/design/ice-mux.md
class IceMuxTcp
{
public:
    /// Ports listened on at once: as many as the mux holds sockets.
    static constexpr size_t kListenersMax = 8;

    IceMuxTcp() = default;
    ~IceMuxTcp();
    IceMuxTcp(const IceMuxTcp&) = delete;
    IceMuxTcp& operator=(const IceMuxTcp&) = delete;

    /// Called from the listening thread for each connection that is some
    /// agent's: the agent, and the number the connection is kept under
    /// (use(), answered()). Set before start().
    void setDeliver(std::function<void(uint32_t feed, uint32_t number)> deliver)
    {
        m_Deliver = std::move(deliver);
    }
    /// Where what is said of the listeners and of the connections goes: a
    /// port that cannot be listened on, and, each time nothing is expected
    /// any more, what became of the connections since the last time (counts,
    /// and nothing that names a peer or an agent). Called from the listening
    /// thread and from open()'s caller. Set before start().
    void setReporter(std::function<void(const std::string&)> warn,
                     std::function<void(const std::string&)> tell)
    {
        m_Warn = std::move(warn);
        m_Tell = std::move(tell);
    }

    /// The thread, which sleeps until an agent expects a connection.
    bool start();
    void stop();
    bool running() const { return m_Running.load(); }
    /// For the tests, before start(): the listeners take connections from
    /// this machine alone.
    void keepToLoopback() { m_Loopback = true; }

    /// The agent `feed`, known to its peer by `ufrag`, expects a connection
    /// on these ports from now on. `owner` says whose worker it is: any
    /// number but 0, one for each worker. The ports are listened on before
    /// this returns: the peer is told of them right after. False when the
    /// agent cannot be taken (IceMuxTcpCore's Table::open()) or none of the
    /// ports can be listened on; nothing is expected for it then. From any
    /// thread.
    bool open(uint32_t feed, std::string_view ufrag, const std::vector<uint16_t>& ports,
              uint32_t owner);
    /// It expects none any more: connected, failed, gone. A port nobody
    /// expects a connection on is no longer listened on. From any thread.
    void close(uint32_t feed);
    bool expects(uint32_t feed) const;

    /// Whether one of these ports at least can be listened on now: it is, or
    /// a listener of a moment could be bound to it. What is asked before a
    /// stream is told to do without a TCP port of its own. From any thread.
    bool canListen(const std::vector<uint16_t>& ports);

    /// Runs `with` on the connection kept under `number` for the agent
    /// `feed`: its socket, and its first frame. That is where the connection
    /// is described or sent to the worker; the socket is this object's, and
    /// is still open for as long as `with` runs. False, and `with` not run,
    /// when there is none such any more; else what `with` returned.
    bool use(uint32_t feed, uint32_t number,
             const std::function<bool(uintptr_t socket, const uint8_t* frame, size_t size)>& with);
    /// For a worker that copies the connection out of this process itself:
    /// the number of this process's handle on it, its first frame, and the
    /// two ports of the connection, for the worker to hold its copy against.
    /// The number gives nothing to a process that may not already open this
    /// one. From this call on the connection is kept until that worker
    /// answers for it or is gone (release()): a handle let go of before the
    /// worker copied it could be another one's number by then.
    bool nameFor(uint32_t feed, uint32_t number, uint64_t* handle, std::vector<uint8_t>* frame,
                 uint16_t* localPort, uint16_t* remotePort);
#ifdef _WIN32
    /// This process's own handle on the process `pid`, to ask whether it
    /// still runs and which it is, nothing more; null when it cannot be had.
    /// Asked for a worker this process started, while what started it still
    /// holds it: the number is then that process's and nobody else's, and
    /// stays so for as long as this handle is kept.
    static void* holdProcess(unsigned long pid);
    static void letGoOfProcess(void* process);
    /// For a worker this process hands the connection to: a
    /// description of it that the process held under `process` alone can
    /// open, and its first frame. Which process that is, is read off the
    /// handle here, and it must still run: never a number somebody gave.
    bool describeFor(uint32_t feed, uint32_t number, void* process,
                     std::vector<uint8_t>* description, std::vector<uint8_t>* frame);
#endif
    /// The worker `owner` has its own end of the connection `number`, or
    /// will not have it: this process's end is closed. With number 0, the
    /// one kept first for that worker among those not named. From any
    /// thread.
    void answered(uint32_t owner, uint32_t number);
    /// There is nobody to hand the connection `number` to: closed at once,
    /// unless it was named. From any thread.
    void letGo(uint32_t number);
    /// The worker `owner` is gone: its agents expect nothing, and everything
    /// kept for it is closed, named or not. From any thread.
    void release(uint32_t owner);

    mw::icemuxtcp::Table::Counters counters() const;

private:
    struct Listener
    {
        uintptr_t socket = mw::icemuxtcp::kNoSocket;
        uint16_t port = 0; ///< 0: free
        /// Not bound: when to try again, and whether that was said
        uint64_t retryMs = 0;
        bool said = false;
    };

    void run();
    /// A listener for every port a connection is expected on, as far as one
    /// can be bound. With m_Lock held; what there is to say goes to `says`.
    /// `retryNow`: a port that could not be bound a moment ago is tried again
    /// all the same.
    void bindListeners(uint64_t nowMs, bool retryNow, std::vector<std::string>& says);
    bool listensOn(uint16_t port) const;
    /// The listeners of ports nobody expects a connection on are closed.
    /// With m_Lock held, and from the listening thread alone: it is the one
    /// that waits on them.
    void trimListeners();
    void say(const std::vector<std::string>& says) const;
    /// What became of the connections since it was last told, empty when
    /// none came. With m_Lock held.
    std::string summary();

    /// The table, the listeners, and what is to be closed. A connection that
    /// waits for its frame is read and closed by the listening thread alone;
    /// one that was handed over is touched under this lock only (use()), and
    /// closed by that thread too, once it is out of the table.
    mutable std::mutex m_Lock;
    std::condition_variable m_Wake;
    /// Its places and their frames, off whatever stack this object is on
    std::unique_ptr<mw::icemuxtcp::Table> m_Table = std::make_unique<mw::icemuxtcp::Table>();
    std::array<Listener, kListenersMax> m_Listeners;
    mw::icemuxtcp::Closed m_Closing;
    /// What summary() last told
    mw::icemuxtcp::Table::Counters m_Told;
    bool m_Stop = false;
    /// keepToLoopback()
    bool m_Loopback = false;

    std::thread m_Thread;
    std::atomic<bool> m_Running{false};
    std::function<void(uint32_t, uint32_t)> m_Deliver;
    std::function<void(const std::string&)> m_Warn;
    std::function<void(const std::string&)> m_Tell;
};
