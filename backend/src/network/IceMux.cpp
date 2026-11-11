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

#include "IceMux.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
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
#define closesocket close
#endif

using namespace mw::icemux;

namespace {

/// The most libjuice reads of a datagram (its BUFFER_SIZE). One byte more is
/// asked for, so that a longer one shows.
constexpr size_t kDatagramMax = 4096;
/// Datagrams taken off one socket before the next is looked at
constexpr int kBurst = 64;
/// The most agents one datagram is copied to (a STUN response several of them
/// may be waiting for)
constexpr size_t kCopiesMax = 32;
/// Reads that fail in a row on one socket, for no datagram's fault, before
/// the socket is left alone for a while
constexpr int kFaultsBeforeRest = 8;
/// How long it is left alone before it is tried again
constexpr uint64_t kRestMs = 1000;
/// The pause when select() has nothing to wait on, or fails itself
constexpr auto kBlindPause = std::chrono::milliseconds(50);

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
    return error == EAGAIN || error == EWOULDBLOCK;
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

/// Nobody else binds this address while we hold it (Windows lets a second
/// process share a port unless the first one says no).
void setExclusive(SOCKET s)
{
#ifdef _WIN32
    setInt(s, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1);
#else
    (void)s; // the default, without SO_REUSEADDR / SO_REUSEPORT
#endif
}

/// A shared port: the options libjuice gives its own UDP socket (udp.c), so
/// that an agent sending through it finds what it expects.
SOCKET openShared(uint16_t port, std::string* why)
{
    const SOCKET s = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (why) *why = "socket() failed, error " + std::to_string(lastError());
        return INVALID_SOCKET;
    }
    setInt(s, IPPROTO_IPV6, IPV6_V6ONLY, 0);
    setExclusive(s);
#if defined(IP_MTU_DISCOVER) && defined(IP_PMTUDISC_DO)
    setInt(s, IPPROTO_IP, IP_MTU_DISCOVER, IP_PMTUDISC_DO);
#ifdef IPV6_MTU_DISCOVER
    setInt(s, IPPROTO_IPV6, IPV6_MTU_DISCOVER, IP_PMTUDISC_DO);
#endif
#endif
    setInt(s, SOL_SOCKET, SO_RCVBUF, 1024 * 1024);
    setInt(s, SOL_SOCKET, SO_SNDBUF, 1024 * 1024);
#ifdef _WIN32
    // An ICMP « port unreachable » that answers a datagram an agent sent must
    // not come back as an error of our next read.
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(s, SIO_UDP_CONNRESET, &off, sizeof(off), nullptr, 0, &got, nullptr, nullptr);
#endif
    setNonBlocking(s);

    sockaddr_in6 any{};
    any.sin6_family = AF_INET6;
    any.sin6_port = htons(port);
    if (bind(s, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) != 0) {
        if (why)
            *why = "UDP port " + std::to_string(port) + " cannot be bound, error " +
                   std::to_string(lastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    return s;
}

/// The socket the datagrams leave from towards the agents, on the loopback.
SOCKET openHop(uint16_t* port, std::string* why)
{
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) {
        if (why) *why = "socket() failed, error " + std::to_string(lastError());
        return INVALID_SOCKET;
    }
    setExclusive(s);
    setInt(s, SOL_SOCKET, SO_SNDBUF, 1024 * 1024);
    setNonBlocking(s);
    sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_type len = sizeof(local);
    if (bind(s, reinterpret_cast<const sockaddr*>(&local), sizeof(local)) != 0 ||
        getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
        if (why) *why = "the loopback socket cannot be bound, error " + std::to_string(lastError());
        closesocket(s);
        return INVALID_SOCKET;
    }
    *port = ntohs(local.sin_port);
    return s;
}

/// The sender as the agents know peers: an IPv4 address that arrived mapped
/// into IPv6 is IPv4 (libjuice does the same to what it reads).
Peer peerOf(const sockaddr_in6& from, int via)
{
    Peer p;
    p.via = static_cast<uint8_t>(via);
    p.port = ntohs(from.sin6_port);
    const uint8_t* a = from.sin6_addr.s6_addr;
    static const uint8_t mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};
    if (std::memcmp(a, mapped, sizeof(mapped)) == 0) {
        p.family = 4;
        std::copy(a + 12, a + 16, p.address.begin());
    } else {
        p.family = 6;
        std::copy(a, a + 16, p.address.begin());
        p.scope = from.sin6_scope_id;
    }
    return p;
}

uint64_t nowMs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

uint64_t nowNs()
{
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count());
}

} // namespace

IceMux::~IceMux()
{
    if (s_Shared == this) s_Shared = nullptr;
    stop();
}

bool IceMux::start(const std::vector<uint16_t>& ports, std::string* why)
{
    if (running() || ports.empty()) return false;
    std::vector<uintptr_t> sockets;
    uint16_t hopPort = 0;
    const SOCKET hop = openHop(&hopPort, why);
    bool ok = hop != INVALID_SOCKET;
    for (size_t i = 0; ok && i < ports.size(); ++i) {
        const SOCKET s = openShared(ports[i], why);
        ok = s != INVALID_SOCKET;
        if (ok) sockets.push_back(static_cast<uintptr_t>(s));
    }
    if (!ok) {
        for (uintptr_t s : sockets)
            closesocket(static_cast<SOCKET>(s));
        if (hop != INVALID_SOCKET) closesocket(hop);
        return false;
    }
    m_Ports = ports;
    m_Sockets = std::move(sockets);
    m_Hop = static_cast<uintptr_t>(hop);
    m_HopPort = hopPort;
    m_Stop = false;
    m_Thread = std::thread([this] { run(); });
    return true;
}

void IceMux::stop()
{
    if (!running()) return;
    m_Stop = true;
    m_Thread.join();
    m_Ailing = 0;
    for (uintptr_t s : m_Sockets)
        closesocket(static_cast<SOCKET>(s));
    closesocket(static_cast<SOCKET>(m_Hop));
    m_Sockets.clear();
    m_Ports.clear();
    m_HopPort = 0;
    const std::lock_guard<std::mutex> lock(m_Lock);
    m_Router = Router();
    m_FeedPorts.clear();
}

uint32_t IceMux::addAgent(const std::string& ufrag)
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    const uint32_t feed = m_NextFeed;
    if (!m_Router.addAgent(feed, ufrag, nowMs())) return 0;
    if (++m_NextFeed == 0) m_NextFeed = 1;
    m_FeedPorts[feed] = 0;
    return feed;
}

void IceMux::setFeedPort(uint32_t feed, uint16_t port)
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    const auto it = m_FeedPorts.find(feed);
    if (it != m_FeedPorts.end()) it->second = port;
}

void IceMux::confirm(uint32_t feed, const std::string& address, uint16_t port, int via)
{
    if (via < 0 || static_cast<size_t>(via) >= m_Sockets.size() || port == 0) return;
    Peer p;
    p.via = static_cast<uint8_t>(via);
    p.port = port;
    in_addr v4{};
    in6_addr v6{};
    if (inet_pton(AF_INET, address.c_str(), &v4) == 1) {
        p.family = 4;
        std::memcpy(p.address.data(), &v4, 4);
    } else if (inet_pton(AF_INET6, address.c_str(), &v6) == 1) {
        p.family = 6;
        std::memcpy(p.address.data(), &v6, 16);
    } else {
        return;
    }
    const std::lock_guard<std::mutex> lock(m_Lock);
    m_Router.confirm(feed, p);
}

void IceMux::removeAgent(uint32_t feed)
{
    const std::lock_guard<std::mutex> lock(m_Lock);
    m_Router.removeAgent(feed);
    m_FeedPorts.erase(feed);
}

#ifdef _WIN32
std::vector<std::vector<uint8_t>> IceMux::shareWith(unsigned long pid, std::string* why)
{
    std::vector<std::vector<uint8_t>> out;
    for (uintptr_t s : m_Sockets) {
        WSAPROTOCOL_INFOW info{};
        if (WSADuplicateSocketW(static_cast<SOCKET>(s), pid, &info) != 0) {
            if (why) *why = "WSADuplicateSocket failed, error " + std::to_string(lastError());
            return {};
        }
        const auto* bytes = reinterpret_cast<const uint8_t*>(&info);
        out.emplace_back(bytes, bytes + sizeof(info));
    }
    return out;
}
#endif

IceMux::Counters IceMux::counters() const
{
    return {m_Read.load(), m_Passed.load(), m_Dropped.load(), m_Lost.load(), m_Faults.load()};
}

void IceMux::run()
{
    // The header's room first, so that a datagram is read where it leaves from
    std::vector<uint8_t> buffer(kFeedHeaderSize + kDatagramMax + 1);
    uint8_t* const datagram = buffer.data() + kFeedHeaderSize;

    // A read that fails for a reason that lasts (the network went down, the
    // socket is no longer one) leaves its socket « readable » at every turn:
    // it must not make a busy loop of this one. A socket whose reads keep
    // failing rests, is said once, and is tried again later; one that reads
    // again, or has nothing to read, is well.
    struct Health
    {
        int faults = 0;
        uint64_t restUntilMs = 0;
        bool ailing = false;
        bool reported = false;
    };
    std::vector<Health> health(m_Sockets.size());
    const auto well = [this](Health& h) {
        h.faults = 0;
        if (h.ailing) {
            h.ailing = false;
            --m_Ailing;
        }
    };

    while (!m_Stop) {
        fd_set readable;
        FD_ZERO(&readable);
        SOCKET highest = 0;
        size_t watched = 0;
        const uint64_t now = nowMs();
        for (size_t i = 0; i < m_Sockets.size(); ++i) {
            if (health[i].restUntilMs > now) continue;
            FD_SET(static_cast<SOCKET>(m_Sockets[i]), &readable);
            highest = (std::max)(highest, static_cast<SOCKET>(m_Sockets[i]));
            ++watched;
        }
        if (watched == 0) {
            std::this_thread::sleep_for(kBlindPause);
            continue;
        }
        timeval wait{0, 250 * 1000}; // how late stop() is noticed, nothing else
        const int ready = select(static_cast<int>(highest) + 1, &readable, nullptr, nullptr, &wait);
        // select() itself refused (one of the sockets is no longer one): it
        // says nothing of the others, so each is asked, after a pause.
        const bool blind = ready < 0;
        if (blind) std::this_thread::sleep_for(kBlindPause);

        for (size_t via = 0; via < m_Sockets.size(); ++via) {
            const SOCKET s = static_cast<SOCKET>(m_Sockets[via]);
            Health& h = health[via];
            if (h.restUntilMs > now) continue;
            // An ailing socket is asked whatever select() said: when nothing
            // arrives, that is how it shows it reads again.
            if (!blind && !h.ailing && !FD_ISSET(s, &readable)) continue;
            for (int n = 0; n < kBurst; ++n) {
                sockaddr_in6 from{};
                socklen_type fromLen = sizeof(from);
                const int got = static_cast<int>(recvfrom(
                    s, reinterpret_cast<char*>(datagram), static_cast<int>(kDatagramMax + 1), 0,
                    reinterpret_cast<sockaddr*>(&from), &fromLen));
                if (got < 0) {
                    const int error = lastError();
                    if (wouldBlock(error)) {
                        well(h);
                        break;
                    }
#ifdef _WIN32
                    // Too long for the buffer (an error on Windows), or the
                    // echo of a refused send: the datagram is gone, on to the
                    // next.
                    if (error == WSAEMSGSIZE || error == WSAECONNRESET || error == WSAENETRESET) {
                        ++m_Read;
                        ++m_Dropped;
                        well(h);
                        continue;
                    }
#endif
                    // Anything else took no datagram off the socket
                    ++m_Faults;
                    if (h.ailing || ++h.faults >= kFaultsBeforeRest) {
                        h.faults = 0;
                        h.restUntilMs = nowMs() + kRestMs;
                        if (!h.reported && m_Report) {
                            m_Report("UDP port " + std::to_string(m_Ports[via]) +
                                     " keeps failing to read, error " + std::to_string(error) +
                                     ": it is tried again every second, and a stream that "
                                     "starts meanwhile binds a port of its own");
                        }
                        h.reported = true;
                        if (!h.ailing) {
                            h.ailing = true;
                            ++m_Ailing;
                        }
                    }
                    break;
                }
                well(h);
                ++m_Read;
                if (static_cast<size_t>(got) > kDatagramMax || got == 0 ||
                    from.sin6_family != AF_INET6) {
                    ++m_Dropped;
                    continue;
                }
                handle(static_cast<int>(via), &from, buffer.data(), static_cast<size_t>(got));
            }
        }
    }
}

void IceMux::handle(int via, const void* sender, uint8_t* packet, size_t size)
{
    const Peer from = peerOf(*static_cast<const sockaddr_in6*>(sender), via);
    uint16_t ports[kCopiesMax];
    size_t count = 0;
    {
        const std::lock_guard<std::mutex> lock(m_Lock);
        const Route route = m_Router.route(from, packet + kFeedHeaderSize, size, nowMs());
        for (uint32_t feed : route.feeds) {
            const auto it = m_FeedPorts.find(feed);
            if (it != m_FeedPorts.end() && it->second != 0 && count < std::size(ports))
                ports[count++] = it->second;
        }
    }
    if (count == 0) {
        ++m_Dropped;
        return;
    }
    writeFeedHeader(packet, from, nowNs());
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    for (size_t i = 0; i < count; ++i) {
        to.sin_port = htons(ports[i]);
        const int sent = static_cast<int>(
            sendto(static_cast<SOCKET>(m_Hop), reinterpret_cast<const char*>(packet),
                   static_cast<int>(kFeedHeaderSize + size), 0,
                   reinterpret_cast<const sockaddr*>(&to), sizeof(to)));
        if (sent < 0)
            ++m_Lost;
        else
            ++m_Passed;
    }
}
