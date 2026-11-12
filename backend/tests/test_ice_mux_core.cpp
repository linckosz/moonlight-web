/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The ICE mux's decisions (network/IceMuxCore): what a datagram on a shared
 * UDP port is, and which agent it goes to. No socket here.
 */
#include "test_framework.h"
#include "network/IceMuxCore.h"

#include <cstring>
#include <string>
#include <vector>

using namespace mw::icemux;

namespace {

using Bytes = std::vector<uint8_t>;

void put16(Bytes& b, size_t at, uint16_t v)
{
    b[at] = static_cast<uint8_t>(v >> 8);
    b[at + 1] = static_cast<uint8_t>(v);
}

/// A STUN message of `type` with the attributes given as (type, value).
Bytes stun(uint16_t type, const std::vector<std::pair<uint16_t, std::string>>& attrs = {})
{
    Bytes b(20, 0);
    put16(b, 0, type);
    b[4] = 0x21;
    b[5] = 0x12;
    b[6] = 0xA4;
    b[7] = 0x42;
    for (size_t i = 8; i < 20; ++i)
        b[i] = static_cast<uint8_t>(i);
    for (const auto& [attr, value] : attrs) {
        const size_t at = b.size();
        b.resize(at + 4 + ((value.size() + 3) & ~size_t(3)), 0);
        put16(b, at, attr);
        put16(b, at + 2, static_cast<uint16_t>(value.size()));
        std::memcpy(b.data() + at + 4, value.data(), value.size());
    }
    put16(b, 2, static_cast<uint16_t>(b.size() - 20));
    return b;
}

Bytes request(const std::string& username)
{
    // PRIORITY first, as a browser sends it: USERNAME is not the first one
    return stun(0x0001, {{0x0024, std::string(4, '\1')}, {0x0006, username}});
}

Peer peer(uint8_t last, uint16_t port, uint8_t via = 0)
{
    Peer p;
    p.family = 4;
    p.address = {192, 0, 2, last};
    p.port = port;
    p.via = via;
    return p;
}

std::vector<uint32_t> to(Router& r, const Peer& from, const Bytes& b, uint64_t now)
{
    return r.route(from, b.data(), b.size(), now).feeds;
}

const Bytes kDtls = {22, 254, 253, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0};

void reading()
{
    SECTION("IceMuxCore: what a datagram is");
    const Bytes req = request("hostAAAA1234:peer");
    StunView v = readStun(req.data(), req.size());
    CHECK(v.kind == Kind::BindingRequest);
    CHECK(v.localUfrag == "hostAAAA1234");

    // No colon, no USERNAME at all: a request nobody can be found for
    const Bytes bare = request("hostAAAA1234");
    v = readStun(bare.data(), bare.size());
    CHECK(v.kind == Kind::BindingRequest && v.localUfrag.empty());
    const Bytes none = stun(0x0001);
    v = readStun(none.data(), none.size());
    CHECK(v.kind == Kind::BindingRequest && v.localUfrag.empty());

    CHECK(readStun(stun(0x0101).data(), 20).kind == Kind::StunResponse);
    CHECK(readStun(stun(0x0111).data(), 20).kind == Kind::StunResponse);
    CHECK(readStun(stun(0x0011).data(), 20).kind == Kind::StunOther); // indication
    CHECK(readStun(stun(0x0003).data(), 20).kind == Kind::StunOther); // Allocate
    CHECK(readStun(kDtls.data(), kDtls.size()).kind == Kind::Other);
    CHECK(readStun(nullptr, 0).kind == Kind::Other);

    // A wrong cookie, a length that is not the datagram's, a short header
    Bytes bad = req;
    bad[4] ^= 1;
    CHECK(readStun(bad.data(), bad.size()).kind == Kind::Other);
    CHECK(readStun(req.data(), req.size() - 4).kind == Kind::Other);
    CHECK(readStun(req.data(), 19).kind == Kind::Other);

    // An attribute that says it is longer than what is left: the walk stops
    Bytes lying = req;
    put16(lying, 20 + 2, 0xFFFC);
    v = readStun(lying.data(), lying.size());
    CHECK(v.kind == Kind::BindingRequest && v.localUfrag.empty());
    // Every cut of a good request, every byte flipped: nothing read outside
    // (the sanitizers of the WSL run are the judge)
    for (size_t n = 0; n <= req.size(); ++n) {
        const Bytes cut(req.begin(), req.begin() + n);
        (void)readStun(cut.data(), cut.size());
    }
    for (size_t i = 0; i < req.size(); ++i) {
        Bytes flipped = req;
        flipped[i] ^= 0xFF;
        (void)readStun(flipped.data(), flipped.size());
    }
}

void header()
{
    SECTION("IceMuxCore: the header a datagram travels behind");
    Peer p = peer(7, 0x1234, 1);
    uint8_t h[kFeedHeaderSize];
    std::memset(h, 0xEE, sizeof(h));
    writeFeedHeader(h, p, 0x0102030405060708ull);
    const uint8_t v4[kFeedHeaderSize] = {1,   1, 4, 0, 0x12, 0x34, 0, 0, 0, 0, 0, 0,
                                         192, 0, 2, 7, 0,    0,    0, 0, 0, 0, 0, 0,
                                         0,   0, 0, 0, 8,    7,    6, 5, 4, 3, 2, 1};
    CHECK(std::memcmp(h, v4, sizeof(h)) == 0);
    p.family = 6;
    p.scope = 0x0A0B0C0D;
    for (size_t i = 0; i < 16; ++i)
        p.address[i] = static_cast<uint8_t>(0xF0 + i);
    writeFeedHeader(h, p, 0);
    CHECK(h[2] == 6 && h[8] == 0x0D && h[11] == 0x0A && h[12] == 0xF0 && h[27] == 0xFF);
}

void routing()
{
    SECTION("IceMuxCore: which agent a datagram goes to");
    Router r;
    const Peer a = peer(1, 5000), b = peer(2, 6000);

    // Nobody streams: everything is dropped
    CHECK(to(r, a, request("hostAAAA1234:x"), 0).empty());

    CHECK(r.addAgent(1, "hostAAAA1234", 0));
    CHECK(r.addAgent(2, "hostBBBB5678", 0));
    CHECK(!r.addAgent(3, "hostAAAA1234", 0)); // the fragment is taken
    CHECK(!r.addAgent(1, "hostCCCC9012", 0)); // the feed is taken
    CHECK(!r.addAgent(3, "short", 0));
    CHECK(!r.addAgent(3, "0123456789abcdef", 0)); // 16: too long
    CHECK(!r.addAgent(3, "host:CCC9012", 0));     // not a character of a fragment
    CHECK(!r.addAgent(3,
                      std::string("hostCCC\0"
                                  "9012",
                                  12),
                      0));

    // Before any request, a peer's DTLS goes nowhere
    CHECK(to(r, a, kDtls, 10).empty());
    // A request goes by its fragment, and its sender's other traffic follows
    CHECK(to(r, a, request("hostAAAA1234:x"), 20) == std::vector<uint32_t>{1});
    CHECK(to(r, a, kDtls, 30) == std::vector<uint32_t>{1});
    CHECK(to(r, b, request("hostBBBB5678:y"), 30) == std::vector<uint32_t>{2});
    CHECK(to(r, b, kDtls, 40) == std::vector<uint32_t>{2});
    // Unknown fragment, indication: dropped, and they teach nothing
    CHECK(to(r, peer(3, 7000), request("hostZZZZ0000:x"), 40).empty());
    CHECK(to(r, peer(3, 7000), kDtls, 40).empty());
    CHECK(to(r, a, stun(0x0011), 40).empty());
    // The same address on the other shared socket is another peer
    CHECK(to(r, peer(1, 5000, 1), kDtls, 40).empty());

    // A provisional peer lapses when its requests stop
    CHECK(to(r, a, kDtls, 20 + Router::kProvisionalLifeMs) == std::vector<uint32_t>{1});
    CHECK(to(r, a, kDtls, 21 + Router::kProvisionalLifeMs).empty());
    CHECK(r.provisionalCount() == 1); // b's

    // Confirmed by its agent, it stays, whatever the time
    CHECK(to(r, a, request("hostAAAA1234:x"), 40'000) == std::vector<uint32_t>{1});
    r.confirm(1, a, 40'000);
    CHECK(r.confirmedCount() == 1);
    CHECK(to(r, a, kDtls, 10'000'000) == std::vector<uint32_t>{1});
    // A request from that address for the other agent goes there, alone:
    // the confirmed traffic does not move
    CHECK(to(r, a, request("hostBBBB5678:y"), 10'000'000) == std::vector<uint32_t>{2});
    CHECK(to(r, a, kDtls, 10'000'001) == std::vector<uint32_t>{1});

    // An agent gone takes its peers with it
    r.removeAgent(1);
    CHECK(to(r, a, kDtls, 10'000'002).empty());
    CHECK(r.confirmedCount() == 0);
    r.removeAgent(2);
    CHECK(!r.hasAgents());
}

void following()
{
    SECTION("IceMuxCore: an address is the peer of every agent it named");
    Router r;
    CHECK(r.addAgent(1, "hostAAAA1234", 0));
    CHECK(r.addAgent(2, "hostBBBB5678", 0));
    const Peer a = peer(1, 5000);
    const uint64_t life = Router::kProvisionalLifeMs;
    const std::vector<uint32_t> both{1, 2}, first{1}, second{2};

    CHECK(to(r, a, request("hostAAAA1234:x"), 100) == first);
    CHECK(to(r, a, kDtls, 150) == first);
    // It names the other agent too: the request goes there alone, and the
    // rest of what the address sends goes to both from then on
    CHECK(to(r, a, request("hostBBBB5678:y"), 200) == second);
    CHECK(to(r, a, kDtls, 300) == both);
    CHECK(r.provisionalCount() == 1);
    CHECK(r.provisionalCount(1) == 1 && r.provisionalCount(2) == 1);
    // Each agent keeps the address by the requests that name it, no other's
    CHECK(to(r, a, request("hostBBBB5678:y"), 90 + life) == second);
    CHECK(to(r, a, kDtls, 100 + life) == both);
    CHECK(to(r, a, kDtls, 101 + life) == second);
    CHECK(r.provisionalCount(1) == 0 && r.provisionalCount(2) == 1);
    CHECK(to(r, a, kDtls, 91 + 2 * life).empty());
    CHECK(r.provisionalCount() == 0 && r.provisionalCount(2) == 0);

    // An agent gone leaves the address to the others
    const Peer c = peer(3, 7000);
    CHECK(to(r, c, request("hostAAAA1234:x"), 200'000) == first);
    CHECK(to(r, c, request("hostBBBB5678:y"), 200'001) == second);
    r.removeAgent(1);
    CHECK(to(r, c, kDtls, 200'002) == second);
    r.removeAgent(2);
    CHECK(r.provisionalCount() == 0);

    // The agent's own word settles it: the address is that agent's alone
    const Peer d = peer(4, 8000);
    const std::vector<uint32_t> third{3}, fourth{4}, thirdAndFourth{3, 4};
    CHECK(r.addAgent(3, "hostCCCC9012", 300'000));
    CHECK(r.addAgent(4, "hostDDDD3456", 300'000));
    CHECK(to(r, d, request("hostCCCC9012:x"), 300'000) == third);
    CHECK(to(r, d, request("hostDDDD3456:y"), 300'001) == fourth);
    CHECK(to(r, d, kDtls, 300'002) == thirdAndFourth);
    r.confirm(4, d, 300'002);
    CHECK(r.provisionalCount() == 0);
    CHECK(r.provisionalCount(3) == 0 && r.provisionalCount(4) == 0);
    CHECK(to(r, d, kDtls, 300'003) == fourth);
    CHECK(to(r, d, request("hostCCCC9012:x"), 300'004) == third);
    CHECK(to(r, d, kDtls, 300'005) == fourth);
    CHECK(r.provisionalCount() == 0);
}

void flood()
{
    SECTION("IceMuxCore: unverified traffic displaces nothing verified");
    Router r;
    CHECK(r.addAgent(1, "hostAAAA1234", 0));
    const Peer real = peer(9, 9000);
    CHECK(to(r, real, request("hostAAAA1234:x"), 1) == std::vector<uint32_t>{1});
    r.confirm(1, real, 1);

    // Many more senders than an agent's share, all with the right fragment
    for (uint32_t i = 0; i < 800; ++i) {
        Peer p = peer(static_cast<uint8_t>(i), static_cast<uint16_t>(10000 + i));
        p.address[2] = static_cast<uint8_t>(i >> 8);
        (void)to(r, p, request("hostAAAA1234:x"), 100 + i);
    }
    CHECK(r.provisionalCount() == Router::kProvisionalPerFeedMax);
    CHECK(r.confirmedCount() == 1);
    CHECK(to(r, real, kDtls, 5000) == std::vector<uint32_t>{1});

    // An agent cannot confirm without end either
    for (uint32_t i = 0; i < 2 * Router::kConfirmedPerFeedMax; ++i)
        r.confirm(1, peer(static_cast<uint8_t>(i), 20000), 5000);
    CHECK(r.confirmedCount() == Router::kConfirmedPerFeedMax);
    r.confirm(7, peer(1, 1), 5000); // no such agent
    CHECK(r.confirmedCount() == Router::kConfirmedPerFeedMax);
}

void runningStreams()
{
    SECTION("IceMuxCore: one agent's word does not take a running stream's peer");
    Router r;
    CHECK(r.addAgent(1, "hostAAAA1234", 0));
    CHECK(r.addAgent(2, "hostBBBB5678", 0));
    const Peer a = peer(1, 5000);
    const uint64_t quiet = Router::kConfirmedQuietMs;
    const std::vector<uint32_t> first{1}, second{2};

    CHECK(to(r, a, request("hostAAAA1234:x"), 1000) == first);
    r.confirm(1, a, 1000);
    CHECK(to(r, a, kDtls, 2000) == first);
    // The other agent says the peer is its own while the first still hears
    // it: nothing moves
    r.confirm(2, a, 2500);
    CHECK(r.confirmedCount() == 1);
    CHECK(to(r, a, kDtls, 3000) == first);
    // Nor a moment before the peer has been quiet long enough, counted from
    // the last thing the first agent was handed
    r.confirm(2, a, 3000 + quiet - 1);
    uint64_t heard = 3000 + quiet - 1;
    CHECK(to(r, a, kDtls, heard) == first);
    // A request of the peer that names its own agent is heard as well
    heard += 100;
    CHECK(to(r, a, request("hostAAAA1234:x"), heard) == first);
    r.confirm(2, a, heard + quiet - 1);
    CHECK(r.confirmedCount() == 1);
    heard += quiet - 1;
    CHECK(to(r, a, kDtls, heard) == first);
    // One that names the other agent goes there, and is nothing the first
    // agent heard: the quiet time runs on
    CHECK(to(r, a, request("hostBBBB5678:y"), heard + quiet) == second);
    // Quiet that long for the first agent: the address has left that stream,
    // and the other agent's word settles it
    r.confirm(2, a, heard + quiet);
    CHECK(r.confirmedCount() == 1);
    CHECK(to(r, a, kDtls, heard + quiet + 1) == second);
    // And it is the second agent's as firmly as it was the first's
    r.confirm(1, a, heard + quiet + 2);
    CHECK(to(r, a, kDtls, heard + quiet + 3) == second);
    // An agent gone leaves its peer free at once
    r.removeAgent(2);
    CHECK(r.confirmedCount() == 0);
    CHECK(to(r, a, request("hostAAAA1234:x"), heard + quiet + 10) == first);
    r.confirm(1, a, heard + quiet + 10);
    CHECK(to(r, a, kDtls, heard + quiet + 11) == first);

    // An agent that keeps all the peers it may takes no other agent's,
    // however quiet: the peer stays where it was
    Router full;
    CHECK(full.addAgent(1, "hostAAAA1234", 0));
    CHECK(full.addAgent(2, "hostBBBB5678", 0));
    const Peer theirs = peer(200, 6000);
    full.confirm(2, theirs, 0);
    for (uint32_t i = 0; i < Router::kConfirmedPerFeedMax; ++i)
        full.confirm(1, peer(static_cast<uint8_t>(i), 20000), 0);
    CHECK(full.confirmedCount() == Router::kConfirmedPerFeedMax + 1);
    full.confirm(1, theirs, 1'000'000);
    CHECK(full.confirmedCount() == Router::kConfirmedPerFeedMax + 1);
    CHECK(to(full, theirs, kDtls, 1'000'001) == second);
}

/// The `i`th of as many distinct senders as a test wants.
Peer nth(uint32_t i)
{
    Peer p = peer(static_cast<uint8_t>(i), static_cast<uint16_t>(10000 + i));
    p.address[2] = static_cast<uint8_t>(i >> 8);
    return p;
}

void shares()
{
    SECTION("IceMuxCore: each agent has its share of the provisional peers");
    Router r;
    CHECK(r.addAgent(1, "hostAAAA1234", 0));
    CHECK(r.addAgent(2, "hostBBBB5678", 0));
    const Peer a = peer(200, 5000);
    const size_t share = Router::kProvisionalPerFeedMax;
    CHECK(to(r, a, request("hostAAAA1234:x"), 10) == std::vector<uint32_t>{1});

    // One agent hears from many more senders than its share: it makes room
    // among its own, the oldest first, and the other's peer stays
    const uint32_t many = 800;
    for (uint32_t i = 0; i < many; ++i)
        (void)to(r, nth(i), request("hostBBBB5678:y"), 20 + i);
    CHECK(r.provisionalCount(2) == share);
    CHECK(r.provisionalCount(1) == 1);
    CHECK(r.provisionalCount() == share + 1);
    CHECK(to(r, a, kDtls, 2000) == std::vector<uint32_t>{1});
    CHECK(to(r, nth(many - 1), kDtls, 2000) == std::vector<uint32_t>{2});
    CHECK(to(r, nth(0), kDtls, 2000).empty());

    // An address both agents were named by counts in the share of each, and
    // the one that needs room leaves it to the other
    CHECK(to(r, a, request("hostBBBB5678:y"), 2100) == std::vector<uint32_t>{2});
    CHECK(r.provisionalCount(2) == share && r.provisionalCount() == share);
    CHECK(to(r, a, kDtls, 2200).size() == 2);
    for (uint32_t i = 0; i < share; ++i)
        (void)to(r, nth(1000 + i), request("hostBBBB5678:y"), 3000 + i);
    CHECK(r.provisionalCount(2) == share && r.provisionalCount(1) == 1);
    CHECK(to(r, a, kDtls, 4000) == std::vector<uint32_t>{1});

    // As many agents as there can be, each with its share full: nothing of
    // one is touched by another, and no further agent is taken
    std::vector<std::string> names;
    for (uint32_t feed = 3; feed <= Router::kAgentsMax; ++feed) {
        names.push_back("hostEEEE00" + std::to_string(10 + feed));
        CHECK(r.addAgent(feed, names.back(), 0));
        const Bytes forIt = request(names.back() + ":z");
        for (uint32_t i = 0; i < 2 * share; ++i)
            (void)to(r, nth(10'000 + 100 * feed + i), forIt, 5000);
        CHECK(r.provisionalCount(feed) == share);
    }
    CHECK(!r.addAgent(Router::kAgentsMax + 1, "hostFFFF0000", 0));
    CHECK(r.provisionalCount() == share * (Router::kAgentsMax - 1) + 1);
    CHECK(r.provisionalCount(1) == 1 && r.provisionalCount(2) == share);
    CHECK(to(r, a, kDtls, 6000) == std::vector<uint32_t>{1});

    // One address that names them all is the peer of them all, and of no more
    const Peer everybody = peer(250, 9000);
    (void)to(r, everybody, request("hostAAAA1234:x"), 7000);
    (void)to(r, everybody, request("hostBBBB5678:y"), 7000);
    for (const std::string& name : names)
        (void)to(r, everybody, request(name + ":z"), 7000);
    CHECK(to(r, everybody, kDtls, 7001).size() == Router::kAgentsMax);
    CHECK(to(r, everybody, stun(0x0101), 7002).size() == Router::kAgentsMax);

    // The counts follow what leaves: confirmed, lapsed, or gone with its agent
    r.confirm(1, a, 7002);
    CHECK(r.provisionalCount(1) == 1); // `everybody`
    r.confirm(2, everybody, 7002);
    CHECK(r.provisionalCount(1) == 0 && r.provisionalCount(3) == share - 1);
    for (uint32_t feed = 3; feed <= Router::kAgentsMax; ++feed)
        r.removeAgent(feed);
    CHECK(r.provisionalCount() == share - 1);
    CHECK(r.provisionalCount(2) == share - 1);
    CHECK(to(r, nth(1001), kDtls, 100'000).empty()); // lapsed long ago
    CHECK(r.provisionalCount(2) == share - 2);
}

void sharedResponses()
{
    SECTION("IceMuxCore: a response reaches the agent whose check it answers");
    Router r;
    CHECK(r.addAgent(1, "hostAAAA1234", 0));
    CHECK(r.addAgent(2, "hostBBBB5678", 0));
    const Peer a = peer(1, 5000), b = peer(2, 6000), c = peer(3, 7000);
    const Bytes response = stun(0x0101);
    const uint32_t budget = Router::kSharedResponsesPerSecond;
    const std::vector<uint32_t> both{2, 1}, first{1}, second{2};

    // The address has named one agent so far, and the other checks with it:
    // its responses reach both
    CHECK(to(r, a, request("hostBBBB5678:y"), 100) == second);
    CHECK(to(r, a, response, 200) == both);
    // A response only: the rest goes to the agents it named
    CHECK(to(r, a, kDtls, 300) == second);
    CHECK(to(r, a, stun(0x0011), 300).empty());

    // So many a second for one address, to the agents it has not named;
    // another address has its own count
    uint32_t shared = 0;
    for (uint32_t i = 0; i < 20 * budget; ++i)
        shared += to(r, a, response, 1000 + i % 500) == both ? 1 : 0;
    CHECK(shared == budget);
    CHECK(to(r, a, response, 1600) == second);
    CHECK(to(r, b, request("hostBBBB5678:y"), 1600) == second);
    CHECK(to(r, b, response, 1601) == both);

    // Once the address has named an agent, that agent gets every one of its
    // responses, whatever their number, and the rest of what it sends
    CHECK(to(r, a, request("hostAAAA1234:x"), 1700) == first);
    uint32_t reached = 0;
    for (uint32_t i = 0; i < 1000; ++i)
        reached += to(r, a, response, 1700 + i % 200) == both ? 1 : 0;
    CHECK(reached == 1000);
    CHECK(to(r, a, kDtls, 1950) == both);

    // The agent's own word ends it: a confirmed peer's responses are its own
    r.confirm(1, a, 2000);
    CHECK(to(r, a, response, 2100) == first);
    CHECK(to(r, a, kDtls, 2100) == first);

    // Agents past their negotiation are no longer asked
    CHECK(to(r, c, request("hostBBBB5678:y"), 70'000) == second);
    CHECK(to(r, c, response, 70'001) == second);
    CHECK(r.addAgent(3, "hostCCCC9012", 70'000));
    CHECK(to(r, c, response, 70'002) == (std::vector<uint32_t>{2, 3}));
}

void unclaimed()
{
    SECTION("IceMuxCore: a STUN response nobody is known to wait for");
    Router r;
    CHECK(r.addAgent(1, "hostAAAA1234", 0));
    CHECK(r.addAgent(2, "hostBBBB5678", 50'000));
    const Bytes response = stun(0x0101);
    const Peer far = peer(77, 3478);

    // Both are negotiating: both get it
    std::vector<uint32_t> got = to(r, far, response, 55'000);
    CHECK(got.size() == 2);
    // The first registered a minute ago: it no longer does
    got = to(r, far, response, 61'000);
    CHECK(got == std::vector<uint32_t>{2});
    // So many a second, no more
    uint32_t passed = 0;
    for (uint32_t i = 0; i < 2 * Router::kUnclaimedPerSecond; ++i)
        passed += to(r, far, response, 70'000 + i % 500).empty() ? 0 : 1;
    CHECK(passed == Router::kUnclaimedPerSecond);
    CHECK(!to(r, far, response, 71'001).empty());
    // And when every agent is past its negotiation: nobody
    CHECK(to(r, far, response, 500'000).empty());
}

} // namespace

void run_ice_mux_core_tests()
{
    reading();
    header();
    routing();
    following();
    flood();
    runningStreams();
    shares();
    sharedResponses();
    unclaimed();
}
