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
    /// The same, for a connection that is for an agent of this process's own
    /// (openHere()): the one that takes it (take()). Set before start().
    void setDeliverHere(std::function<void(uint32_t feed, uint32_t number)> deliver)
    {
        m_DeliverHere = std::move(deliver);
    }
    /// Where what is said of the listeners and of the connections goes: a
    /// port that cannot be listened on; each listener that is closed, with
    /// its port and why nothing is expected there any more; and, each time
    /// nothing is expected at all, what became of the connections since the
    /// last time (counts, and nothing that names a peer or an agent). Called
    /// from the listening thread and from open()'s caller. Set before start().
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
    /// It expects none any more: connected (`connected`, which is only what
    /// the log then says of the port), or failed, gone. A port nobody expects
    /// a connection on is no longer listened on. From any thread.
    void close(uint32_t feed, bool connected = false);
    bool expects(uint32_t feed) const;

    /// An agent that lives in this process (the entry link's tunnel), known
    /// to its peer by `ufrag`, expects a connection on these ports from now
    /// on, as a worker's does and within the same bounds. The number it is
    /// known under here, which is no worker's agent's; 0 when it cannot be
    /// taken or none of the ports can be listened on. Ended by close().
    /// From any thread.
    uint32_t openHere(std::string_view ufrag, const std::vector<uint16_t>& ports);
    /// The connection kept under `number` for that agent is its own from
    /// here on: its socket, which this object forgets and no longer closes,
    /// and its first frame. Nothing is copied: same process. False when
    /// there is none such, or `feed` is not an agent of this process's.
    bool take(uint32_t feed, uint32_t number, uintptr_t* socket, std::vector<uint8_t>* frame);
    /// A connection that was taken, and that its taker will not have after
    /// all (its agent refused it): closed at once, so that nothing of it
    /// lingers on the port.
    static void closeTaken(uintptr_t socket);

    /// A try of the way to this host (network/Tcp443Probe): one connection
    /// is expected on `port` whose first frame names `fragment`
    /// (mw::icemuxtcp::kTokenSize characters, made for this try). The port is
    /// listened on before this returns. The try's number; 0 when it cannot be
    /// made. Such a connection is closed here and is nobody's. From any
    /// thread.
    uint32_t expectToken(std::string_view fragment, uint16_t port);
    /// Waits `forMs` at most for that connection. True when it came. The try
    /// is over either way. From any thread but the listening one.
    bool awaitToken(uint32_t id, uint64_t forMs);

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
    /// The listeners of ports nobody expects a connection on are closed, and
    /// each says so in `tells`. With m_Lock held, and from the listening
    /// thread alone: it is the one that waits on them.
    void trimListeners(std::vector<std::string>& tells);
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
    /// awaitToken(): a try's connection came, or everything stops
    std::condition_variable m_TokenCame;
    /// The last number given to an agent of this process's own
    uint32_t m_NextHere = 0;
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
    std::function<void(uint32_t, uint32_t)> m_DeliverHere;
    std::function<void(const std::string&)> m_Warn;
    std::function<void(const std::string&)> m_Tell;
};
