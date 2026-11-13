/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Which ports the streams share, and which the router forwards
 * (network/IceMuxPorts), with a real mux and a router played by hand: what is
 * bound when, what a claim's answer does to the mux, and what becomes of a
 * port whose hole goes to a neighbour. The router's own rules are
 * test_router_port_core.cpp's.
 */
#include "test_framework.h"
#include "network/IceMuxPorts.h"
#include "network/RouterPortPools.h"

#include <QtGlobal>

#include <chrono>
#include <functional>
#include <set>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#endif

namespace {

using Gateway = IceMuxPorts::Router::Gateway;
using Hold = IceMuxPorts::Hold;
constexpr int kStandard = IceMux::kStandard;
constexpr int kBlock = IceMux::kBlock;

/// The router's allocator, played by hand. It answers a claim when the test
/// says so, the way RouterPortCore does: the port the mux has first, then the
/// walk; a number a neighbour holds is passed over before anything is bound;
/// one the router refuses after it was bound for the claim is handed back.
struct ScriptedRouter : IceMuxPorts::Router
{
    bool on = true;
    Gateway state = Gateway::Looking;
    int looks = 0, claims = 0, releases = 0;
    uint16_t holes[IceMux::kRoles] = {0, 0};
    bool tcpHoles[IceMux::kRoles] = {true, true};
    QString ip = QStringLiteral("203.0.113.7");
    std::set<uint16_t> neighbours;
    std::set<uint16_t> refused;
    QList<uint16_t> walk[IceMux::kRoles];

    struct Asked
    {
        int which;
        uint16_t current;
        std::function<Hold(uint16_t)> hold;
        std::function<void(uint16_t)> drop;
        std::function<void(uint16_t, const QString&)> done;
    };
    std::vector<Asked> asked;

    bool enabled() const override { return on; }
    void look() override { ++looks; }
    Gateway gateway() const override { return state; }
    uint16_t hole(int which) const override { return holes[which]; }
    bool holeTcp(int which) const override { return holes[which] != 0 && tcpHoles[which]; }
    QString publicIp() const override { return ip; }
    void claim(int which, uint16_t current, std::function<Hold(uint16_t)> hold,
               std::function<void(uint16_t)> drop,
               std::function<void(uint16_t, const QString&)> done) override
    {
        ++claims;
        asked.push_back({which, current, std::move(hold), std::move(drop), std::move(done)});
    }
    void release(int which) override
    {
        ++releases;
        holes[which] = 0;
    }

    Asked next()
    {
        Asked a = std::move(asked.front());
        asked.erase(asked.begin());
        return a;
    }
    /// The router answers the oldest claim.
    void answer()
    {
        const Asked a = next();
        QList<uint16_t> numbers;
        if (a.current != 0) numbers.append(a.current);
        for (const uint16_t port : walk[a.which])
            if (!numbers.contains(port)) numbers.append(port);
        for (const uint16_t port : numbers) {
            if (neighbours.count(port)) continue;
            const Hold held = a.hold(port);
            if (held == Hold::Failed) continue;
            if (refused.count(port)) {
                if (held == Hold::NewlyBound) a.drop(port);
                continue;
            }
            holes[a.which] = port;
            a.done(port, {});
            return;
        }
        a.done(0, QStringLiteral("no router port left"));
    }
    /// The oldest claim comes back with nothing, for this reason.
    void fail(const QString& why) { next().done(0, why); }
};

/// A mux, a router and the ports between them. The numbers a port may take
/// are the same for the router's walk and for a bind without it.
struct Rig
{
    IceMux mux;
    ScriptedRouter router;
    bool wantsStandard = true;
    IceMuxPorts ports;

    Rig(const QList<uint16_t>& standard, const QList<uint16_t>& block)
        : ports(mux, router,
                IceMuxPorts::Policy{[this](int which) { return router.walk[which]; },
                                    [this]() { return wantsStandard; }})
    {
        router.walk[kStandard] = standard;
        router.walk[kBlock] = block;
    }
};

/// `count` ports of the stream block nobody has bound right now, from its far
/// end.
std::vector<uint16_t> freePorts(size_t count)
{
    std::vector<uint16_t> ports;
    IceMux probe;
    if (!probe.start(nullptr)) return ports;
    using namespace mw::routerports;
    for (int p = kMediaBasePort + kMediaPortCount - 1; p >= kMediaBasePort && ports.size() < count;
         --p) {
        const auto port = static_cast<uint16_t>(p);
        if (probe.hold(port) != IceMux::Held::NewlyBound) continue;
        probe.drop(port);
        ports.push_back(port);
    }
    return ports;
}

void quiet(QtMsgType, const QMessageLogContext&, const QString&) {}

/// Lets the mux's reading thread take a turn: it looks at its sockets again
/// every tenth of a second at the latest.
void settle()
{
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
}

/// Whether the mux lets go of `port` in good time: its reading thread does
/// it, at its next look at its sockets.
bool letsGo(const IceMux& mux, uint16_t port)
{
    for (int waited = 0; waited < 3000 && mux.holds(port); waited += 20)
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    return !mux.holds(port);
}

} // namespace

void run_ice_mux_ports_tests()
{
    SECTION("IceMuxPorts: without a router to ask, the ports are bound all the same");
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    const std::vector<uint16_t> spare = freePorts(4);
    CHECK(spare.size() == 4);
    if (spare.size() < 4) return;
    const uint16_t s1 = spare[0], s2 = spare[1], b1 = spare[2], b2 = spare[3];
    const QList<uint16_t> standard{s1, s2}, block{b1, b2};
    // What the ports log is not this suite's to print
    const QtMessageHandler before = qInstallMessageHandler(quiet);
    QString why;
    {
        Rig r(standard, block);
        r.router.on = false;
        CHECK(!r.ports.inUse());
        CHECK(r.ports.start(&why));
        CHECK(r.ports.inUse() && r.mux.running());
        CHECK(r.mux.port(kBlock) == b1 && r.mux.port(kStandard) == s1);
        // The conferencing port first: the order an agent is given them in
        CHECK(r.mux.serving().size() == 2);
        CHECK(r.router.claims == 0 && r.router.looks == 0);
        // Started once
        CHECK(r.ports.start(&why));
        CHECK(r.mux.port(kBlock) == b1 && !r.mux.holds(b2));
    }
    {
        // A port another program has is passed over
        IceMux busy;
        CHECK(busy.start(nullptr));
        CHECK(busy.hold(b1) == IceMux::Held::NewlyBound);
        Rig r(standard, block);
        r.router.on = false;
        CHECK(r.ports.start(&why));
        CHECK(r.mux.port(kBlock) == b2);
    }

    SECTION("IceMuxPorts: no port of the block to bind, no mux");
    {
        IceMux busy;
        CHECK(busy.start(nullptr));
        CHECK(busy.hold(b1) == IceMux::Held::NewlyBound);
        CHECK(busy.hold(b2) == IceMux::Held::NewlyBound);
        Rig r(standard, block);
        r.router.state = Gateway::Ready;
        why.clear();
        CHECK(!r.ports.start(&why));
        CHECK(!why.isEmpty());
        CHECK(!r.ports.inUse() && !r.mux.running());
        CHECK(!r.mux.holds(s1) && r.router.claims == 0);
        // And nothing the router says later starts anything
        r.ports.routerChanged();
        r.ports.holeLost(kBlock, b1);
        r.ports.switchedOff();
        CHECK(r.router.claims == 0 && r.router.releases == 0 && !r.mux.running());
    }

    SECTION("IceMuxPorts: a host that is not open to the Internet has no conferencing port");
    {
        Rig r(standard, block);
        r.wantsStandard = false;
        r.router.on = false;
        CHECK(r.ports.start(&why));
        CHECK(r.mux.port(kBlock) == b1 && r.mux.port(kStandard) == 0 && !r.mux.holds(s1));
        // Nor when the router can be asked: the block port's hole alone
        r.router.on = true;
        r.router.state = Gateway::Ready;
        r.ports.routerChanged();
        CHECK(r.router.claims == 1 && r.router.asked.front().which == kBlock);
        r.router.answer();
        CHECK(r.router.holes[kBlock] == b1 && r.mux.port(kStandard) == 0);
    }

    SECTION("IceMuxPorts: the block port at once, the conferencing port when the router answers");
    {
        Rig r(standard, block);
        CHECK(r.ports.start(&why));
        // The router is being looked for: nothing is asked of it yet, and no
        // conferencing number is bound before it has had its say
        CHECK(r.router.looks >= 1 && r.router.claims == 0);
        CHECK(r.mux.port(kBlock) == b1 && r.mux.port(kStandard) == 0 && !r.mux.holds(s1));
        r.router.state = Gateway::Ready;
        r.ports.routerChanged();
        CHECK(r.router.claims == 2);
        // One claim at a time for each port
        r.ports.routerChanged();
        CHECK(r.router.claims == 2);
        // The conferencing port: bound for the claim, and it serves
        CHECK(r.router.asked.front().which == kStandard && r.router.asked.front().current == 0);
        r.router.answer();
        CHECK(r.router.holes[kStandard] == s1 && r.mux.port(kStandard) == s1);
        // The block port: the hole lands on the port the streams already have
        CHECK(r.router.asked.front().which == kBlock && r.router.asked.front().current == b1);
        r.router.answer();
        CHECK(r.router.holes[kBlock] == b1 && r.mux.port(kBlock) == b1 && !r.mux.holds(b2));
        CHECK(r.mux.serving().size() == 2);
        // Held: nothing more is asked
        r.ports.routerChanged();
        CHECK(r.router.claims == 2 && r.router.releases == 0);

        SECTION(
            "IceMuxPorts: a hole the router gave to a neighbour is asked for on the next number");
        // A stream runs: its worker was given the two ports
        const IceMux::Share stream = r.mux.shareByHandle();
        CHECK(stream.lease != 0 && stream.sockets.size() == 2);
        // (the allocator has dropped the hole when it says so)
        r.router.holes[kStandard] = 0;
        r.router.neighbours.insert(s1);
        r.ports.holeLost(kStandard, s1);
        CHECK(r.router.claims == 3 && r.router.asked.front().current == s1);
        r.router.answer();
        CHECK(r.router.holes[kStandard] == s2 && r.mux.port(kStandard) == s2);
        // The port that lost its hole stays bound: that stream is on it
        settle();
        CHECK(r.mux.holds(s1));
        // A port that is not one of the two is nobody's
        r.ports.holeLost(5, s1);
        r.ports.holeLost(-1, s1);
        CHECK(r.router.claims == 3);
        // The stream over, its worker gone: the mux lets the port go
        r.mux.release(stream.lease);
        CHECK(letsGo(r.mux, s1));
        CHECK(r.mux.port(kStandard) == s2 && r.mux.port(kBlock) == b1);

        SECTION("IceMuxPorts: switched off, the holes go and the ports stay");
        r.router.on = false;
        r.ports.switchedOff();
        CHECK(r.router.releases == 2 && r.router.holes[kStandard] == 0 &&
              r.router.holes[kBlock] == 0);
        CHECK(r.mux.port(kStandard) == s2 && r.mux.port(kBlock) == b1);
        CHECK(r.router.claims == 3);
        // Switched on again, they are asked for where the streams are
        r.router.on = true;
        r.ports.routerChanged();
        CHECK(r.router.claims == 5);
        r.router.answer();
        r.router.answer();
        CHECK(r.router.holes[kStandard] == s2 && r.router.holes[kBlock] == b1);
        CHECK(r.mux.port(kStandard) == s2 && r.mux.port(kBlock) == b1);
    }

    SECTION("IceMuxPorts: only a port the router forwards is announced at the public address");
    {
        Rig r(standard, block);
        // Not started: nothing
        CHECK(r.ports.announced().ports.isEmpty());
        r.router.state = Gateway::Ready;
        CHECK(r.ports.start(&why));
        // Bound, its hole asked for and not made yet: nothing to announce
        CHECK(r.mux.port(kBlock) == b1 && r.ports.announced().ports.isEmpty());
        r.router.answer();
        // The conferencing port has its hole, the block port not yet
        CHECK(r.ports.announced().ports == QList<uint16_t>{s1});
        CHECK(r.ports.announced().publicIp == r.router.ip);
        r.router.answer();
        // Both, the conferencing port first
        CHECK(r.ports.announced().ports == (QList<uint16_t>{s1, b1}));
        // And in TCP those the router forwards in TCP: a port whose TCP side
        // is a neighbour's is announced for UDP alone
        CHECK(r.ports.announced().tcpPorts == (QList<uint16_t>{s1, b1}));
        r.router.tcpHoles[kStandard] = false;
        CHECK(r.ports.announced().tcpPorts == QList<uint16_t>{b1});
        CHECK(r.ports.announced().ports == (QList<uint16_t>{s1, b1}));
        r.router.tcpHoles[kStandard] = true;
        // A router that does not say its public address: nothing to announce
        r.router.ip.clear();
        CHECK(r.ports.announced().ports.isEmpty());
        r.router.ip = QStringLiteral("203.0.113.7");
        // A hole the router gave to another machine: its port is announced no
        // more, though it still serves this LAN
        r.router.holes[kStandard] = 0;
        r.router.neighbours.insert(s1);
        r.ports.holeLost(kStandard, s1);
        CHECK(r.mux.port(kStandard) == s1);
        CHECK(r.ports.announced().ports == QList<uint16_t>{b1});
        // The next number has the hole and serves: announced again
        r.router.answer();
        CHECK(r.ports.announced().ports == (QList<uint16_t>{s2, b1}));
        // A hole on another number than the one that serves is not announced
        r.router.holes[kBlock] = b2;
        CHECK(r.ports.announced().ports == QList<uint16_t>{s2});
        r.router.holes[kBlock] = b1;
        // Switched off: the holes are gone and nothing is announced; the
        // ports stay
        r.router.on = false;
        r.ports.switchedOff();
        CHECK(r.ports.announced().ports.isEmpty());
        CHECK(r.mux.port(kStandard) == s2 && r.mux.port(kBlock) == b1);
    }

    SECTION("IceMuxPorts: a neighbour on the block port, and the mux moves up the block");
    {
        Rig r(standard, block);
        r.router.state = Gateway::Ready;
        r.router.neighbours.insert(b1);
        CHECK(r.ports.start(&why));
        CHECK(r.mux.port(kBlock) == b1 && r.router.claims == 2);
        r.router.answer(); // the conferencing port
        r.router.answer(); // the block port
        CHECK(r.router.holes[kBlock] == b2 && r.mux.port(kBlock) == b2);
        // The port bound at the start had no stream on it: the mux lets it go
        CHECK(letsGo(r.mux, b1));
        CHECK(r.mux.port(kBlock) == b2 && r.mux.port(kStandard) == s1);
    }

    SECTION("IceMuxPorts: no replaced port is let go while the router is asked for one");
    {
        Rig r(standard, block);
        r.router.state = Gateway::Ready;
        CHECK(r.ports.start(&why));
        r.router.answer();
        r.router.answer();
        CHECK(r.mux.port(kStandard) == s1 && r.mux.port(kBlock) == b1);
        // A stream runs on the two ports, and the conferencing port moves to
        // the next number: the first stays, for that stream
        const IceMux::Share stream = r.mux.shareByHandle();
        CHECK(stream.lease != 0);
        r.router.holes[kStandard] = 0;
        r.router.neighbours.insert(s1);
        r.ports.holeLost(kStandard, s1);
        r.router.answer();
        CHECK(r.mux.port(kStandard) == s2 && r.mux.holds(s1));
        // That hole is lost in its turn, and the first number is free on the
        // router again: the router is asked, and has not answered
        r.router.holes[kStandard] = 0;
        r.router.neighbours.clear();
        r.router.neighbours.insert(s2);
        r.ports.holeLost(kStandard, s2);
        CHECK(r.router.asked.size() == 1);
        // The stream ends meanwhile: nothing goes while the router is asked
        r.mux.release(stream.lease);
        settle();
        CHECK(r.mux.holds(s1));
        // Its answer is the port the mux still had: that one serves again,
        // and the one it replaces, with nobody on it, goes
        r.router.answer();
        CHECK(r.router.holes[kStandard] == s1 && r.mux.port(kStandard) == s1);
        CHECK(letsGo(r.mux, s2));
        CHECK(r.mux.holds(s1) && r.mux.holds(b1));
    }

    SECTION("IceMuxPorts: every conferencing port taken, and the streams keep to the block port");
    {
        Rig r(standard, block);
        r.router.state = Gateway::Ready;
        r.router.neighbours.insert(s1);
        r.router.refused.insert(s2);
        CHECK(r.ports.start(&why));
        r.router.answer();
        // Nothing bound is left behind: one was a neighbour's and never
        // bound, the other was bound, refused and let go
        CHECK(r.mux.port(kStandard) == 0 && !r.mux.holds(s1) && !r.mux.holds(s2));
        CHECK(r.router.holes[kStandard] == 0);
        // And the router is not asked again until it changes
        CHECK(r.router.claims == 2);
        r.router.answer();
        CHECK(r.mux.port(kBlock) == b1 && r.router.claims == 2);
        CHECK(r.mux.serving().size() == 1);
    }

    SECTION("IceMuxPorts: no gateway, then one");
    {
        Rig r(standard, block);
        r.router.state = Gateway::Missing;
        CHECK(r.ports.start(&why));
        // Bound without it
        CHECK(r.mux.port(kStandard) == s1 && r.mux.port(kBlock) == b1 && r.router.claims == 0);
        // Found later: the holes are asked for on the ports that serve
        r.router.state = Gateway::Ready;
        r.ports.routerChanged();
        CHECK(r.router.claims == 2 && r.router.asked.front().current == s1);
        r.router.answer();
        r.router.answer();
        CHECK(r.router.holes[kStandard] == s1 && r.router.holes[kBlock] == b1);
        CHECK(!r.mux.holds(s2) && !r.mux.holds(b2));
    }
    {
        // The gateway goes while it is asked: the port is bound without it
        Rig r(standard, block);
        r.router.state = Gateway::Ready;
        CHECK(r.ports.start(&why));
        CHECK(r.router.claims == 2 && r.mux.port(kStandard) == 0);
        r.router.state = Gateway::Missing;
        r.router.fail(QStringLiteral("no IGD"));
        CHECK(r.mux.port(kStandard) == s1);
        r.router.fail(QStringLiteral("no IGD"));
        CHECK(r.mux.port(kBlock) == b1 && r.router.claims == 2);
    }

    SECTION("IceMuxPorts: switched off while the router answers");
    {
        Rig r(standard, block);
        r.router.state = Gateway::Ready;
        r.router.neighbours.insert(b1);
        CHECK(r.ports.start(&why));
        CHECK(r.router.claims == 2);
        r.router.on = false;
        // The conferencing port had none: the one bound for the claim
        // serves, without its hole
        r.router.answer();
        CHECK(r.router.releases == 1 && r.router.holes[kStandard] == 0);
        CHECK(r.mux.port(kStandard) == s1);
        // The block port had one: it keeps it, and the number bound for the
        // claim is let go
        r.router.answer();
        CHECK(r.router.releases == 2 && r.router.holes[kBlock] == 0);
        CHECK(r.mux.port(kBlock) == b1 && !r.mux.holds(b2));
    }
    {
        // Internet Access itself went off meanwhile: the host wants no
        // conferencing port any more, and the number bound for the claim is
        // let go rather than served
        Rig r(standard, block);
        r.router.state = Gateway::Ready;
        CHECK(r.ports.start(&why));
        CHECK(r.router.claims == 2);
        r.router.on = false;
        r.wantsStandard = false;
        r.router.answer();
        CHECK(r.router.releases == 1 && r.router.holes[kStandard] == 0);
        CHECK(r.mux.port(kStandard) == 0 && !r.mux.holds(s1));
        // The block port is wanted whatever the switches say
        r.router.answer();
        CHECK(r.router.releases == 2 && r.mux.port(kBlock) == b1);
        CHECK(r.mux.serving().size() == 1);
        // And nothing more is asked or bound
        r.ports.switchedOff();
        r.ports.routerChanged();
        CHECK(r.router.claims == 2 && r.mux.port(kStandard) == 0 && !r.mux.holds(s1));
    }
    qInstallMessageHandler(before);
}
