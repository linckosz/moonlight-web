/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The ICE mux with its sockets (network/IceMux): shared UDP ports, agents'
 * feed sockets on the loopback, and a peer that sends to the ports.
 * What the decisions are is test_ice_mux_core.cpp's; here, that a datagram
 * really travels, behind its header, to the one feed it is for, and what
 * becomes of a port as it is held, serves, and gives its role to another.
 */
#include "test_framework.h"
#include "network/IceMux.h"
#include "network/RouterPortPools.h"

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
    static uintptr_t socket(const IceMux& mux, uint16_t port)
    {
        for (const IceMux::Slot& slot : mux.m_Slots)
            if (slot.state != IceMux::State::Free && slot.port == port) return slot.socket;
        return ~uintptr_t(0);
    }
};

namespace {

using Held = IceMux::Held;

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

/// `count` ports of the stream block nobody has bound right now, from its far
/// end, where no stream of a test sits. Fewer when the block is that busy.
std::vector<uint16_t> freeBlockPorts(size_t count)
{
    std::vector<uint16_t> ports;
    IceMux probe;
    if (!probe.start(nullptr)) return ports;
    using namespace mw::routerports;
    for (int p = kMediaBasePort + kMediaPortCount - 1; p >= kMediaBasePort && ports.size() < count;
         --p) {
        const auto port = static_cast<uint16_t>(p);
        if (probe.hold(port) != Held::NewlyBound) continue;
        probe.drop(port);
        ports.push_back(port);
    }
    return ports;
}

/// Lets the reading thread take a turn: it looks at its sockets again every
/// tenth of a second at the latest.
void settle()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

/// Whether the mux lets go of `port` in good time: the reading thread does
/// it, at its next look at its sockets.
bool letsGo(const IceMux& mux, uint16_t port)
{
    for (int waited = 0; waited < 3000 && mux.holds(port); waited += 20)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return !mux.holds(port);
}

} // namespace

void run_ice_mux_socket_tests()
{
    SECTION("IceMux: datagrams on the shared ports reach their agent's feed");
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const std::vector<uint16_t> two = freeBlockPorts(2);
    CHECK(two.size() == 2);
    if (two.size() < 2) return;
    const uint16_t first = two[0], second = two[1];
    IceMux mux;
    std::string why;
    std::atomic<int> reports{0};
    mux.setReporter([&reports](const std::string&) { ++reports; });
    CHECK(mux.start(&why));
    if (!mux.running()) {
        std::fprintf(stderr, "  (%s)\n", why.c_str());
        return;
    }
    CHECK(!mux.start(&why)); // once
    CHECK(mux.hold(first) == Held::NewlyBound);
    CHECK(mux.hold(second) == Held::NewlyBound);
    CHECK(mux.hold(first) == Held::AlreadyHeld);
    CHECK(mux.serve(IceMux::kStandard, first));
    CHECK(mux.serve(IceMux::kBlock, second));
    CHECK(mux.port(IceMux::kStandard) == first && mux.port(IceMux::kBlock) == second);
    // Held: nobody else binds them
    IceMux other;
    CHECK(other.start(&why));
    CHECK(other.hold(second, &why) == Held::Failed);
    CHECK(!other.holds(second));
    CHECK(mux.hopPort() != 0);

    uint16_t feedPortA = 0, feedPortB = 0, peerPort = 0;
    const SOCKET feedA = udpSocket(&feedPortA, mux.hopPort());
    const SOCKET feedB = udpSocket(&feedPortB, mux.hopPort());
    const SOCKET peer = udpSocket(&peerPort);
    // The conferencing port first: the order an agent prefers them in
    const std::vector<uint8_t> both = mux.serving();
    CHECK(both.size() == 2);
    const uint32_t a = mux.addAgent("hostAAAA1234", both);
    const uint32_t b = mux.addAgent("hostBBBB5678", both);
    CHECK(a != 0 && b != 0 && a != b);
    CHECK(mux.addAgent("hostAAAA1234", both) == 0);
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
    mux.confirm(b, "127.0.0.1", peerPort);
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
    closesocket(static_cast<SOCKET>(IceMuxTestAccess::socket(mux, second)));
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
    CHECK(mux.hold(first) == Held::Failed);        // stopped: it binds nothing
    CHECK(other.hold(second) == Held::NewlyBound); // the ports are free again
    other.stop();
    closesocket(feedA);
    closesocket(feedB);

    SECTION("IceMux: a port is held, then serves or is let go");
    const std::vector<uint16_t> three = freeBlockPorts(3);
    CHECK(three.size() == 3);
    if (three.size() < 3) {
        closesocket(peer);
        return;
    }
    const uint16_t pa = three[0], pb = three[1], pc = three[2];
    IceMux m;
    CHECK(m.start(&why));
    CHECK(other.start(&why));
    // Nothing serves yet: nothing to give an agent
    CHECK(m.serving().empty() && m.port(IceMux::kBlock) == 0 && !m.holds(pa));
    CHECK(m.addAgent("hostCCCC1234", {}) == 0);
    CHECK(m.hold(0) == Held::Failed);
    CHECK(m.hold(pa) == Held::NewlyBound);
    CHECK(m.holds(pa) && m.serving().empty());
    // Held and not serving: no agent is given it, and nothing is read off it
    CHECK(m.addAgent("hostCCCC1234", {0}) == 0);
    sendTo(peer, pa, other5, 5);
    settle();
    CHECK(m.counters().read == 0);
    // Let go, it is anybody's again
    m.drop(pa);
    CHECK(!m.holds(pa));
    CHECK(other.hold(pa) == Held::NewlyBound);
    other.drop(pa);
    // What waited on a held port is read once the port serves, and goes to
    // nobody: no agent was there to hear it
    CHECK(m.hold(pa) == Held::NewlyBound);
    sendTo(peer, pa, other5, 5);
    CHECK(m.serve(IceMux::kBlock, pa));
    CHECK(m.serve(IceMux::kBlock, pa)); // again: as it was
    settle();
    CHECK(m.counters().read == 1 && m.counters().dropped == 1 && m.counters().passed == 0);
    // A port that serves is not let go by drop()
    m.drop(pa);
    CHECK(m.holds(pa) && m.port(IceMux::kBlock) == pa);
    CHECK(other.hold(pa) == Held::Failed);
    // serve() takes a held port, a role there is, and a socket in one role
    CHECK(!m.serve(IceMux::kStandard, pb));
    CHECK(!m.serve(IceMux::kStandard, pa));
    CHECK(!m.serve(IceMux::kRoles, pa) && !m.serve(-1, pa));
    // The mux holds a port in IPv4 as in IPv6: a port an IPv4 program has is
    // not held, and a held one is no IPv4 program's to bind (the tunnel's
    // probe, a stream's own socket)
    {
        sockaddr_in any{};
        any.sin_family = AF_INET;
        any.sin_port = htons(pb);
        const SOCKET theirs = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(bind(theirs, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) == 0);
        CHECK(m.hold(pb) == Held::Failed);
        CHECK(!m.holds(pb));
        closesocket(theirs);
        any.sin_port = htons(pa);
        const SOCKET late = socket(AF_INET, SOCK_DGRAM, 0);
        CHECK(bind(late, reinterpret_cast<const sockaddr*>(&any), sizeof(any)) != 0);
        closesocket(late);
    }

    SECTION("IceMux: an agent hears a socket under its own number, and its own sockets only");
    // X is registered while the block port is the only one: it has that one
    uint16_t feedPortX = 0, feedPortY = 0, feedPortZ = 0;
    const SOCKET feedX = udpSocket(&feedPortX, m.hopPort());
    const SOCKET feedY = udpSocket(&feedPortY, m.hopPort());
    const SOCKET feedZ = udpSocket(&feedPortZ, m.hopPort());
    const std::vector<uint8_t> one = m.serving();
    CHECK(one.size() == 1);
    const uint32_t x = m.addAgent("hostXXXX1234", one);
    CHECK(x != 0);
    m.setFeedPort(x, feedPortX);
    // An agent names each socket once, and only sockets there are
    CHECK(m.addAgent("hostWWWW1234", {one[0], one[0]}) == 0);
    CHECK(m.addAgent("hostWWWW1234", {static_cast<uint8_t>(IceMux::kSocketsMax)}) == 0);
    CHECK(m.addAgent("hostWWWW1234", {0, 1, 2, 3, 4}) == 0);
    // The conferencing port comes to serve: the agents registered from now on
    // are given both, that one first
    CHECK(m.hold(pb) == Held::NewlyBound);
    CHECK(m.serve(IceMux::kStandard, pb));
    const std::vector<uint8_t> pair = m.serving();
    CHECK(pair.size() == 2 && pair[1] == one[0]);
    const uint32_t y = m.addAgent("hostYYYY1234", pair);
    CHECK(y != 0);
    m.setFeedPort(y, feedPortY);
    const std::vector<uint8_t> forX = bindingRequest("hostXXXX1234:peer");
    const std::vector<uint8_t> forY = bindingRequest("hostYYYY1234:peer");
    const std::vector<uint8_t> forZ = bindingRequest("hostZZZZ1234:peer");
    // The block port is Y's second socket and X's first
    sendTo(peer, pa, forY.data(), forY.size());
    n = readWithin(feedY, got, sizeof(got), 2000);
    CHECK(n == static_cast<int>(mw::icemux::kFeedHeaderSize + forY.size()) && got[1] == 1);
    sendTo(peer, pa, forX.data(), forX.size());
    n = readWithin(feedX, got, sizeof(got), 2000);
    CHECK(n == static_cast<int>(mw::icemux::kFeedHeaderSize + forX.size()) && got[1] == 0);
    // The conferencing port is Y's first, and not X's at all: X could not
    // answer through it, and is not handed what names it there
    sendTo(peer, pb, forY.data(), forY.size());
    n = readWithin(feedY, got, sizeof(got), 2000);
    CHECK(n == static_cast<int>(mw::icemux::kFeedHeaderSize + forY.size()) && got[1] == 0);
    const uint64_t droppedBefore = m.counters().dropped;
    sendTo(peer, pb, forX.data(), forX.size());
    CHECK(readWithin(feedX, got, sizeof(got), 300) < 0);
    CHECK(m.counters().dropped == droppedBefore + 1);

    SECTION("IceMux: a port that gave its role to another keeps the agents it has");
    CHECK(m.hold(pc) == Held::NewlyBound);
    CHECK(m.serve(IceMux::kBlock, pc));
    CHECK(m.port(IceMux::kBlock) == pc && m.port(IceMux::kStandard) == pb);
    // Still bound, still the mux's, and still X's and Y's
    CHECK(m.holds(pa) && other.hold(pa) == Held::Failed);
    sendTo(peer, pa, forX.data(), forX.size());
    n = readWithin(feedX, got, sizeof(got), 2000);
    CHECK(n == static_cast<int>(mw::icemux::kFeedHeaderSize + forX.size()) && got[1] == 0);
    // An agent registered now is given the two that serve, not that one
    const std::vector<uint8_t> now = m.serving();
    CHECK(now.size() == 2 && now[0] == pair[0] && now[1] != pair[1]);
    const uint32_t z = m.addAgent("hostZZZZ1234", now);
    CHECK(z != 0);
    m.setFeedPort(z, feedPortZ);
    sendTo(peer, pa, forZ.data(), forZ.size());
    CHECK(readWithin(feedZ, got, sizeof(got), 300) < 0);
    sendTo(peer, pc, forZ.data(), forZ.size());
    n = readWithin(feedZ, got, sizeof(got), 2000);
    CHECK(n == static_cast<int>(mw::icemux::kFeedHeaderSize + forZ.size()) && got[1] == 1);
    // A peer an agent confirms is its own on each of its sockets, and on no
    // other: on the conferencing port the peer stays what it was, Y's
    m.confirm(x, "127.0.0.1", peerPort);
    sendTo(peer, pa, other5, 5);
    n = readWithin(feedX, got, sizeof(got), 2000);
    CHECK(n == 41 && got[1] == 0);
    CHECK(readWithin(feedY, got, sizeof(got), 200) < 0);
    sendTo(peer, pb, other5, 5);
    n = readWithin(feedY, got, sizeof(got), 2000);
    CHECK(n == 41 && got[1] == 0);
    CHECK(readWithin(feedX, got, sizeof(got), 200) < 0);
    // Asked for again, the retired port is the mux's already, and may serve
    CHECK(m.hold(pa) == Held::AlreadyHeld);
    CHECK(m.serve(IceMux::kBlock, pa));
    CHECK(m.port(IceMux::kBlock) == pa && m.holds(pc));
    CHECK(m.healthy());
    m.stop();
    CHECK(!m.holds(pa) && m.serving().empty());
    closesocket(feedX);
    closesocket(feedY);
    closesocket(feedZ);

    SECTION("IceMux: a port that was replaced is let go once nobody has it");
    {
        IceMux r;
        std::atomic<int> told{0};
        r.setReporter([&told](const std::string& what) {
            if (what.find("is let go") != std::string::npos) ++told;
        });
        CHECK(r.start(&why));
        // Nothing serves: nothing to share, and no lease
        CHECK(r.shareByHandle().lease == 0);
        CHECK(r.hold(pa) == Held::NewlyBound && r.serve(IceMux::kBlock, pa));
        // A worker is given the port, then the port is replaced: it stays
        // bound and read, for that worker
        const IceMux::Share worker = r.shareByHandle();
        CHECK(worker.lease != 0 && worker.sockets.size() == 1 && worker.handles.size() == 1);
        CHECK(worker.ports.size() == 1 && worker.ports[0] == pa);
        CHECK(r.hold(pb) == Held::NewlyBound && r.serve(IceMux::kBlock, pb));
        settle();
        CHECK(r.holds(pa) && other.hold(pa) == Held::Failed && told == 0);
        // The worker's next agent registers on it still, and hears it
        uint16_t feedPortR = 0;
        const SOCKET feedR = udpSocket(&feedPortR, r.hopPort());
        const uint32_t agent = r.addAgent("hostRRRR1234", worker.sockets);
        CHECK(agent != 0);
        r.setFeedPort(agent, feedPortR);
        const std::vector<uint8_t> forR = bindingRequest("hostRRRR1234:peer");
        sendTo(peer, pa, forR.data(), forR.size());
        n = readWithin(feedR, got, sizeof(got), 2000);
        CHECK(n == static_cast<int>(mw::icemux::kFeedHeaderSize + forR.size()) && got[1] == 0);
        // The lease given back while an agent is registered there: it stays
        r.release(worker.lease);
        settle();
        CHECK(r.holds(pa) && told == 0);
        // The agent gone too: nobody has the port, and it goes
        r.removeAgent(agent);
        CHECK(letsGo(r, pa));
        CHECK(told == 1);
        CHECK(r.port(IceMux::kBlock) == pb && r.holds(pb));
        // It is anybody's again, and no agent is given it any more
        CHECK(other.hold(pa) == Held::NewlyBound);
        other.drop(pa);
        CHECK(r.addAgent("hostSSSS1234", worker.sockets) == 0);
        // A lease given back twice, and one that never was: nothing
        r.release(worker.lease);
        r.release(0);
        r.release(0xFFFFFFF0u);
        settle();
        CHECK(r.holds(pb) && told == 1);

        // A port replaced with nobody on it goes at once
        CHECK(r.hold(pc) == Held::NewlyBound && r.serve(IceMux::kBlock, pc));
        CHECK(letsGo(r, pb));
        CHECK(told == 2);
        // While a port is being asked for, none is let go: the answer may be
        // that the replaced one serves again
        const IceMux::Share held = r.shareByHandle();
        CHECK(r.hold(pa) == Held::NewlyBound && r.serve(IceMux::kBlock, pa));
        r.beginAsking();
        r.release(held.lease);
        settle();
        CHECK(r.holds(pc) && told == 2);
        CHECK(r.hold(pc) == Held::AlreadyHeld);
        CHECK(r.serve(IceMux::kBlock, pc));
        r.endAsking();
        // The asking over, the port that serves stays and the other goes
        CHECK(letsGo(r, pa));
        CHECK(told == 3 && r.holds(pc) && r.port(IceMux::kBlock) == pc);
        // Two workers on a port: it stays until both are gone
        const IceMux::Share left = r.shareByHandle(), right = r.shareByHandle();
        CHECK(left.lease != 0 && right.lease != 0 && left.lease != right.lease);
        CHECK(r.hold(pb) == Held::NewlyBound && r.serve(IceMux::kBlock, pb));
        r.release(left.lease);
        settle();
        CHECK(r.holds(pc) && told == 3);
        r.release(right.lease);
        CHECK(letsGo(r, pc));
        CHECK(told == 4);
        // The number a port left is another port's from then on, and that
        // one is read like any other
        const uint32_t late = r.addAgent("hostTTTT1234", r.serving());
        CHECK(late != 0);
        r.setFeedPort(late, feedPortR);
        const std::vector<uint8_t> forT = bindingRequest("hostTTTT1234:peer");
        sendTo(peer, pb, forT.data(), forT.size());
        n = readWithin(feedR, got, sizeof(got), 2000);
        CHECK(n == static_cast<int>(mw::icemux::kFeedHeaderSize + forT.size()) && got[1] == 0);
        r.removeAgent(late);

        // A lease of before a stop() means nothing after it
        const IceMux::Share stale = r.shareByHandle();
        CHECK(stale.lease != 0);
        r.stop();
        r.release(stale.lease);
        CHECK(r.start(&why));
        CHECK(r.hold(pa) == Held::NewlyBound && r.serve(IceMux::kBlock, pa));
        const IceMux::Share fresh = r.shareByHandle();
        CHECK(fresh.lease != 0 && fresh.lease != stale.lease);
        CHECK(r.hold(pb) == Held::NewlyBound && r.serve(IceMux::kBlock, pb));
        r.release(stale.lease);
        settle();
        CHECK(r.holds(pa));
        r.release(fresh.lease);
        CHECK(letsGo(r, pa));
        r.stop();
        closesocket(feedR);
    }

    SECTION("IceMux: so many sockets, and no more");
    const std::vector<uint16_t> many = freeBlockPorts(IceMux::kSocketsMax + 1);
    if (many.size() != IceMux::kSocketsMax + 1) {
        // Streams of this machine hold most of the block: not this run
        std::fprintf(stderr, "  (only %zu free ports in the stream block: skipped)\n", many.size());
    } else {
        IceMux full;
        std::atomic<int> said{0};
        full.setReporter([&said](const std::string&) { ++said; });
        CHECK(full.start(&why));
        for (size_t i = 0; i < IceMux::kSocketsMax; ++i)
            CHECK(full.hold(many[i]) == Held::NewlyBound);
        CHECK(said == 0);
        CHECK(full.hold(many[IceMux::kSocketsMax], &why) == Held::Failed);
        CHECK(!full.holds(many[IceMux::kSocketsMax]));
        // Said, and said once however often it is asked
        CHECK(said == 1);
        CHECK(full.hold(many[IceMux::kSocketsMax]) == Held::Failed);
        CHECK(said == 1);
        // A port it has is not one too many
        CHECK(full.hold(many[0]) == Held::AlreadyHeld);
        // The port was not even bound: it is free for anybody
        CHECK(other.hold(many[IceMux::kSocketsMax]) == Held::NewlyBound);
        other.drop(many[IceMux::kSocketsMax]);
        // One let go makes room for one
        full.drop(many[0]);
        CHECK(full.hold(many[IceMux::kSocketsMax]) == Held::NewlyBound);
        CHECK(said == 1);
        full.stop();

        // The replaced ports that streams are still on count too, and one
        // that is let go makes room for a port again
        IceMux turn;
        std::atomic<int> fullSaid{0};
        turn.setReporter([&fullSaid](const std::string& what) {
            if (what.find("every socket") != std::string::npos) ++fullSaid;
        });
        CHECK(turn.start(&why));
        std::vector<uint32_t> streams;
        for (size_t i = 0; i < IceMux::kSocketsMax; ++i) {
            CHECK(turn.hold(many[i]) == Held::NewlyBound && turn.serve(IceMux::kBlock, many[i]));
            streams.push_back(turn.shareByHandle().lease);
        }
        settle();
        for (size_t i = 0; i < IceMux::kSocketsMax; ++i)
            CHECK(turn.holds(many[i]));
        CHECK(turn.hold(many[IceMux::kSocketsMax]) == Held::Failed && fullSaid == 1);
        turn.release(streams[0]);
        CHECK(letsGo(turn, many[0]));
        CHECK(turn.hold(many[IceMux::kSocketsMax]) == Held::NewlyBound);
        // Full once more, and said once more
        CHECK(turn.hold(many[0]) == Held::Failed && fullSaid == 2);
        turn.stop();
    }
    other.stop();
    closesocket(peer);
}
