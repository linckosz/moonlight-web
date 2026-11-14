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

void closeSocket(SOCKET s)
{
#ifdef _WIN32
    ::closesocket(s);
#else
    ::close(s);
#endif
}

/// A connection to `address:port` made within `withinMs`, that no process
/// gets by being started by this one; INVALID_SOCKET when the address is not
/// IPv4 in figures, the port is 0, or the handshake did not complete in that
/// time (refused, unanswered, unreachable: all the same to the guard).
SOCKET dial(const QString& address, uint16_t port, uint64_t withinMs)
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
        fd_set writable, failed;
        FD_ZERO(&writable);
        FD_ZERO(&failed);
        FD_SET(s, &writable);
        FD_SET(s, &failed);
        timeval wait{};
        wait.tv_sec = static_cast<decltype(wait.tv_sec)>(withinMs / 1000);
        wait.tv_usec = static_cast<decltype(wait.tv_usec)>((withinMs % 1000) * 1000);
        int error = 0;
        socklen_type size = sizeof(error);
        // Anything that is not a plain "connected" is no connection: a
        // select() cut short, an error it does not name
        made = pending && select(static_cast<int>(s) + 1, nullptr, &writable, &failed, &wait) > 0 &&
               FD_ISSET(s, &writable) && !FD_ISSET(s, &failed) &&
               getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&error), &size) == 0 &&
               error == 0;
    }
    if (!made) {
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

uint64_t since(Clock::time_point began)
{
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(Clock::now() - began).count());
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

bool Probe::arrivesHere(const QString& address, uint16_t port)
{
    if (m_LocalPort == 0) return false;
    const std::string fragment = freshFragment();
    const Clock::time_point began = Clock::now();
    const auto left = [this, began]() -> uint64_t {
        const uint64_t gone = since(began);
        return gone >= m_TryMs ? 0 : m_TryMs - gone;
    };
    // Expected first, and listened for by the time this returns: then dialled
    const uint32_t id = m_Listener.expectToken(fragment, m_LocalPort);
    if (id == 0) return false;
    const SOCKET s = dial(address, port, left());
    const bool sent = s != INVALID_SOCKET && sendWhole(s, knock(fragment));
    // Asked about either way: that is what ends the try. The connection
    // stays open meanwhile, so that what was sent is read before it is over.
    const bool came = m_Listener.awaitToken(id, sent ? left() : 0);
    if (s != INVALID_SOCKET) closeSocket(s);
    return came;
}

bool Probe::answers(const QString& address, uint16_t port)
{
    const SOCKET s = dial(address, port, m_TryMs);
    if (s == INVALID_SOCKET) return false;
    // Nothing was sent, and nothing is: whoever answered sees a connection
    // that came and went
    closeSocket(s);
    return true;
}

} // namespace mw::tcp443
