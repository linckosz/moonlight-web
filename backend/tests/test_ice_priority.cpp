/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The priority of the UPnP copy of a host candidate (IcePriority.h).
 *
 * What a same-LAN session relies on: the copy at the public address ranks below
 * the LAN host candidate it copies, the copies of several interfaces keep their
 * order, and only the priority field of the line changes.
 */

#include "../src/streaming/HostCandidates.h"
#include "../src/streaming/IcePriority.h"

#include "test_framework.h"

#include <string>
#include <vector>

namespace {

void testSrflxPriorityRanksBelowHost()
{
    // The two candidates of the hairpinned session, 09/10/2026: the LAN one
    // and its copy went out at the same 2114976511.
    const uint32_t host = 2114976511u;
    CHECK_EQ(host >> 24, mw::ice::kHostTypePreference);

    const uint32_t copy = mw::ice::srflxPriority(host);
    CHECK(copy < host);
    CHECK_EQ(copy >> 24, mw::ice::kSrflxTypePreference);
    CHECK_EQ(copy & 0x00FFFFFFu, host & 0x00FFFFFFu);

    // Below every host candidate, whatever its local preference.
    CHECK(copy < (mw::ice::kHostTypePreference << 24));

    // Two interfaces' copies keep the order of their host candidates.
    CHECK(mw::ice::srflxPriority(2114977791u) > mw::ice::srflxPriority(2114976511u));
}

void testWithPriorityRewritesOnlyThePriority()
{
    const std::string line = "candidate:6 1 UDP 2114976511 203.0.113.10 46102 typ host";
    CHECK_EQ(mw::ice::withPriority(line, 1678768895u),
             std::string("candidate:6 1 UDP 1678768895 203.0.113.10 46102 typ host"));

    // A tail (TCP type, generation) is kept as it is.
    const std::string tcp =
        "candidate:2 1 TCP 2105524479 203.0.113.10 46102 typ host tcptype passive";
    CHECK_EQ(mw::ice::withPriority(tcp, 7u),
             std::string("candidate:2 1 TCP 7 203.0.113.10 46102 typ host tcptype passive"));
}

void testWithPriorityLeavesAMalformedLineAlone()
{
    CHECK_EQ(mw::ice::withPriority("", 1u), std::string());
    CHECK_EQ(mw::ice::withPriority("candidate:6 1 UDP", 1u), std::string("candidate:6 1 UDP"));
    const std::string notANumber = "candidate:6 1 UDP high 203.0.113.10 46102 typ host";
    CHECK_EQ(mw::ice::withPriority(notANumber, 1u), notANumber);
}

void testPublicPortOfAHostCandidate()
{
    using mw::ice::publicPortFor;
    const std::vector<uint16_t> none, one{3478}, both{3478, 48550};

    // A stream with a socket of its own: its one hole, whatever port the
    // socket is on, and whatever the shared ports are.
    CHECK_EQ(publicPortFor(48562, false, 46100, none), uint16_t(46100));
    CHECK_EQ(publicPortFor(48573, false, 46100, both), uint16_t(46100));
    CHECK_EQ(publicPortFor(48562, false, 0, both), uint16_t(0));

    // On the shared ports: each port the router forwards, on its own number.
    CHECK_EQ(publicPortFor(3478, true, 0, both), uint16_t(3478));
    CHECK_EQ(publicPortFor(48550, true, 0, both), uint16_t(48550));
    // A shared port the router does not forward has no copy at the public
    // address, and a hole of the stream's own plays no part there.
    CHECK_EQ(publicPortFor(48550, true, 46100, one), uint16_t(0));
    CHECK_EQ(publicPortFor(48550, true, 46100, none), uint16_t(0));
    // Nor has a candidate whose port is not known.
    CHECK_EQ(publicPortFor(0, true, 46100, both), uint16_t(0));
}

void testTheCopyOnTcp443()
{
    using mw::ice::has443Copy;
    // The passive TCP candidate on the very port the router's 443 leads to,
    // and no other: not another shared port, not an active candidate (never
    // dialled), not a UDP one, and none when 443 leads nowhere.
    CHECK(has443Copy(true, 48550, 48550));
    CHECK(!has443Copy(true, 3478, 48550));
    CHECK(!has443Copy(false, 48550, 48550));
    CHECK(!has443Copy(true, 48550, 0));
    CHECK(!has443Copy(true, 0, 0));
    CHECK_EQ(mw::ice::kTcp443, uint16_t(443));

    // Below the copy on the port's own number, and below every other
    // candidate of the connection, whatever the interface
    const uint32_t passive = 2105524479u;
    const uint32_t last = mw::ice::lastResortPriority(passive);
    CHECK(last < mw::ice::srflxPriority(passive));
    CHECK(last < mw::ice::srflxPriority(0u));
    CHECK_EQ(last >> 24, mw::ice::kLastResortTypePreference);
    CHECK_EQ(last & 0x00FFFFFFu, passive & 0x00FFFFFFu);
    CHECK(last > 0x00FFFFFFu);

    // As it goes out: the same line, on 443, with that priority
    const std::string own =
        "candidate:2 1 TCP 2105524479 203.0.113.10 443 typ host tcptype passive";
    CHECK_EQ(mw::ice::withPriority(own, last),
             std::string("candidate:2 1 TCP 847233279 203.0.113.10 443 typ host tcptype passive"));
}

// ── What the browser is told of a host candidate (HostCandidates.h) ─────────

using mw::ice::CandidateVerdict;
using mw::ice::CandidateView;
using mw::ice::LocalCandidate;
using Transport = LocalCandidate::Transport;
using Why = CandidateVerdict::Why;
using Kind = NetClassify::Kind;

LocalCandidate cand(Kind kind, bool ipv4, Transport transport, uint16_t port, bool host = true)
{
    LocalCandidate c;
    c.host = host;
    c.ipv4 = ipv4;
    c.kind = kind;
    c.transport = transport;
    c.port = port;
    return c;
}

// The server's shared port 48597, forwarded by the router in UDP and in TCP
CandidateView sharedHole(bool peerOfOurs)
{
    CandidateView v;
    v.forceHostPublic = true;
    v.havePublicIp = true;
    v.onSharedPorts = true;
    v.udpHoles = {48597};
    v.tcpHoles = {48597};
    v.suppressIpv6 = true;
    v.emitLan = peerOfOurs;
    return v;
}

// A host with seven addresses, as the bench's: the LAN one, a virtual switch's,
// WSL's, a virtual machine network's (all private IPv4), a mesh VPN's IPv4
// (100.64/10) and IPv6 (a ULA), and a global IPv6. Each in UDP and in passive
// TCP: the same verdict for both transports.
void testEveryTransportIsJudgedAlike()
{
    for (const Transport t : {Transport::Udp, Transport::TcpPassive}) {
        for (const bool ours : {true, false}) {
            const CandidateView v = sharedHole(ours);
            // The private IPv4 ones: a copy at the public address; themselves
            // to a peer of our network only
            const auto lan = mw::ice::hostCandidateVerdict(cand(Kind::Private, true, t, 48597), v);
            CHECK_EQ(lan.asIs, ours);
            CHECK_EQ(lan.publicPort, uint16_t(48597));
            CHECK(lan.why == Why::Copied);
            // The mesh VPN's IPv4: never rewritten, to a peer of our network only
            const auto vpn = mw::ice::hostCandidateVerdict(cand(Kind::Tunnel, true, t, 48597), v);
            CHECK_EQ(vpn.asIs, ours);
            CHECK_EQ(vpn.publicPort, uint16_t(0));
            CHECK(vpn.why == (ours ? Why::TunnelForLan : Why::TunnelWithheld));
            // Its ULA: withheld from a public peer
            const auto ula = mw::ice::hostCandidateVerdict(cand(Kind::Private, false, t, 48597), v);
            CHECK_EQ(ula.asIs, ours);
            CHECK_EQ(ula.publicPort, uint16_t(0));
            // A global IPv6: to everyone, as it is
            const auto v6 = mw::ice::hostCandidateVerdict(cand(Kind::Public, false, t, 48597), v);
            CHECK(v6.asIs);
            CHECK_EQ(v6.publicPort, uint16_t(0));
        }
    }
}

// The copies of the host's addresses are one copy: one per transport and port
void testOneCopyPerTransportAndPort()
{
    using mw::ice::publicCopyKey;
    CHECK_EQ(publicCopyKey(Transport::Udp, 48597), publicCopyKey(Transport::Udp, 48597));
    CHECK(publicCopyKey(Transport::Udp, 48597) != publicCopyKey(Transport::TcpPassive, 48597));
    CHECK(publicCopyKey(Transport::Udp, 48597) != publicCopyKey(Transport::Udp, 48596));
    CHECK(publicCopyKey(Transport::TcpPassive, 48597) !=
          publicCopyKey(Transport::TcpPassive, mw::ice::kTcp443));

    // Whichever address comes first (a virtual switch's, here), its copy is
    // the right one: only the port is taken from the candidate, the router
    // leads that port to the address the host gave it
    const CandidateView v = sharedHole(false);
    const auto first =
        mw::ice::hostCandidateVerdict(cand(Kind::Private, true, Transport::TcpPassive, 48597), v);
    CHECK_EQ(first.publicPort, uint16_t(48597));
}

// An active TCP candidate (port 9, never dialled) has no copy at the public
// address, and goes, like any internal address, to a peer of our network only
void testTheActiveCandidate()
{
    for (const bool ours : {true, false}) {
        const CandidateView v = sharedHole(ours);
        const auto lan =
            mw::ice::hostCandidateVerdict(cand(Kind::Private, true, Transport::TcpActive, 9), v);
        CHECK_EQ(lan.asIs, ours);
        CHECK_EQ(lan.publicPort, uint16_t(0));
        CHECK(lan.why == Why::ActiveForLan);
        const auto vpn =
            mw::ice::hostCandidateVerdict(cand(Kind::Tunnel, true, Transport::TcpActive, 9), v);
        CHECK_EQ(vpn.asIs, ours);
        CHECK_EQ(vpn.publicPort, uint16_t(0));
    }
    // On a stream's own hole too: its port was copied onto the hole before
    CandidateView own;
    own.forceHostPublic = true;
    own.havePublicIp = true;
    own.ownHole = 46100;
    own.suppressIpv6 = true;
    const auto active =
        mw::ice::hostCandidateVerdict(cand(Kind::Private, true, Transport::TcpActive, 9), own);
    CHECK(!active.asIs);
    CHECK_EQ(active.publicPort, uint16_t(0));
}

void testOwnHoleAndPortsNotForwarded()
{
    // A stream's own hole: any port of its socket is copied onto the hole
    CandidateView own;
    own.forceHostPublic = true;
    own.havePublicIp = true;
    own.ownHole = 46100;
    for (const Transport t : {Transport::Udp, Transport::TcpPassive}) {
        const auto c = mw::ice::hostCandidateVerdict(cand(Kind::Private, true, t, 48550), own);
        CHECK(!c.asIs);
        CHECK_EQ(c.publicPort, uint16_t(46100));
    }
    // A shared port the router does not forward: to a peer of ours only
    const auto notForwarded = mw::ice::hostCandidateVerdict(
        cand(Kind::Private, true, Transport::Udp, 48596), sharedHole(false));
    CHECK(!notForwarded.asIs);
    CHECK_EQ(notForwarded.publicPort, uint16_t(0));
    CHECK(notForwarded.why == Why::NotForwarded);
    CHECK(mw::ice::hostCandidateVerdict(cand(Kind::Private, true, Transport::Udp, 48596),
                                        sharedHole(true))
              .asIs);
    // The router's TCP 443 leads to 48597: a second copy, of the passive TCP one only
    CandidateView by443 = sharedHole(false);
    by443.tcp443LeadsTo = 48597;
    CHECK(mw::ice::hostCandidateVerdict(cand(Kind::Private, true, Transport::TcpPassive, 48597),
                                        by443)
              .by443);
    CHECK(!mw::ice::hostCandidateVerdict(cand(Kind::Private, true, Transport::Udp, 48597), by443)
               .by443);
}

void testReflexiveCandidatesGoAsTheyAre()
{
    // Never judged: their address is the one the world already sees (a
    // carrier-grade NAT's 100.64/10 included)
    for (const bool ours : {true, false})
        for (const Kind k : {Kind::Public, Kind::Tunnel, Kind::Private}) {
            const auto r = mw::ice::hostCandidateVerdict(
                cand(k, true, Transport::Udp, 50000, /*host=*/false), sharedHole(ours));
            CHECK(r.asIs);
            CHECK_EQ(r.publicPort, uint16_t(0));
        }
}

// Without a hole, a host candidate goes out as it is, whoever the peer: no
// copy at the public address, nothing held back. What a peer outside our own
// network is told then is a question apart, not this rule's.
void testWithoutAHole()
{
    CandidateView none;
    for (const Transport t : {Transport::Udp, Transport::TcpPassive, Transport::TcpActive}) {
        const auto c = mw::ice::hostCandidateVerdict(cand(Kind::Private, true, t, 48597), none);
        CHECK(c.asIs);
        CHECK_EQ(c.publicPort, uint16_t(0));
    }
    CHECK(
        mw::ice::hostCandidateVerdict(cand(Kind::Public, true, Transport::Udp, 48597), none).asIs);
}

} // namespace

void run_ice_priority_tests()
{
    testSrflxPriorityRanksBelowHost();
    testWithPriorityRewritesOnlyThePriority();
    testWithPriorityLeavesAMalformedLineAlone();
    testPublicPortOfAHostCandidate();
    testTheCopyOnTcp443();
    testEveryTransportIsJudgedAlike();
    testOneCopyPerTransportAndPort();
    testTheActiveCandidate();
    testOwnHoleAndPortsNotForwarded();
    testReflexiveCandidatesGoAsTheyAre();
    testWithoutAHole();
}
