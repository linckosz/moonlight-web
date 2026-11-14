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

#include "IceMuxTcp.h"

#include "common/LaunchLock.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
using socklen_type = socklen_t;
constexpr int INVALID_SOCKET = -1;
#endif

using namespace mw::icemuxtcp;

namespace {

/// Connections the system may hold for a listener between two looks at it
constexpr int kBacklog = 8;
/// Connections taken off one listener before the next is looked at
constexpr int kAcceptBurst = 8;
/// Reads of one connection in one turn: its length, its frame, and a spare
constexpr int kReadRounds = 4;
/// How long a port that could not be listened on waits for its next try
constexpr uint64_t kRetryMs = 1000;
/// How late stop() and a new listener are noticed by a thread that waits on
/// the sockets
constexpr long kWaitMicros = 100 * 1000;
/// The pause when there is nothing to wait on but the time
constexpr auto kPause = std::chrono::milliseconds(50);

int lastError()
{
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

bool wouldBlock(int error)
{
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EAGAIN || error == EWOULDBLOCK || error == EINTR;
#endif
}

/// The one way a socket is closed here (this class has a close() of its own)
void closeSocket(SOCKET s)
{
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

void setNonBlocking(SOCKET s)
{
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#endif
}

void setInt(SOCKET s, int level, int name, int value)
{
    setsockopt(s, level, name, reinterpret_cast<const char*>(&value), sizeof(value));
}

uint64_t nowMs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

/// A listener on `port`, IPv4 and IPv6 at once, that no process gets by being
/// started by this one. kNoSocket, and the system's error, when it cannot be.
///
/// Bound as a stream's own listener is: on Windows the port is not held to
/// this process alone, as the UDP one is, because a port so held cannot be
/// listened on again while a connection it took still lives, and one stream
/// after another is reached on this port. Elsewhere the listener binds while
/// such a connection lives, and never beside another listener.
uintptr_t openListener(uint16_t port, bool loopback, int* error)
{
#ifdef _WIN32
    const SOCKET s = WSASocketW(AF_INET6, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
    const SOCKET s = socket(AF_INET6, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
    const SOCKET s = socket(AF_INET6, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
    if (s == INVALID_SOCKET) {
        *error = lastError();
        return kNoSocket;
    }
#ifndef _WIN32
    // The listening thread waits with select(), which knows no higher number
    if (s >= FD_SETSIZE) {
        *error = EMFILE;
        closeSocket(s);
        return kNoSocket;
    }
    setInt(s, SOL_SOCKET, SO_REUSEADDR, 1);
#endif
    setInt(s, IPPROTO_IPV6, IPV6_V6ONLY, 0);
    setNonBlocking(s);

    sockaddr_in6 at{};
    at.sin6_family = AF_INET6;
    at.sin6_port = htons(port);
    if (loopback) {
        // 127.0.0.1, as a socket of both families names it
        at.sin6_addr.s6_addr[10] = 0xFF;
        at.sin6_addr.s6_addr[11] = 0xFF;
        at.sin6_addr.s6_addr[12] = 127;
        at.sin6_addr.s6_addr[15] = 1;
    }
    if (::bind(s, reinterpret_cast<const sockaddr*>(&at), sizeof(at)) != 0 ||
        ::listen(s, kBacklog) != 0) {
        *error = lastError();
        closeSocket(s);
        return kNoSocket;
    }
    return static_cast<uintptr_t>(s);
}

/// The next connection of `listener`, kept from whatever this process starts
/// and non-blocking; INVALID_SOCKET when there is none now. Nothing is asked
/// of the connection beyond that: it may be another process's within seconds.
SOCKET acceptOne(SOCKET listener, sockaddr_in6* from)
{
    socklen_type length = sizeof(*from);
#if defined(__linux__)
    // Kept from children from the instant it exists
    return ::accept4(listener, reinterpret_cast<sockaddr*>(from), &length,
                     SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    // Elsewhere it is kept from them by the line after the accept: under the
    // lock a stream worker is started under, so that none starts in between.
    // Taken alone: the caller holds no lock of this object (LaunchLock.h).
    const std::lock_guard<std::mutex> launch(mw::launch::lock());
    const SOCKET s = ::accept(listener, reinterpret_cast<sockaddr*>(from), &length);
    if (s == INVALID_SOCKET) return s;
#ifdef _WIN32
    SetHandleInformation(reinterpret_cast<HANDLE>(s), HANDLE_FLAG_INHERIT, 0);
#else
    fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
    setNonBlocking(s);
    return s;
#endif
}

/// Whom it comes from, as the places are counted: an IPv4 address that
/// arrived mapped into IPv6 is IPv4.
Source sourceOfAddress(const sockaddr_in6& from)
{
    const uint8_t* a = from.sin6_addr.s6_addr;
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(a, mapped, sizeof(mapped)) == 0) return sourceOf(4, a + 12);
    return sourceOf(6, a);
}

void closeGone(const Gone& gone)
{
    const SOCKET s = static_cast<SOCKET>(gone.socket);
    if (!gone.handed) {
        // Never anybody else's: reset, so that nothing of it lingers on the
        // port. One that was handed over is closed as it is: its options are
        // the worker's too by now.
        linger abortive{};
        abortive.l_onoff = 1;
        abortive.l_linger = 0;
        setsockopt(s, SOL_SOCKET, SO_LINGER, reinterpret_cast<const char*>(&abortive),
                   sizeof(abortive));
    }
    closeSocket(s);
}

/// The two ports of a connection: this machine's, and its peer's.
bool portsOf(SOCKET s, uint16_t* local, uint16_t* remote)
{
    const auto portOf = [](const sockaddr_storage& name, uint16_t* port) {
        if (name.ss_family == AF_INET6)
            *port = ntohs(reinterpret_cast<const sockaddr_in6*>(&name)->sin6_port);
        else if (name.ss_family == AF_INET)
            *port = ntohs(reinterpret_cast<const sockaddr_in*>(&name)->sin_port);
        else
            return false;
        return *port != 0;
    };
    sockaddr_storage here{}, there{};
    socklen_type hereLength = sizeof(here), thereLength = sizeof(there);
    return getsockname(s, reinterpret_cast<sockaddr*>(&here), &hereLength) == 0 &&
           getpeername(s, reinterpret_cast<sockaddr*>(&there), &thereLength) == 0 &&
           portOf(here, local) && portOf(there, remote);
}

std::string count(uint64_t n, const char* what)
{
    return std::to_string(n) + " " + what;
}

/// Why nothing is expected on a port any more, in words.
const char* said(End why)
{
    switch (why) {
    case End::Connected: return "whoever expected a connection there is connected";
    case End::TimedOut: return "no connection was taken in the time one is expected";
    case End::Over: return "whoever expected a connection there is gone";
    case End::Tried: return "a try of the way to this host is over";
    case End::None: break;
    }
    return "nothing is expected there";
}

} // namespace

IceMuxTcp::~IceMuxTcp()
{
    stop();
}

bool IceMuxTcp::start()
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    if (m_Running) return false;
    m_Stop = false;
    m_Running = true;
    m_Thread = std::thread([this] { run(); });
    return true;
}

void IceMuxTcp::stop()
{
    {
        // open() takes nothing from here on
        const std::lock_guard<std::mutex> lock(m_Lock);
        if (!m_Running) return;
        m_Running = false;
        m_Stop = true;
    }
    m_Wake.notify_all();
    m_TokenCame.notify_all();
    m_Thread.join();
    // The thread is gone: what is left is closed here
    const std::lock_guard<std::mutex> lock(m_Lock);
    m_Table->reset(m_Closing);
    for (const Gone& gone : m_Closing)
        closeGone(gone);
    m_Closing.clear();
    for (Listener& listener : m_Listeners) {
        if (listener.socket != kNoSocket) closeSocket(static_cast<SOCKET>(listener.socket));
        listener = Listener{};
    }
}

bool IceMuxTcp::open(uint32_t feed, std::string_view ufrag, const std::vector<uint16_t>& ports,
                     uint32_t owner)
{
    std::vector<std::string> says;
    bool listening = false;
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        if (!m_Running) return false;
        const uint64_t now = nowMs();
        if (!m_Table->open(feed, ufrag, ports.data(), ports.size(), now, owner)) return false;
        // Now, in the caller's own time: the peer is told of these ports
        // right after, and may dial at once
        bindListeners(now, true, says);
        for (const uint16_t port : ports)
            listening = listening || listensOn(port);
        if (!listening) m_Table->close(feed, m_Closing);
    }
    say(says);
    m_Wake.notify_one();
    return listening;
}

void IceMuxTcp::close(uint32_t feed, bool connected)
{
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        m_Table->close(feed, m_Closing, connected ? End::Connected : End::Over);
    }
    m_Wake.notify_one();
}

uint32_t IceMuxTcp::openHere(std::string_view ufrag, const std::vector<uint16_t>& ports)
{
    uint32_t feed = 0;
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        // Numbered apart from the workers' agents (kHereFeeds), and never the
        // number that says "none"
        m_NextHere = (m_NextHere + 1) & ~kHereFeeds;
        if (m_NextHere == 0) m_NextHere = 1;
        feed = kHereFeeds | m_NextHere;
    }
    return open(feed, ufrag, ports, kHere) ? feed : 0;
}

bool IceMuxTcp::take(uint32_t feed, uint32_t number, uintptr_t* socket, std::vector<uint8_t>* frame)
{
    if (!socket || !frame || !hereFeed(feed)) return false;
    const std::lock_guard<std::mutex> lock(m_Lock);
    Table::Held held;
    // Asked about first, taken after: one that is another agent's stays
    // where it is, for whoever it is for
    if (!m_Table->held(number, &held) || held.feed != feed) return false;
    if (!m_Table->take(kHere, number, &held)) return false;
    *socket = held.socket;
    frame->assign(held.frame, held.frame + held.size);
    return true;
}

void IceMuxTcp::closeTaken(uintptr_t socket)
{
    if (socket != kNoSocket) closeGone(Gone{socket, false});
}

uint32_t IceMuxTcp::expectToken(std::string_view fragment, uint16_t port)
{
    std::vector<std::string> says;
    uint32_t id = 0;
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        if (!m_Running) return 0;
        const uint64_t now = nowMs();
        id = m_Table->expectToken(fragment, port, now);
        if (id != 0) {
            // Now, in the caller's own time: it dials right after
            bindListeners(now, true, says);
            if (!listensOn(port)) {
                m_Table->endToken(id, m_Closing);
                id = 0;
            }
        }
    }
    say(says);
    m_Wake.notify_one();
    return id;
}

bool IceMuxTcp::awaitToken(uint32_t id, uint64_t forMs)
{
    bool came = false;
    {
        std::unique_lock<std::mutex> lock(m_Lock);
        m_TokenCame.wait_for(lock, std::chrono::milliseconds(forMs), [this, id] {
            return m_Stop || m_Table->token(id) != Table::Token::Waiting;
        });
        came = m_Table->token(id) == Table::Token::Arrived;
        m_Table->endToken(id, m_Closing);
    }
    m_Wake.notify_one();
    return came;
}

bool IceMuxTcp::expects(uint32_t feed) const
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    return m_Table->expects(feed);
}

bool IceMuxTcp::canListen(const std::vector<uint16_t>& ports)
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    if (!m_Running) return false;
    for (const uint16_t port : ports) {
        if (port == 0) continue;
        if (listensOn(port)) return true;
        // Under the lock every listener is bound under: this try and a real
        // one are never at the same port at once
        int error = 0;
        const uintptr_t tried = openListener(port, m_Loopback, &error);
        if (tried == kNoSocket) continue;
        closeSocket(static_cast<SOCKET>(tried));
        return true;
    }
    return false;
}

bool IceMuxTcp::use(
    uint32_t feed, uint32_t number,
    const std::function<bool(uintptr_t socket, const uint8_t* frame, size_t size)>& with)
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    Table::Held held;
    if (!with || !m_Table->held(number, &held) || held.feed != feed) return false;
    return with(held.socket, held.frame, held.size);
}

bool IceMuxTcp::nameFor(uint32_t feed, uint32_t number, uint64_t* handle,
                        std::vector<uint8_t>* frame, uint16_t* localPort, uint16_t* remotePort)
{
    if (!handle || !frame || !localPort || !remotePort) return false;
    const std::lock_guard<std::mutex> lock(m_Lock);
    Table::Held held;
    if (!m_Table->held(number, &held) || held.feed != feed) return false;
    if (!portsOf(static_cast<SOCKET>(held.socket), localPort, remotePort)) return false;
    // Named last, once nothing can fail any more: from here on only its
    // worker's answer, or its worker's end, lets go of it
    if (!m_Table->name(number)) return false;
    *handle = static_cast<uint64_t>(held.socket);
    frame->assign(held.frame, held.frame + held.size);
    return true;
}

#ifdef _WIN32
void* IceMuxTcp::holdProcess(unsigned long pid)
{
    if (pid == 0) return nullptr;
    return OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
}

void IceMuxTcp::letGoOfProcess(void* process)
{
    if (process) CloseHandle(static_cast<HANDLE>(process));
}

bool IceMuxTcp::describeFor(uint32_t feed, uint32_t number, void* process,
                            std::vector<uint8_t>* description, std::vector<uint8_t>* frame)
{
    const HANDLE target = static_cast<HANDLE>(process);
    if (!target || !description || !frame) return false;
    // Still running, and its number as the system gives it for this handle
    if (WaitForSingleObject(target, 0) != WAIT_TIMEOUT) return false;
    const DWORD pid = GetProcessId(target);
    if (pid == 0) return false;
    return use(feed, number,
               [pid, description, frame](uintptr_t socket, const uint8_t* first, size_t size) {
                   WSAPROTOCOL_INFOW info{};
                   if (WSADuplicateSocketW(static_cast<SOCKET>(socket), pid, &info) != 0)
                       return false;
                   const auto* bytes = reinterpret_cast<const uint8_t*>(&info);
                   description->assign(bytes, bytes + sizeof(info));
                   frame->assign(first, first + size);
                   return true;
               });
}
#endif

void IceMuxTcp::answered(uint32_t owner, uint32_t number)
{
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        m_Table->answered(owner, number, m_Closing);
    }
    m_Wake.notify_one();
}

void IceMuxTcp::letGo(uint32_t number)
{
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        m_Table->letGo(number, m_Closing);
    }
    m_Wake.notify_one();
}

void IceMuxTcp::release(uint32_t owner)
{
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        m_Table->releaseOwner(owner, m_Closing);
    }
    m_Wake.notify_one();
}

Table::Counters IceMuxTcp::counters() const
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    return m_Table->counters();
}

bool IceMuxTcp::listensOn(uint16_t port) const
{
    for (const Listener& listener : m_Listeners)
        if (listener.port == port && listener.socket != kNoSocket) return true;
    return false;
}

void IceMuxTcp::bindListeners(uint64_t now, bool retryNow, std::vector<std::string>& says)
{
    uint16_t wanted[kListenersMax];
    const size_t wantedCount = m_Table->listenPorts(wanted, kListenersMax);
    for (size_t i = 0; i < wantedCount; ++i) {
        Listener* slot = nullptr;
        Listener* room = nullptr;
        for (Listener& listener : m_Listeners) {
            if (listener.port == wanted[i])
                slot = &listener;
            else if (listener.port == 0 && !room)
                room = &listener;
        }
        if (!slot) {
            if (!room) continue;
            slot = room;
            *slot = Listener{};
            slot->port = wanted[i];
        }
        if (slot->socket != kNoSocket || (!retryNow && now < slot->retryMs)) continue;
        int error = 0;
        slot->socket = openListener(wanted[i], m_Loopback, &error);
        if (slot->socket == kNoSocket) {
            slot->retryMs = now + kRetryMs;
            if (!slot->said)
                says.push_back("TCP port " + std::to_string(wanted[i]) +
                               " cannot be listened on, error " + std::to_string(error) +
                               ": another program on this machine has it. A stream that needs "
                               "TCP is not reached there, and whatever the router forwards to "
                               "that port in TCP reaches that program for as long as it has it "
                               "(tried again every second while a stream waits)");
            slot->said = true;
        } else if (slot->said) {
            says.push_back("TCP port " + std::to_string(wanted[i]) + " is listened on again");
            slot->said = false;
        }
    }
}

void IceMuxTcp::trimListeners(std::vector<std::string>& tells)
{
    uint16_t wanted[kListenersMax];
    const size_t wantedCount = m_Table->listenPorts(wanted, kListenersMax);
    for (Listener& listener : m_Listeners) {
        if (listener.port == 0) continue;
        if (std::find(wanted, wanted + wantedCount, listener.port) != wanted + wantedCount)
            continue;
        if (listener.socket != kNoSocket) {
            closeSocket(static_cast<SOCKET>(listener.socket));
            // Each time, with the port and why: what the router forwards
            // there in TCP reaches nothing of this host's from here on
            tells.push_back("TCP port " + std::to_string(listener.port) +
                            " is no longer listened on: " + said(m_Table->endedOn(listener.port)));
        }
        listener = Listener{};
    }
}

void IceMuxTcp::say(const std::vector<std::string>& says) const
{
    if (!m_Warn) return;
    for (const std::string& what : says)
        m_Warn(what);
}

std::string IceMuxTcp::summary()
{
    const Table::Counters now = m_Table->counters();
    const Table::Counters& was = m_Told;
    const uint64_t closedHere = (now.evicted - was.evicted) + (now.timedOut - was.timedOut) +
                                (now.ended - was.ended) + (now.malformed - was.malformed) +
                                (now.unrouted - was.unrouted) + (now.bounded - was.bounded) +
                                (now.unexpected - was.unexpected) + (now.tried - was.tried);
    const uint64_t handed = now.handed - was.handed;
    if (closedHere + handed == 0) return {};
    std::string out = "TCP on the shared ports: " + count(closedHere + handed, "connection(s), ") +
                      count(handed, "handed to a worker (") +
                      count(now.answered - was.answered, "answered for), ") +
                      count(closedHere, "closed here (") +
                      count(now.evicted - was.evicted, "gave way, ") +
                      count(now.timedOut - was.timedOut, "sent no frame in time, ") +
                      count(now.ended - was.ended, "ended first, ") +
                      count(now.malformed - was.malformed, "sent no frame, ") +
                      count(now.unrouted - was.unrouted, "for no agent, ") +
                      count(now.bounded - was.bounded, "over an agent's share, ") +
                      count(now.tried - was.tried, "this host's own tries of the way here, ") +
                      count(now.unexpected - was.unexpected, "when none was expected)");
    m_Told = now;
    return out;
}

void IceMuxTcp::run()
{
    struct Watched
    {
        SOCKET socket = INVALID_SOCKET;
        uint16_t port = 0;
    };
    std::array<Watched, kListenersMax> listeners;
    std::array<SOCKET, kPlaces> waiting;
    Closed closing;
    closing.reserve(kPlaces + kHandedMax);
    std::vector<std::string> says;
    std::vector<std::string> tells;
    std::vector<std::pair<uint32_t, uint32_t>> deliveries;
    deliveries.reserve(kPlaces);

    for (;;) {
        size_t listenerCount = 0;
        bool anyWaiting = false;
        bool tryCame = false;
        std::string told;
        {
            // Asleep for as long as nothing is expected, nothing waits,
            // nothing is kept and nothing is left to close
            std::unique_lock<std::mutex> lock(m_Lock);
            m_Wake.wait(lock, [this] {
                if (m_Stop || !m_Table->idle() || !m_Closing.empty()) return true;
                return std::any_of(m_Listeners.begin(), m_Listeners.end(),
                                   [](const Listener& listener) { return listener.port != 0; });
            });
            if (m_Stop) break;
            const uint64_t now = nowMs();
            m_Table->expire(now, m_Closing);
            // A listener is closed here and by stop(), nowhere else: this is
            // the thread that waits on it, and it is not waiting now
            trimListeners(tells);
            bindListeners(now, false, says);
            for (const Listener& listener : m_Listeners)
                if (listener.socket != kNoSocket)
                    listeners[listenerCount++] = {static_cast<SOCKET>(listener.socket),
                                                  listener.port};
            for (size_t k = 0; k < kPlaces; ++k) {
                const uintptr_t socket = m_Table->socketAt(static_cast<int>(k));
                waiting[k] = socket == kNoSocket ? INVALID_SOCKET : static_cast<SOCKET>(socket);
                anyWaiting = anyWaiting || socket != kNoSocket;
            }
            closing.swap(m_Closing);
            if (m_Table->idle()) told = summary();
        }
        // Out of the table, and so nobody's but this thread's
        for (const Gone& gone : closing)
            closeGone(gone);
        closing.clear();
        say(says);
        says.clear();
        if (m_Tell)
            for (const std::string& what : tells)
                m_Tell(what);
        tells.clear();
        if (!told.empty() && m_Tell) m_Tell(told);

        if (listenerCount == 0 && !anyWaiting) {
            // Connections kept for a worker's word, or a port that cannot be
            // listened on: nothing to wait on but the time
            std::unique_lock<std::mutex> lock(m_Lock);
            m_Wake.wait_for(lock, kPause, [this] { return m_Stop; });
            continue;
        }

        fd_set readable;
        FD_ZERO(&readable);
        SOCKET highest = 0;
        for (size_t i = 0; i < listenerCount; ++i) {
            FD_SET(listeners[i].socket, &readable);
            highest = (std::max)(highest, listeners[i].socket);
        }
        for (const SOCKET socket : waiting) {
            if (socket == INVALID_SOCKET) continue;
            FD_SET(socket, &readable);
            highest = (std::max)(highest, socket);
        }
        timeval wait{0, kWaitMicros};
        const int ready = select(static_cast<int>(highest) + 1, &readable, nullptr, nullptr, &wait);
        if (ready == 0) continue;
        // select() itself refused: it says nothing of any socket, so each is
        // asked, after a pause (none of them blocks)
        const bool blind = ready < 0;
        if (blind) std::this_thread::sleep_for(kPause);

        // The connections that wait, before any new one takes a place
        {
            const std::lock_guard<std::mutex> lock(m_Lock);
            const uint64_t now = nowMs();
            for (size_t k = 0; k < kPlaces; ++k) {
                const SOCKET socket = waiting[k];
                if (socket == INVALID_SOCKET || (!blind && !FD_ISSET(socket, &readable))) continue;
                const int place = static_cast<int>(k);
                // Let go of meanwhile (its agent expects nothing any more):
                // still open, and no longer this place's
                if (m_Table->socketAt(place) != static_cast<uintptr_t>(socket)) continue;
                for (int round = 0; round < kReadRounds; ++round) {
                    const Table::Need need = m_Table->need(place);
                    if (!need.into || need.count == 0) {
                        m_Table->refuse(place, m_Closing);
                        break;
                    }
                    // What the frame still lacks, and never a byte beyond it
                    const int got =
                        static_cast<int>(recv(socket, reinterpret_cast<char*>(need.into),
                                              static_cast<int>(need.count), 0));
                    if (got < 0) {
                        if (!wouldBlock(lastError())) m_Table->drop(place, m_Closing);
                        break;
                    }
                    if (got == 0) {
                        m_Table->drop(place, m_Closing);
                        break;
                    }
                    const Table::Step step = m_Table->got(place, static_cast<size_t>(got));
                    if (step == Table::Step::More) continue;
                    if (step == Table::Step::Close) {
                        m_Table->refuse(place, m_Closing);
                        break;
                    }
                    const Table::Handed handed = m_Table->route(place, now, m_Closing);
                    if (handed.feed != 0) deliveries.emplace_back(handed.feed, handed.number);
                    tryCame = tryCame || handed.token != 0;
                    break;
                }
            }
        }
        // Whoever waits for a try's connection reads the table under the lock
        // that was just let go of
        if (tryCame) m_TokenCame.notify_all();
        for (const auto& [feed, number] : deliveries) {
            // An agent of this process's own takes it itself; a worker's is
            // handed it. The two are numbered apart, and so never confused.
            const auto& deliver = hereFeed(feed) ? m_DeliverHere : m_Deliver;
            if (deliver) deliver(feed, number);
        }
        deliveries.clear();

        // Then the new ones: so many from each listener, and on to the next
        bool stalled = false;
        for (size_t i = 0; i < listenerCount; ++i) {
            if (!blind && !FD_ISSET(listeners[i].socket, &readable)) continue;
            for (int n = 0; n < kAcceptBurst; ++n) {
                sockaddr_in6 from{};
                const SOCKET socket = acceptOne(listeners[i].socket, &from);
                if (socket == INVALID_SOCKET) {
                    // A listener that says it has a connection and gives none
                    // (no descriptor left, say) says so again at once
                    stalled = stalled || !wouldBlock(lastError());
                    break;
                }
                bool kept = from.sin6_family == AF_INET6;
#ifndef _WIN32
                kept = kept && socket < FD_SETSIZE;
#endif
                if (!kept) {
                    closeGone(Gone{static_cast<uintptr_t>(socket), false});
                    continue;
                }
                const std::lock_guard<std::mutex> lock(m_Lock);
                m_Table->accept(static_cast<uintptr_t>(socket), sourceOfAddress(from),
                                listeners[i].port, nowMs(), m_Closing);
            }
        }
        // ...and must not make a busy loop of this one
        if (stalled) std::this_thread::sleep_for(kPause);
    }
}
