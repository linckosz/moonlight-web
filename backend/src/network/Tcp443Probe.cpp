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

#include "Tcp443Probe.h"

#include "IceMuxTcp.h"

#include <QByteArray>
#include <QRandomGenerator>

#include <algorithm>
#include <chrono>
#include <system_error>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
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

namespace mw::tcp443 {

namespace {

using Clock = std::chrono::steady_clock;

/// How long a wait goes on before it looks whether the dial was cut short.
constexpr uint64_t kSliceMs = 50;

void closeSocket(SOCKET s)
{
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

uint64_t since(Clock::time_point began)
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(Clock::now() - began).count());
}

/// A connection to `address:port` made within `withinMs`, that no process
/// gets by being started by this one; INVALID_SOCKET when the address is not
/// IPv4 in figures, the port is 0, or the handshake did not complete in that
/// time (refused, unanswered, unreachable: all the same to the guard), or
/// when `cutShort` was set meanwhile: the wait looks at it every kSliceMs.
SOCKET connectTo(const QString& address, uint16_t port, uint64_t withinMs,
                 const std::atomic<bool>& cutShort)
{
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    const QByteArray text = address.toLatin1();
    if (port == 0 || inet_pton(AF_INET, text.constData(), &to.sin_addr) != 1) return INVALID_SOCKET;
#ifdef _WIN32
    const SOCKET s = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, nullptr, 0,
                                WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
#elif defined(SOCK_CLOEXEC)
    const SOCKET s = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, IPPROTO_TCP);
#else
    const SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s != INVALID_SOCKET) fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
#ifdef _WIN32
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
#else
    // Waited on with select(), which knows no higher number
    if (s >= FD_SETSIZE) {
        closeSocket(s);
        return INVALID_SOCKET;
    }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    const int quiet = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &quiet, sizeof(quiet));
#endif
#endif
    bool made = ::connect(s, reinterpret_cast<const sockaddr*>(&to), sizeof(to)) == 0;
    if (!made) {
#ifdef _WIN32
        const bool pending = WSAGetLastError() == WSAEWOULDBLOCK;
#else
        const bool pending = errno == EINPROGRESS;
#endif
        // Waited for a slice at a time, so that a dial nobody wants any more
        // is over within one
        const Clock::time_point began = Clock::now();
        while (pending && !cutShort.load()) {
            const uint64_t gone = since(began);
            if (gone >= withinMs) break;
            const uint64_t slice = (std::min)(withinMs - gone, kSliceMs);
            fd_set writable, failed;
            FD_ZERO(&writable);
            FD_ZERO(&failed);
            FD_SET(s, &writable);
            FD_SET(s, &failed);
            timeval wait{};
            wait.tv_sec = static_cast<decltype(wait.tv_sec)>(slice / 1000);
            wait.tv_usec = static_cast<decltype(wait.tv_usec)>((slice % 1000) * 1000);
            const int ready = select(static_cast<int>(s) + 1, nullptr, &writable, &failed, &wait);
            if (ready == 0) continue;
            int error = 0;
            socklen_type size = sizeof(error);
            // Anything that is not a plain "connected" is no connection: a
            // select() cut short, an error it does not name
            made =
                ready > 0 && FD_ISSET(s, &writable) && !FD_ISSET(s, &failed) &&
                getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 &&
                error == 0;
            break;
        }
    }
    if (!made || cutShort.load()) {
        closeSocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/// Whether the whole of `bytes` went out at once: what a frame this small
/// does on a connection just made, or the try is no try.
bool sendWhole(SOCKET s, const std::vector<uint8_t>& bytes)
{
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    return send(s, reinterpret_cast<const char*>(bytes.data()), static_cast<int>(bytes.size()),
                flags) == static_cast<int>(bytes.size());
}

} // namespace

Probe::Probe(IceMuxTcp& listener)
    : m_Listener(listener)
{}

std::string Probe::freshFragment()
{
    // Letters and figures: all an ICE fragment may hold, and no colon, which
    // ends it in a USERNAME
    static const char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
    std::string out(mw::icemuxtcp::kTokenSize, 'A');
    QRandomGenerator* random = QRandomGenerator::system();
    for (char& c : out)
        c = kAlphabet[random->bounded(static_cast<quint32>(sizeof(kAlphabet) - 1))];
    return out;
}

std::vector<uint8_t> Probe::knock(std::string_view fragment)
{
    // USERNAME is "<the fragment>:p": the part before the colon is what the
    // listener reads, the rest stands for a peer's fragment and says nothing
    const size_t name = fragment.size() + 2;
    const size_t padded = (name + 3) & ~size_t(3);
    const size_t message = 20 + 4 + padded;
    std::vector<uint8_t> out(2 + message, 0);
    out[0] = static_cast<uint8_t>(message >> 8);
    out[1] = static_cast<uint8_t>(message);
    uint8_t* m = out.data() + 2;
    m[1] = 0x01; // a Binding request
    m[2] = static_cast<uint8_t>((4 + padded) >> 8);
    m[3] = static_cast<uint8_t>(4 + padded);
    m[4] = 0x21; // the magic cookie
    m[5] = 0x12;
    m[6] = 0xA4;
    m[7] = 0x42;
    QRandomGenerator* random = QRandomGenerator::system();
    for (size_t i = 8; i < 20; ++i) // the transaction's own number
        m[i] = static_cast<uint8_t>(random->bounded(256u));
    m[21] = 0x06; // USERNAME
    m[22] = static_cast<uint8_t>(name >> 8);
    m[23] = static_cast<uint8_t>(name);
    std::copy(fragment.begin(), fragment.end(), m + 24);
    m[24 + fragment.size()] = ':';
    m[24 + fragment.size() + 1] = 'p';
    return out;
}

bool Probe::make(const Dial& dial)
{
    switch (dial.kind) {
    case Dial::Kind::ArrivesHere: return arrivesHere(dial.address, dial.port, dial.leadsHereOn);
    case Dial::Kind::Answers: return answers(dial.address, dial.port);
    case Dial::Kind::None: break;
    }
    return false;
}

void Probe::cancel()
{
    m_Cancelled.store(true);
    // What waits for a try's connection looks at that mark when it is woken
    m_Listener.wakeTokens();
}

bool Probe::arrivesHere(const QString& address, uint16_t port, uint16_t leadsHereOn)
{
    if (leadsHereOn == 0 || m_Cancelled.load()) return false;
    const std::string fragment = freshFragment();
    const Clock::time_point began = Clock::now();
    const auto left = [this, began]() -> uint64_t {
        const uint64_t gone = since(began);
        return gone >= m_TryMs ? 0 : m_TryMs - gone;
    };
    // Expected first, and listened for by the time this returns: then dialled
    const uint32_t id = m_Listener.expectToken(fragment, leadsHereOn);
    if (id == 0) return false;
    const SOCKET s = connectTo(address, port, left(), m_Cancelled);
    const bool sent = s != INVALID_SOCKET && sendWhole(s, knock(fragment));
    // Asked about either way, cut short or not: that is what ends the try.
    // The connection stays open meanwhile, so that what was sent is read
    // before it is over.
    const bool came = m_Listener.awaitToken(id, sent ? left() : 0, &m_Cancelled);
    if (s != INVALID_SOCKET) closeSocket(s);
    // A dial that was cut short says no, whatever came
    return came && !m_Cancelled.load();
}

bool Probe::answers(const QString& address, uint16_t port)
{
    if (m_Cancelled.load()) return false;
    const SOCKET s = connectTo(address, port, m_TryMs, m_Cancelled);
    if (s == INVALID_SOCKET) return false;
    // Nothing was sent, and nothing is: whoever answered sees a connection
    // that came and went
    closeSocket(s);
    return true;
}

// ── The thread the dials are made on ────────────────────────────────────────

Dialler::Dialler(Probe& probe, std::function<void(uint32_t run, bool result)> done)
    : m_Probe(probe)
    , m_Done(std::move(done))
{}

Dialler::~Dialler()
{
    stop();
}

bool Dialler::ask(uint32_t run, const Dial& dial)
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    // One place: never a dial over one that waits there or is being made
    if (m_Stop || m_Waits || m_Making || dial.kind == Dial::Kind::None) return false;
    if (!m_Thread.joinable()) {
        try {
            m_Thread = std::thread([this] { work(); });
        } catch (const std::system_error&) {
            return false;
        }
    }
    m_Number = run;
    m_Dial = dial;
    m_Cancelled = false;
    m_Waits = true;
    m_Wake.notify_one();
    return true;
}

void Dialler::cancel()
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    if (!m_Waits && !m_Making) return;
    m_Cancelled = true;
    // Being made: its waits are cut short. Still waiting: it is not made at
    // all (work()). Told no either way.
    if (m_Making) m_Probe.cancel();
}

void Dialler::stop()
{
    std::thread thread;
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        m_Stop = true;
        if (m_Waits || m_Making) m_Cancelled = true;
        if (m_Making) m_Probe.cancel();
        thread = std::move(m_Thread);
    }
    m_Wake.notify_all();
    if (thread.joinable()) thread.join();
}

void Dialler::work()
{
    std::unique_lock<std::mutex> lock(m_Lock);
    for (;;) {
        m_Wake.wait(lock, [this] { return m_Stop || m_Waits; });
        if (m_Stop) return;
        const uint32_t number = m_Number;
        const Dial what = m_Dial;
        m_Waits = false;
        bool result = false;
        if (!m_Cancelled) {
            // Armed under the lock: a cancel() from here on reaches the
            // probe, and one from before was seen just above
            m_Making = true;
            m_Probe.rearm();
            lock.unlock();
            result = m_Probe.make(what);
            lock.lock();
            m_Making = false;
            if (m_Cancelled) result = false;
        }
        // The place is free, and the dial is answered: in that order, so that
        // whoever is told may ask for the next at once
        m_Cancelled = false;
        m_Dial = Dial{};
        if (m_Stop) return;
        lock.unlock();
        m_Done(number, result);
        lock.lock();
    }
}

} // namespace mw::tcp443
