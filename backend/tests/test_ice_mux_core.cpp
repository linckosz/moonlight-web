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
    r.confirm(1, a);
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
    SECTION("IceMuxCore: a request does not move another agent's peer");
    Router r;
    CHECK(r.addAgent(1, "hostAAAA1234", 0));
    CHECK(r.addAgent(2, "hostBBBB5678", 0));
    const Peer a = peer(1, 5000);
    const uint64_t life = Router::kProvisionalLifeMs;

    CHECK(to(r, a, request("hostAAAA1234:x"), 100) == std::vector<uint32_t>{1});
    // The same address names the other agent: the request goes there, and
    // the rest of what that address sends stays where it was
    CHECK(to(r, a, request("hostBBBB5678:y"), 200) == std::vector<uint32_t>{2});
    CHECK(to(r, a, kDtls, 300) == std::vector<uint32_t>{1});
    // Nor does such a request keep the entry alive: only its agent's do
    CHECK(to(r, a, request("hostBBBB5678:y"), 90 + life) == std::vector<uint32_t>{2});
    CHECK(to(r, a, kDtls, 100 + life) == std::vector<uint32_t>{1});
    CHECK(to(r, a, kDtls, 101 + life).empty());
    // Lapsed (and dropped by the line above): the address is free to follow
    CHECK(to(r, a, request("hostBBBB5678:y"), 200 + life) == std::vector<uint32_t>{2});
    CHECK(to(r, a, kDtls, 300 + life) == std::vector<uint32_t>{2});

    // Lapsed but still in the table: the next request takes it over
    const Peer b = peer(2, 6000);
    CHECK(to(r, b, request("hostAAAA1234:x"), 1000) == std::vector<uint32_t>{1});
    CHECK(to(r, b, request("hostBBBB5678:y"), 1001 + life) == std::vector<uint32_t>{2});
    CHECK(to(r, b, kDtls, 1002 + life) == std::vector<uint32_t>{2});

    // Its agent gone, an address follows the next one at once
    const Peer c = peer(3, 7000);
    CHECK(to(r, c, request("hostAAAA1234:x"), 200'000) == std::vector<uint32_t>{1});
    r.removeAgent(1);
    CHECK(to(r, c, kDtls, 200'001).empty());
    CHECK(to(r, c, request("hostBBBB5678:y"), 200'002) == std::vector<uint32_t>{2});
    CHECK(to(r, c, kDtls, 200'003) == std::vector<uint32_t>{2});

    // The agent's own word settles it, whoever the address followed before
    const Peer d = peer(4, 8000);
    CHECK(r.addAgent(3, "hostCCCC9012", 300'000));
    CHECK(to(r, d, request("hostCCCC9012:x"), 300'000) == std::vector<uint32_t>{3});
    CHECK(to(r, d, request("hostBBBB5678:y"), 300'001) == std::vector<uint32_t>{2});
    CHECK(to(r, d, kDtls, 300'002) == std::vector<uint32_t>{3});
    r.confirm(2, d);
    CHECK(to(r, d, kDtls, 300'003) == std::vector<uint32_t>{2});
    CHECK(to(r, d, request("hostCCCC9012:x"), 300'004) == std::vector<uint32_t>{3});
    CHECK(to(r, d, kDtls, 300'005) == std::vector<uint32_t>{2});
}

void flood()
{
    SECTION("IceMuxCore: unverified traffic displaces nothing verified");
    Router r;
    CHECK(r.addAgent(1, "hostAAAA1234", 0));
    const Peer real = peer(9, 9000);
    CHECK(to(r, real, request("hostAAAA1234:x"), 1) == std::vector<uint32_t>{1});
    r.confirm(1, real);

    // More senders than the provisional level holds, all with the right fragment
    for (uint32_t i = 0; i < 3 * Router::kProvisionalMax; ++i) {
        Peer p = peer(static_cast<uint8_t>(i), static_cast<uint16_t>(10000 + i));
        p.address[2] = static_cast<uint8_t>(i >> 8);
        (void)to(r, p, request("hostAAAA1234:x"), 100 + i);
    }
    CHECK(r.provisionalCount() == Router::kProvisionalPerFeedMax);
    CHECK(r.confirmedCount() == 1);
    CHECK(to(r, real, kDtls, 5000) == std::vector<uint32_t>{1});

    // An agent cannot confirm without end either
    for (uint32_t i = 0; i < 2 * Router::kConfirmedPerFeedMax; ++i)
        r.confirm(1, peer(static_cast<uint8_t>(i), 20000));
    CHECK(r.confirmedCount() == Router::kConfirmedPerFeedMax);
    r.confirm(7, peer(1, 1)); // no such agent
    CHECK(r.confirmedCount() == Router::kConfirmedPerFeedMax);
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
    CHECK(to(r, a, request("hostAAAA1234:x"), 10) == std::vector<uint32_t>{1});

    // One agent hears from more senders than the whole level holds: it makes
    // room among its own, the oldest first, and the other's peer stays
    const uint32_t many = 3 * static_cast<uint32_t>(Router::kProvisionalMax);
    for (uint32_t i = 0; i < many; ++i)
        (void)to(r, nth(i), request("hostBBBB5678:y"), 20 + i);
    CHECK(r.provisionalCount(2) == Router::kProvisionalPerFeedMax);
    CHECK(r.provisionalCount(1) == 1);
    CHECK(r.provisionalCount() == Router::kProvisionalPerFeedMax + 1);
    CHECK(to(r, a, kDtls, 2000) == std::vector<uint32_t>{1});
    CHECK(to(r, nth(many - 1), kDtls, 2000) == std::vector<uint32_t>{2});
    CHECK(to(r, nth(0), kDtls, 2000).empty());

    // Agents enough to fill the level between them: it stays full, the one
    // that holds the most gives way, and the one that holds one keeps it
    const char* const names[] = {"hostCCCC0003", "hostCCCC0004", "hostCCCC0005", "hostCCCC0006",
                                 "hostCCCC0007", "hostCCCC0008", "hostCCCC0009"};
    uint32_t feed = 3, sender = many;
    for (const char* name : names) {
        CHECK(r.addAgent(feed, name, 0));
        const Bytes forIt = request(std::string(name) + ":z");
        for (size_t i = 0; i < Router::kProvisionalPerFeedMax; ++i, ++sender)
            (void)to(r, nth(sender), forIt, 3000 + sender);
        CHECK(r.provisionalCount(feed) == Router::kProvisionalPerFeedMax);
        ++feed;
    }
    CHECK(r.provisionalCount() == Router::kProvisionalMax);
    CHECK(r.provisionalCount(1) == 1);
    CHECK(to(r, a, kDtls, 9000) == std::vector<uint32_t>{1});
    // The counts follow what leaves: confirmed, lapsed, or gone with its agent
    r.confirm(1, a);
    CHECK(r.provisionalCount(1) == 0);
    r.removeAgent(9);
    CHECK(r.provisionalCount() <= Router::kProvisionalMax - Router::kProvisionalPerFeedMax);
    CHECK(to(r, nth(5000), request("hostBBBB5678:y"), 100'000) == std::vector<uint32_t>{2});
    CHECK(to(r, nth(5001), kDtls, 100'000).empty());
    const size_t held = r.provisionalCount(2);
    CHECK(to(r, nth(many - 1), kDtls, 100'000).empty()); // lapsed long ago
    CHECK(r.provisionalCount(2) == held - 1);
    for (uint32_t i = 0; i < Router::kProvisionalMax; ++i)
        (void)to(r, nth(6000 + i), request("hostCCCC0003:z"), 100'001);
    CHECK(r.provisionalCount(3) == Router::kProvisionalPerFeedMax);
    CHECK(r.provisionalCount(2) == held - 1);
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

    // The address follows one agent so far, and the other checks with it too
    CHECK(to(r, a, request("hostBBBB5678:y"), 100) == std::vector<uint32_t>{2});
    std::vector<uint32_t> got = to(r, a, response, 200);
    CHECK(got.size() == 2 && got.front() == 2 && got.back() == 1);
    // A response only: the rest goes to the one it follows
    CHECK(to(r, a, kDtls, 300) == std::vector<uint32_t>{2});
    CHECK(to(r, a, stun(0x0011), 300).empty());

    // So many a second for one address; another address has its own count
    uint32_t shared = 0;
    for (uint32_t i = 0; i < 2 * budget; ++i)
        shared += to(r, a, response, 1000 + i % 500).size() == 2 ? 1 : 0;
    CHECK(shared == budget);
    CHECK(to(r, a, response, 1600) == std::vector<uint32_t>{2});
    CHECK(to(r, b, request("hostBBBB5678:y"), 1600) == std::vector<uint32_t>{2});
    CHECK(to(r, b, response, 1601).size() == 2);
    CHECK(to(r, a, response, 2001).size() == 2);

    // The agent's own word ends it: a confirmed peer's responses are its own
    r.confirm(1, a);
    CHECK(to(r, a, response, 2100) == std::vector<uint32_t>{1});
    CHECK(to(r, a, kDtls, 2100) == std::vector<uint32_t>{1});

    // Agents past their negotiation are no longer asked
    CHECK(to(r, c, request("hostBBBB5678:y"), 70'000) == std::vector<uint32_t>{2});
    CHECK(to(r, c, response, 70'001) == std::vector<uint32_t>{2});
    CHECK(r.addAgent(3, "hostCCCC9012", 70'000));
    got = to(r, c, response, 70'002);
    CHECK(got.size() == 2 && got.front() == 2 && got.back() == 3);
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
    shares();
    sharedResponses();
    unclaimed();
}
