/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The ICE mux with its sockets (network/IceMux): two shared UDP ports, two
 * agents' feed sockets on the loopback, and a peer that sends to the ports.
 * What the decisions are is test_ice_mux_core.cpp's; here, that a datagram
 * really travels, behind its header, to the one feed it is for.
 */
#include "test_framework.h"
#include "network/IceMux.h"

#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
using socklen_type = int;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using SOCKET = int;
using socklen_type = socklen_t;
#define closesocket close
#endif

/// The mux's own socket for a port, so that a test can break it under it.
struct IceMuxTestAccess
{
    static uintptr_t socket(const IceMux& mux, size_t index) { return mux.m_Sockets[index]; }
};

namespace {

std::vector<uint8_t> bindingRequest(const char* username)
{
    std::vector<uint8_t> b(20, 0);
    b[1] = 1;
    b[4] = 0x21;
    b[5] = 0x12;
    b[6] = 0xA4;
    b[7] = 0x42;
    const size_t n = std::strlen(username);
    const size_t padded = (n + 3) & ~size_t(3);
    b.resize(24 + padded, 0);
    b[21] = 6; // USERNAME
    b[23] = static_cast<uint8_t>(n);
    std::memcpy(&b[24], username, n);
    b[3] = static_cast<uint8_t>(4 + padded);
    return b;
}

sockaddr_in loopback(uint16_t port)
{
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    a.sin_port = htons(port);
    return a;
}

/// A loopback UDP socket on a port of its own; connected to `peer` when not 0.
SOCKET udpSocket(uint16_t* port, uint16_t peer = 0)
{
    const SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a = loopback(0);
    socklen_type len = sizeof(a);
    bind(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    getsockname(s, reinterpret_cast<sockaddr*>(&a), &len);
    *port = ntohs(a.sin_port);
    if (peer) {
        a = loopback(peer);
        connect(s, reinterpret_cast<const sockaddr*>(&a), sizeof(a));
    }
    return s;
}

void sendTo(SOCKET s, uint16_t port, const void* data, size_t size)
{
    const sockaddr_in to = loopback(port);
    sendto(s, static_cast<const char*>(data), static_cast<int>(size), 0,
           reinterpret_cast<const sockaddr*>(&to), sizeof(to));
}

/// What `s` receives within `ms`, or -1.
int readWithin(SOCKET s, uint8_t* buffer, size_t size, int ms)
{
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(s, &readable);
    timeval wait{ms / 1000, (ms % 1000) * 1000};
    if (select(static_cast<int>(s) + 1, &readable, nullptr, nullptr, &wait) <= 0) return -1;
    return static_cast<int>(recv(s, reinterpret_cast<char*>(buffer), static_cast<int>(size), 0));
}

} // namespace

void run_ice_mux_socket_tests()
{
    SECTION("IceMux: datagrams on the shared ports reach their agent's feed");
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    // Two ports of the media block's far end, where no stream of a test sits
    const uint16_t first = 48571, second = 48572;
    IceMux mux;
    std::string why;
    std::atomic<int> reports{0};
    mux.setReporter([&reports](const std::string&) { ++reports; });
    CHECK(mux.start({first, second}, &why));
    if (!mux.running()) {
        std::fprintf(stderr, "  (%s)\n", why.c_str());
        return;
    }
    // Held: nobody else binds them
    IceMux other;
    CHECK(!other.start({second}, &why));
    CHECK(mux.hopPort() != 0);

    uint16_t feedPortA = 0, feedPortB = 0, peerPort = 0;
    const SOCKET feedA = udpSocket(&feedPortA, mux.hopPort());
    const SOCKET feedB = udpSocket(&feedPortB, mux.hopPort());
    const SOCKET peer = udpSocket(&peerPort);
    const uint32_t a = mux.addAgent("hostAAAA1234");
    const uint32_t b = mux.addAgent("hostBBBB5678");
    CHECK(a != 0 && b != 0 && a != b);
    CHECK(mux.addAgent("hostAAAA1234") == 0);
    mux.setFeedPort(a, feedPortA);
    mux.setFeedPort(b, feedPortB);

    uint8_t got[5000];
    const std::vector<uint8_t> request = bindingRequest("hostBBBB5678:peer");
    const char other5[] = "\x16zzzz"; // not STUN

    // Before any request the peer is nobody
    sendTo(peer, second, other5, 5);
    CHECK(readWithin(feedB, got, sizeof(got), 300) < 0);
    // Its request goes to the agent it names, behind the header: the second
    // socket (1), IPv4, the peer's port and address
    sendTo(peer, second, request.data(), request.size());
    int n = readWithin(feedB, got, sizeof(got), 2000);
    CHECK(n == static_cast<int>(mw::icemux::kFeedHeaderSize + request.size()));
    CHECK(got[0] == 1 && got[1] == 1 && got[2] == 4);
    CHECK(got[4] == (peerPort >> 8) && got[5] == (peerPort & 0xFF));
    CHECK(got[12] == 127 && got[15] == 1);
    CHECK(n > 36 && std::memcmp(got + 36, request.data(), request.size()) == 0);
    CHECK(readWithin(feedA, got, sizeof(got), 200) < 0);
    // And what it sends next follows
    sendTo(peer, second, other5, 5);
    n = readWithin(feedB, got, sizeof(got), 2000);
    CHECK(n == 41 && got[36] == 0x16);

    // The same peer on the first port is another one, until the agent says so
    sendTo(peer, first, other5, 5);
    CHECK(readWithin(feedB, got, sizeof(got), 300) < 0);
    mux.confirm(b, "127.0.0.1", peerPort, 0);
    sendTo(peer, first, other5, 5);
    n = readWithin(feedB, got, sizeof(got), 2000);
    CHECK(n == 41 && got[1] == 0);

    // A datagram longer than an agent reads is dropped, the next one passes
    const std::vector<uint8_t> big(5000, 0x16);
    sendTo(peer, first, big.data(), big.size());
    sendTo(peer, first, "\x16next", 5);
    n = readWithin(feedB, got, sizeof(got), 2000);
    CHECK(n == 41 && got[37] == 'n');

    // The feed hears the mux alone: a datagram sent straight to it is not read
    sendTo(peer, feedPortB, "forged", 6);
    CHECK(readWithin(feedB, got, sizeof(got), 300) < 0);

    // An agent gone hears nothing more
    mux.removeAgent(b);
    sendTo(peer, first, other5, 5);
    CHECK(readWithin(feedB, got, sizeof(got), 300) < 0);

    const IceMux::Counters c = mux.counters();
    CHECK(c.read == 8 && c.passed == 4 && c.dropped == 4 && c.lost == 0);
    CHECK(c.faults == 0 && mux.healthy() && reports == 0);

    SECTION("IceMux: a shared port that no longer reads does not take the thread");
    // The second port's socket is closed under the mux: every read of it
    // fails from now on, and so does a select() that holds it. (No socket is
    // made from here to stop(): the number must not become another one.)
    closesocket(static_cast<SOCKET>(IceMuxTestAccess::socket(mux, 1)));
    for (int i = 0; i < 100 && mux.healthy(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK(!mux.healthy());
    CHECK(reports == 1);
    // Tried again once a second, not in a loop, and said once
    const uint64_t before = mux.counters().faults;
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));
    const uint64_t tries = mux.counters().faults - before;
    CHECK(tries >= 1 && tries <= 6);
    CHECK(reports == 1);
    // The first port serves meanwhile
    const std::vector<uint8_t> forA = bindingRequest("hostAAAA1234:peer");
    sendTo(peer, first, forA.data(), forA.size());
    n = readWithin(feedA, got, sizeof(got), 2000);
    CHECK(n == static_cast<int>(mw::icemux::kFeedHeaderSize + forA.size()) && got[1] == 0);

    mux.stop();
    CHECK(!mux.running());
    CHECK(other.start({second}, &why)); // the ports are free again
    other.stop();
    closesocket(feedA);
    closesocket(feedB);
    closesocket(peer);
}
