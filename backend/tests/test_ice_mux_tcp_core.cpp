/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * What the server decides of a TCP connection on a shared port
 * (network/IceMuxTcpCore): where it waits, whose place it takes, which agent
 * it goes to, and how many an agent is sent. No socket here: a socket is a
 * number, and the test checks that each is let go of once and only once.
 */
#include "test_framework.h"
#include "network/IceMuxTcpCore.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <string>
#include <vector>

using namespace mw::icemuxtcp;

namespace {

using Bytes = std::vector<uint8_t>;
using Sockets = Closed;

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

/// A Binding request as a browser sends it: USERNAME is not the first one
Bytes request(const std::string& username)
{
    return stun(0x0001, {{0x0024, std::string(4, '\1')}, {0x0006, username}});
}

/// What goes on the wire for one message (RFC 4571): its length, then it
Bytes framed(const Bytes& message)
{
    Bytes out(2 + message.size());
    put16(out, 0, static_cast<uint16_t>(message.size()));
    std::copy(message.begin(), message.end(), out.begin() + 2);
    return out;
}

Source v4(uint8_t last)
{
    const uint8_t address[4] = {192, 0, 2, last};
    return sourceOf(4, address);
}

Source v6(uint8_t net, uint8_t host)
{
    uint8_t address[16] = {0x20, 0x01, 0x0d, 0xb8, 0, 0, 0, net, 0, 0, 0, 0, 0, 0, 0, host};
    return sourceOf(6, address);
}

/// Gives the connection in `place` these bytes, `chunk` at a time at most
/// and never more than it asks for. The last step, and how many were taken.
Table::Step give(Table& t, int place, const Bytes& wire, size_t chunk, size_t* taken = nullptr)
{
    size_t at = 0;
    Table::Step step = Table::Step::More;
    while (at < wire.size() && step == Table::Step::More) {
        const Table::Need need = t.need(place);
        if (!need.into || need.count == 0) break;
        const size_t n = (std::min)({need.count, chunk, wire.size() - at});
        std::memcpy(need.into, wire.data() + at, n);
        at += n;
        step = t.got(place, n);
    }
    if (taken) *taken = at;
    return step;
}

/// The worker an agent of these tests is of: one for each agent
uint32_t ownerOf(uint32_t feed)
{
    return 100 + feed;
}

const uint16_t kPort = 48550;
const uint16_t kOther = 3478;
const char* const kUfrag = "hostAAAA1234";

/// A connection from `from` on `port` that sends `message` whole: where it
/// went.
Table::Handed sends(Table& t, uintptr_t socket, const Source& from, uint16_t port,
                    const Bytes& message, uint64_t now, Sockets& closed)
{
    const int place = t.accept(socket, from, port, now, closed);
    if (place < 0) return {};
    if (give(t, place, framed(message), 4096) != Table::Step::Whole) {
        t.refuse(place, closed);
        return {};
    }
    return t.route(place, now, closed);
}

/// Let go of, and once
bool has(const Sockets& closed, uintptr_t socket)
{
    return std::count_if(closed.begin(), closed.end(),
                         [socket](const Gone& gone) { return gone.socket == socket; }) == 1;
}

/// How it is to be closed: as a connection a worker may hold an end of
bool handedOver(const Sockets& closed, uintptr_t socket)
{
    return std::any_of(closed.begin(), closed.end(),
                       [socket](const Gone& gone) { return gone.socket == socket && gone.handed; });
}

void windows()
{
    SECTION("IceMuxTcpCore: which agents expect a connection");
    Table t;
    Sockets closed;
    const uint16_t two[] = {kOther, kPort};
    CHECK(t.idle() && !t.anyWindow());
    CHECK(!t.open(0, kUfrag, two, 2, 0, ownerOf(0)));
    CHECK(!t.open(1, "short", two, 2, 0, ownerOf(1)));
    CHECK(!t.open(1, "sixteenCharsLong", two, 2, 0, ownerOf(1)));
    CHECK(!t.open(1, kUfrag, two, 0, 0, ownerOf(1)));
    CHECK(!t.open(1, kUfrag, nullptr, 1, 0, ownerOf(1)));
    const uint16_t five[] = {1, 2, 3, 4, 5};
    CHECK(!t.open(1, kUfrag, five, 5, 0, ownerOf(1)));
    const uint16_t nought[] = {kPort, 0};
    CHECK(!t.open(1, kUfrag, nought, 2, 0, ownerOf(1)));
    // Nobody's worker
    CHECK(!t.open(1, kUfrag, two, 2, 0, 0));
    CHECK(!t.anyWindow());

    CHECK(t.open(1, kUfrag, two, 2, 0, ownerOf(1)));
    CHECK(t.expects(1) && !t.expects(2) && t.anyWindow() && !t.idle());
    // One window for an agent, one agent for a fragment
    CHECK(!t.open(1, "hostBBBB1234", two, 2, 0, ownerOf(1)));
    CHECK(!t.open(2, kUfrag, two, 2, 0, ownerOf(2)));
    // The same port twice is one port
    const uint16_t twice[] = {kPort, kPort, 5349};
    CHECK(t.open(2, "hostBBBB1234", twice, 3, 0, ownerOf(2)));
    uint16_t ports[8] = {};
    CHECK_EQ(t.listenPorts(ports, 8), size_t(3));
    CHECK(ports[0] == kOther && ports[1] == kPort && ports[2] == 5349);
    CHECK_EQ(t.listenPorts(ports, 2), size_t(2));

    t.close(1, closed);
    CHECK(!t.expects(1) && t.expects(2));
    CHECK_EQ(t.listenPorts(ports, 8), size_t(2));
    CHECK(ports[0] == kPort && ports[1] == 5349);
    t.close(1, closed); // twice is nothing
    t.close(2, closed);
    CHECK(t.idle() && closed.empty());
    // And its fragment is free again
    CHECK(t.open(3, kUfrag, two, 2, 0, ownerOf(3)));

    // As many as the mux knows agents, and no more
    Table full;
    const auto most = static_cast<uint32_t>(kWindowsMax);
    for (uint32_t feed = 1; feed <= most; ++feed)
        CHECK(full.open(feed, "agent" + std::to_string(1000 + feed), two, 2, 0, ownerOf(feed)));
    CHECK(!full.open(most + 1, "agentOneTooMany", two, 2, 0, ownerOf(most + 1)));
    full.close(7, closed);
    CHECK(full.open(most + 1, "agentOneTooMany", two, 2, 0, ownerOf(most + 1)));
}

void firstFrame()
{
    SECTION("IceMuxTcpCore: the first frame, and not a byte more");
    const uint16_t one[] = {kPort};
    const Bytes good = framed(request(std::string(kUfrag) + ":peer"));
    for (const size_t chunk : {size_t(1), size_t(2), size_t(3), size_t(7), size_t(4096)}) {
        Table t;
        Sockets closed;
        CHECK(t.open(1, kUfrag, one, 1, 0, ownerOf(1)));
        const int place = t.accept(100, v4(1), kPort, 0, closed);
        CHECK(place >= 0 && t.socketAt(place) == 100);
        // The length first, and only it
        CHECK_EQ(t.need(place).count, size_t(2));
        // Followed on the wire by what is not this process's to read
        Bytes wire = good;
        wire.insert(wire.end(), 50, 0xEE);
        size_t taken = 0;
        CHECK(give(t, place, wire, chunk, &taken) == Table::Step::Whole);
        CHECK_EQ(taken, good.size());
        // Whole: it asks for nothing more
        CHECK_EQ(t.need(place).count, size_t(0));
        CHECK(t.got(place, 1) == Table::Step::Close);
        const Table::Handed handed = t.route(place, 0, closed);
        CHECK(handed.feed == 1 && handed.number != 0);
        CHECK(t.socketAt(place) == kNoSocket && closed.empty());
        Table::Held held;
        CHECK(t.held(handed.number, &held));
        CHECK(held.socket == 100 && held.feed == 1 && held.size == good.size() - 2);
        CHECK(std::memcmp(held.frame, good.data() + 2, held.size) == 0);
    }

    Table t;
    Sockets closed;
    CHECK(t.open(1, kUfrag, one, 1, 0, ownerOf(1)));
    // The lengths no frame has: one under a STUN header, one over the most
    for (const uint16_t length : {uint16_t(0), uint16_t(19), uint16_t(1025), uint16_t(0xFFFF)}) {
        const int place = t.accept(200 + length, v4(2), kPort, 0, closed);
        Bytes header(2);
        put16(header, 0, length);
        CHECK(give(t, place, header, 1) == Table::Step::Close);
        t.refuse(place, closed);
        CHECK(has(closed, 200 + length) && t.socketAt(place) == kNoSocket);
    }
    CHECK_EQ(t.counters().malformed, uint64_t(4));
    // The two ends of what one has: asked for whole, and taken
    for (const uint16_t length : {uint16_t(kFrameMin), uint16_t(kFrameMax)}) {
        const int place = t.accept(300 + length, v4(3), kPort, 0, closed);
        Bytes header(2);
        put16(header, 0, length);
        CHECK(give(t, place, header, 2) == Table::Step::More);
        CHECK_EQ(t.need(place).count, size_t(length));
        t.drop(place, closed);
    }
    CHECK_EQ(t.counters().ended, uint64_t(2));

    // What the table is told it read, it checks: nothing, more than it asked
    // for, a place that holds nobody
    const int place = t.accept(400, v4(4), kPort, 0, closed);
    CHECK(t.got(place, 0) == Table::Step::Close);
    CHECK(t.got(place, 3) == Table::Step::Close);
    CHECK(t.got(place, 1) == Table::Step::More);
    CHECK(t.got(place, 2) == Table::Step::Close);
    // A frame that is not whole is routed to nobody
    closed.clear();
    CHECK(t.route(place, 0, closed).feed == 0 && has(closed, 400));
    CHECK(t.got(place, 1) == Table::Step::Close && t.need(place).into == nullptr);
    CHECK(t.got(-1, 1) == Table::Step::Close && t.got(int(kPlaces), 1) == Table::Step::Close);
    CHECK(t.need(-1).into == nullptr && t.socketAt(99) == kNoSocket);
    closed.clear();
    t.drop(place, closed);
    t.refuse(-1, closed);
    CHECK(closed.empty() && t.route(-1, 0, closed).feed == 0);
}

void places()
{
    SECTION("IceMuxTcpCore: the places, and whose a newcomer takes");
    const uint16_t one[] = {kPort};
    Table t;
    Sockets closed;
    // Nobody expects a connection: none is kept
    CHECK(t.accept(1, v4(1), kPort, 0, closed) == -1 && has(closed, 1));
    CHECK_EQ(t.counters().unexpected, uint64_t(1));
    CHECK(t.accept(kNoSocket, v4(1), kPort, 0, closed) == -1);
    closed.clear();

    CHECK(t.open(1, kUfrag, one, 1, 0, ownerOf(1)));
    // Eight sources, eight places
    for (uint8_t i = 0; i < kPlaces; ++i)
        CHECK(t.accept(100 + i, v4(i), kPort, i, closed) == int(i));
    CHECK(closed.empty());
    // The ninth takes the oldest's, whatever that one has sent
    Bytes header(2);
    put16(header, 0, 100);
    CHECK(give(t, 0, header, 2) == Table::Step::More);
    CHECK(t.accept(200, v4(50), kPort, 10, closed) == 0 && has(closed, 100));
    // And is new there: its own length is asked for
    CHECK_EQ(t.need(0).count, size_t(2));
    // The next takes the next oldest's
    CHECK(t.accept(201, v4(51), kPort, 11, closed) == 1 && has(closed, 101));
    CHECK_EQ(t.counters().evicted, uint64_t(2));

    // One source holds two places and no more: its third takes its own oldest
    Table u;
    closed.clear();
    CHECK(u.open(1, kUfrag, one, 1, 0, ownerOf(1)));
    CHECK(u.accept(300, v4(9), kPort, 0, closed) == 0);
    CHECK(u.accept(301, v4(1), kPort, 1, closed) == 1);
    CHECK(u.accept(302, v4(9), kPort, 2, closed) == 2);
    CHECK(u.accept(303, v4(9), kPort, 3, closed) == 0 && has(closed, 300));
    CHECK(u.accept(304, v4(9), kPort, 4, closed) == 2 && has(closed, 302));
    CHECK(u.socketAt(1) == 301 && closed.size() == 2);
    // However many it opens, the others' places are theirs
    for (uintptr_t s = 0; s < 100; ++s)
        u.accept(1000 + s, v4(9), kPort, 5 + s, closed);
    CHECK(u.socketAt(1) == 301 && u.socketAt(3) == kNoSocket);

    // An IPv6 host is its /64: as many addresses as it likes, two places
    Table w;
    closed.clear();
    CHECK(w.open(1, kUfrag, one, 1, 0, ownerOf(1)));
    CHECK(w.accept(400, v6(1, 1), kPort, 0, closed) == 0);
    CHECK(w.accept(401, v6(1, 2), kPort, 1, closed) == 1);
    CHECK(w.accept(402, v6(1, 3), kPort, 2, closed) == 0 && has(closed, 400));
    // Another network is another source, and an IPv4 address is none of them
    CHECK(w.accept(403, v6(2, 1), kPort, 3, closed) == 2);
    CHECK(w.accept(404, v4(1), kPort, 4, closed) == 3);
    CHECK(closed.size() == 1);
    CHECK(v6(1, 1) == v6(1, 200) && !(v6(1, 1) == v6(2, 1)) && !(v4(1) == v4(2)));
}

void routing()
{
    SECTION("IceMuxTcpCore: which agent a connection goes to");
    const uint16_t both[] = {kOther, kPort};
    const uint16_t block[] = {kPort};
    Table t;
    Sockets closed;
    CHECK(t.open(1, kUfrag, both, 2, 0, ownerOf(1)));
    CHECK(t.open(2, "hostBBBB1234", block, 1, 0, ownerOf(2)));
    const std::string mine = std::string(kUfrag) + ":peerXXXX";

    // By the fragment its request names, on either of that agent's ports
    Table::Handed a = sends(t, 10, v4(1), kPort, request(mine), 0, closed);
    CHECK(a.feed == 1 && a.number != 0);
    Table::Handed b = sends(t, 11, v4(2), kOther, request(mine), 0, closed);
    CHECK(b.feed == 1 && b.number != 0 && b.number != a.number);
    Table::Handed c = sends(t, 12, v4(3), kPort, request("hostBBBB1234:x"), 0, closed);
    CHECK(c.feed == 2);
    CHECK(closed.empty());
    CHECK_EQ(t.counters().handed, uint64_t(3));

    // On a port that agent does not hold: to nobody
    CHECK(sends(t, 20, v4(4), kOther, request("hostBBBB1234:x"), 0, closed).feed == 0);
    CHECK(has(closed, 20));
    // A fragment nobody has, part of one, one with more after it, none at all
    CHECK(sends(t, 21, v4(4), kPort, request("hostCCCC1234:x"), 0, closed).feed == 0);
    CHECK(sends(t, 22, v4(4), kPort, request("hostBBBB:x"), 0, closed).feed == 0);
    CHECK(sends(t, 23, v4(4), kPort, request("hostBBBB12345:x"), 0, closed).feed == 0);
    CHECK(sends(t, 24, v4(4), kPort, request("hostBBBB1234"), 0, closed).feed == 0);
    CHECK(sends(t, 25, v4(4), kPort, stun(0x0001), 0, closed).feed == 0);
    // What is not a request: a response, an indication, another method
    CHECK(sends(t, 26, v4(4), kPort, stun(0x0101, {{0x0006, "hostBBBB1234:x"}}), 0, closed).feed ==
          0);
    CHECK(sends(t, 27, v4(4), kPort, stun(0x0011, {{0x0006, "hostBBBB1234:x"}}), 0, closed).feed ==
          0);
    CHECK(sends(t, 28, v4(4), kPort, stun(0x0003, {{0x0006, "hostBBBB1234:x"}}), 0, closed).feed ==
          0);
    // What is not STUN, and a message whose own length is not the frame's
    CHECK(sends(t, 29, v4(4), kPort, Bytes(40, 0x16), 0, closed).feed == 0);
    Bytes longer = request("hostBBBB1234:x");
    longer.resize(longer.size() + 4, 0);
    CHECK(sends(t, 30, v4(4), kPort, longer, 0, closed).feed == 0);
    for (uintptr_t s = 20; s <= 30; ++s)
        CHECK(has(closed, s));
    CHECK_EQ(t.counters().unrouted, uint64_t(11));
    CHECK_EQ(closed.size(), size_t(11));

    // What was closed here was never anybody else's
    for (const Gone& gone : closed)
        CHECK(!gone.handed);

    // The answer is the worker's own: for what it was handed, and nothing of
    // another's
    closed.clear();
    CHECK(!t.answered(ownerOf(2), a.number, closed));
    CHECK(!t.answered(0, a.number, closed));
    CHECK(!t.answered(ownerOf(1), 999999, closed));
    CHECK(closed.empty());
    CHECK(t.answered(ownerOf(1), b.number, closed) && has(closed, 11));
    CHECK(!t.answered(ownerOf(1), b.number, closed));
    Table::Held held;
    CHECK(!t.held(b.number, &held) && !t.held(0, &held) && t.held(a.number, &held));
    CHECK(held.socket == 10);
    // With no number: the one handed first
    CHECK(t.answered(ownerOf(1), 0, closed) && has(closed, 10));
    CHECK(!t.answered(ownerOf(1), 0, closed));
    CHECK(t.answered(ownerOf(2), 0, closed) && has(closed, 12));
    CHECK_EQ(t.counters().answered, uint64_t(3));
}

void bounds()
{
    SECTION("IceMuxTcpCore: what naming an agent can cost");
    const uint16_t one[] = {kPort};
    Table t;
    Sockets closed;
    CHECK(t.open(1, kUfrag, one, 1, 0, ownerOf(1)));
    CHECK(t.open(2, "hostBBBB1234", one, 1, 0, ownerOf(2)));
    const Bytes mine = request(std::string(kUfrag) + ":x");
    const Bytes other = request("hostBBBB1234:x");

    // Two the worker has not answered for: the third is closed here
    const Table::Handed first = sends(t, 1, v4(1), kPort, mine, 0, closed);
    const Table::Handed second = sends(t, 2, v4(2), kPort, mine, 0, closed);
    CHECK(first.feed == 1 && second.feed == 1 && closed.empty());
    CHECK(sends(t, 3, v4(3), kPort, mine, 0, closed).feed == 0 && has(closed, 3));
    CHECK_EQ(t.counters().bounded, uint64_t(1));
    // Which costs the other agent nothing
    CHECK(sends(t, 4, v4(4), kPort, other, 0, closed).feed == 2);
    // An answer makes room for one
    CHECK(t.answered(ownerOf(1), first.number, closed) && has(closed, 1));
    CHECK(sends(t, 5, v4(5), kPort, mine, 0, closed).feed == 1);
    CHECK(sends(t, 6, v4(6), kPort, mine, 0, closed).feed == 0);
    // And so does the time the worker had to answer
    closed.clear();
    t.expire(kAnswerMs - 1, closed);
    CHECK(closed.empty());
    t.expire(kAnswerMs, closed);
    CHECK(has(closed, 2) && has(closed, 5) && has(closed, 4) && closed.size() == 3);
    CHECK_EQ(t.counters().unanswered, uint64_t(3));
    CHECK(sends(t, 7, v4(7), kPort, mine, kAnswerMs, closed).feed == 1);
    CHECK(t.answered(ownerOf(1), 0, closed));

    // So many in a window's life, answered or not, and no more
    Table u;
    closed.clear();
    CHECK(u.open(1, kUfrag, one, 1, 0, ownerOf(1)));
    CHECK(u.open(2, "hostBBBB1234", one, 1, 0, ownerOf(2)));
    uintptr_t socket = 100;
    for (uint32_t i = 0; i < kHandedPerWindow; ++i) {
        CHECK(sends(u, socket++, v4(uint8_t(i)), kPort, mine, 0, closed).feed == 1);
        CHECK(u.answered(ownerOf(1), 0, closed));
    }
    CHECK(sends(u, socket, v4(200), kPort, mine, 0, closed).feed == 0 && has(closed, socket));
    CHECK(sends(u, 999, v4(201), kPort, other, 0, closed).feed == 2);
    CHECK_EQ(u.counters().handed, uint64_t(kHandedPerWindow + 1));
}

void timeouts()
{
    SECTION("IceMuxTcpCore: what runs out of time");
    const uint16_t one[] = {kPort};
    Table t;
    Sockets closed;
    CHECK(t.open(1, kUfrag, one, 1, 1000, ownerOf(1)));
    const Bytes mine = request(std::string(kUfrag) + ":x");

    // The frame, within two seconds of the accept, however much of it came
    const int slow = t.accept(1, v4(1), kPort, 1000, closed);
    const int slower = t.accept(2, v4(2), kPort, 1500, closed);
    Bytes part = framed(mine);
    part.resize(10);
    CHECK(give(t, slow, part, 4096) == Table::Step::More);
    t.expire(1000 + kFirstFrameMs - 1, closed);
    CHECK(closed.empty());
    t.expire(1000 + kFirstFrameMs, closed);
    CHECK(has(closed, 1) && closed.size() == 1 && t.socketAt(slow) == kNoSocket);
    CHECK(t.socketAt(slower) == 2);
    t.expire(1500 + kFirstFrameMs, closed);
    CHECK(has(closed, 2));
    CHECK_EQ(t.counters().timedOut, uint64_t(2));

    // The window, 45 s after it opened: what names its agent after that goes
    // to nobody, even before anything looked at the time
    closed.clear();
    CHECK(sends(t, 3, v4(3), kPort, mine, 1000 + kWindowMs - 1, closed).feed == 1);
    CHECK(sends(t, 4, v4(4), kPort, mine, 1000 + kWindowMs, closed).feed == 0 && has(closed, 4));
    const int waits = t.accept(5, v4(5), kPort, 1000 + kWindowMs, closed);
    CHECK(waits >= 0);
    t.expire(1000 + kWindowMs, closed);
    // Its window gone, and it was the last: the one handed and the one that
    // waited are let go, and nothing is expected
    CHECK(has(closed, 3) && has(closed, 5) && closed.size() == 3);
    CHECK(!t.expects(1) && t.idle());
    CHECK(t.accept(6, v4(6), kPort, 1000 + kWindowMs, closed) == -1);
}

void closing()
{
    SECTION("IceMuxTcpCore: an agent that expects none any more");
    const uint16_t one[] = {kPort};
    Table t;
    Sockets closed;
    CHECK(t.open(1, kUfrag, one, 1, 0, ownerOf(1)));
    CHECK(t.open(2, "hostBBBB1234", one, 1, 0, ownerOf(2)));
    CHECK(sends(t, 1, v4(1), kPort, request(std::string(kUfrag) + ":x"), 0, closed).feed == 1);
    CHECK(sends(t, 2, v4(2), kPort, request("hostBBBB1234:x"), 0, closed).feed == 2);
    const int waits = t.accept(3, v4(3), kPort, 0, closed);
    CHECK(waits >= 0 && closed.empty());

    // What was handed for it goes; the other agent's, and what waits, stay
    t.close(1, closed);
    CHECK(has(closed, 1) && closed.size() == 1);
    CHECK(t.socketAt(waits) == 3 && !t.idle());
    CHECK(!t.answered(ownerOf(1), 0, closed));
    // The last one: what waits has nobody to go to
    t.close(2, closed);
    CHECK(has(closed, 2) && has(closed, 3) && closed.size() == 3);
    CHECK(t.idle());
    // What a worker was handed may be its own by now: said so, to whoever
    // closes; what never left this process is not
    CHECK(handedOver(closed, 1) && handedOver(closed, 2) && !handedOver(closed, 3));

    // Everything at once, as when the server stops
    Table u;
    closed.clear();
    CHECK(u.open(1, kUfrag, one, 1, 0, ownerOf(1)));
    CHECK(u.open(2, "hostBBBB1234", one, 1, 0, ownerOf(2)));
    CHECK(sends(u, 11, v4(1), kPort, request(std::string(kUfrag) + ":x"), 0, closed).feed == 1);
    CHECK(u.accept(12, v4(2), kPort, 0, closed) >= 0);
    u.reset(closed);
    CHECK(has(closed, 11) && has(closed, 12) && closed.size() == 2 && u.idle());
    u.reset(closed);
    CHECK(closed.size() == 2);
    uint16_t ports[4];
    CHECK_EQ(t.listenPorts(ports, 4), size_t(0));
}

/// Whatever is done to the table, in whatever order, each socket it was given
/// is let go of once: none twice, and none kept once nothing is expected.
void everySocketOnce()
{
    SECTION("IceMuxTcpCore: each socket is let go of once");
    const uint16_t ports[] = {kOther, kPort};
    const std::string names[] = {"hostAAAA1234", "hostBBBB1234", "hostCCCC1234", "nobodyAtAll12"};
    for (uint32_t seed = 1; seed <= 20; ++seed) {
        Table t;
        Sockets closed;
        std::set<uintptr_t> given;
        std::vector<uint32_t> numbers;
        uint32_t state = seed * 2654435761u;
        const auto next = [&state](uint32_t below) {
            state = state * 1664525u + 1013904223u;
            return (state >> 8) % below;
        };
        uintptr_t socket = 1;
        uint64_t now = 0;
        for (int step = 0; step < 4000; ++step) {
            now += next(300);
            // Each draw on a line of its own: the order they are made in is
            // then the same under every compiler
            const uint32_t what = next(10);
            const uint32_t feed = 1 + next(3);
            if (what == 0) {
                const std::string& name = names[next(3)];
                const size_t count = 1 + next(2);
                t.open(feed, name, ports, count, now, ownerOf(feed));
            } else if (what == 1) {
                t.close(feed, closed);
            } else if (what == 2 || what == 3) {
                const uintptr_t s = socket++;
                const Source from = v4(static_cast<uint8_t>(next(5)));
                const uint16_t port = ports[next(2)];
                const int place = t.accept(s, from, port, now, closed);
                given.insert(s);
                if (place < 0) continue;
                const Bytes wire = framed(request(names[next(4)] + ":x"));
                Bytes sent = wire;
                if (next(4) == 0) sent.resize(next(static_cast<uint32_t>(wire.size())));
                const size_t chunk = 1 + next(40);
                const Table::Step done = give(t, place, sent, chunk);
                if (done == Table::Step::Close) {
                    t.refuse(place, closed);
                } else if (done == Table::Step::Whole) {
                    const Table::Handed handed = t.route(place, now, closed);
                    if (handed.feed != 0) numbers.push_back(handed.number);
                } else if (next(3) == 0) {
                    t.drop(place, closed);
                }
            } else if (what == 4) {
                if (numbers.empty()) continue;
                const uint32_t number = numbers[next(static_cast<uint32_t>(numbers.size()))];
                t.answered(ownerOf(feed), number, closed);
            } else if (what == 5) {
                t.answered(ownerOf(feed), 0, closed);
            } else if (what == 6) {
                if (numbers.empty()) continue;
                const uint32_t number = numbers[next(static_cast<uint32_t>(numbers.size()))];
                if (next(2) == 0)
                    t.name(number);
                else
                    t.letGo(number, closed);
            } else if (what == 7 && next(8) == 0) {
                t.releaseOwner(ownerOf(feed), closed);
            } else {
                t.expire(now, closed);
            }
        }
        // What was named outlives its agent: only its worker's end, or the
        // server's, lets go of it
        for (uint32_t feed = 1; feed <= 3; ++feed)
            t.close(feed, closed);
        t.reset(closed);
        CHECK(t.idle());
        std::set<uintptr_t> once;
        for (const Gone& gone : closed)
            once.insert(gone.socket);
        CHECK_EQ(once.size(), closed.size());
        CHECK(once == given);
        // And each is counted once, under what became of it
        const Table::Counters& c = t.counters();
        CHECK_EQ(c.handed, c.answered + c.unanswered);
        const uint64_t turnedAway = static_cast<uint64_t>(given.size()) - c.accepted;
        CHECK_EQ(c.accepted + turnedAway, c.handed + c.evicted + c.timedOut + c.ended +
                                              c.malformed + c.unrouted + c.bounded + c.unexpected);
    }
}

/// A connection told to a worker by the number of this process's handle on it
/// is copied out by that worker later. Until the worker says so, or is gone,
/// the handle is kept: let go of earlier, its number could be another's.
void named()
{
    SECTION("IceMuxTcpCore: a connection named to its worker waits for that worker");
    const uint16_t one[] = {kPort};
    const uint32_t worker = 7, other = 9;
    const Bytes mine = request(std::string(kUfrag) + ":x");
    Table t;
    Sockets closed;
    CHECK(t.open(1, kUfrag, one, 1, 0, worker));
    const Table::Handed a = sends(t, 1, v4(1), kPort, mine, 0, closed);
    const Table::Handed b = sends(t, 2, v4(2), kPort, mine, 0, closed);
    CHECK(a.feed == 1 && b.feed == 1 && closed.empty());
    CHECK(t.name(a.number));
    CHECK(!t.name(0) && !t.name(999999));

    // The time a worker has to answer lets go of the other one, not of it
    t.expire(kAnswerMs + 1000, closed);
    CHECK(has(closed, 2) && closed.size() == 1);
    // Nor does its agent's end, nor the time a window lasts
    t.close(1, closed);
    t.expire(10 * kWindowMs, closed);
    CHECK(closed.size() == 1 && !t.idle());
    Table::Held held;
    CHECK(t.held(a.number, &held) && held.socket == 1);
    // Nor a server that finds nobody to hand it to, nor another worker's
    // word, nor an answer that names no connection
    CHECK(!t.letGo(a.number, closed));
    CHECK(!t.answered(other, a.number, closed));
    CHECK(!t.answered(worker, 0, closed));
    CHECK(closed.size() == 1);
    // Its worker's answer does
    CHECK(t.answered(worker, a.number, closed));
    CHECK(has(closed, 1) && handedOver(closed, 1) && t.idle());
    CHECK(!t.answered(worker, a.number, closed));

    // And its worker's end, which lets go of all that worker was handed and
    // of nothing of another's
    closed.clear();
    CHECK(t.open(2, kUfrag, one, 1, 0, worker));
    CHECK(t.open(3, "hostBBBB1234", one, 1, 0, other));
    const Table::Handed c = sends(t, 11, v4(1), kPort, mine, 0, closed);
    const Table::Handed d = sends(t, 12, v4(2), kPort, mine, 0, closed);
    const Table::Handed e = sends(t, 13, v4(3), kPort, request("hostBBBB1234:x"), 0, closed);
    CHECK(c.feed == 2 && d.feed == 2 && e.feed == 3);
    CHECK(t.name(c.number));
    t.releaseOwner(worker, closed);
    CHECK(has(closed, 11) && has(closed, 12) && closed.size() == 2);
    CHECK(!t.expects(2) && t.expects(3));
    t.releaseOwner(0, closed);
    t.releaseOwner(worker, closed);
    CHECK(closed.size() == 2);
    // One that was not named, and that nobody can be handed: let go at once
    CHECK(t.letGo(e.number, closed) && has(closed, 13) && handedOver(closed, 13));
    CHECK(!t.letGo(e.number, closed) && !t.letGo(0, closed));

    // A worker that answers for nothing holds so many, one agent after the
    // other, and no more: what names an agent of its is then closed here
    Table u;
    closed.clear();
    uintptr_t socket = 100;
    uint32_t feed = 1;
    uint64_t now = 0;
    std::vector<uint32_t> kept;
    for (size_t i = 0; i < kNamedPerOwnerMax; ++i, ++feed, now += 10) {
        CHECK(u.open(feed, kUfrag, one, 1, now, worker));
        const Table::Handed handed = sends(u, socket++, v4(1), kPort, mine, now, closed);
        CHECK(handed.feed == feed && u.name(handed.number));
        kept.push_back(handed.number);
        u.close(feed, closed);
    }
    CHECK(closed.empty());
    CHECK(u.open(feed, kUfrag, one, 1, now, worker));
    CHECK(sends(u, socket, v4(1), kPort, mine, now, closed).feed == 0 && has(closed, socket));
    ++socket;
    CHECK_EQ(u.counters().bounded, uint64_t(1));
    // Another worker's agent is handed its own
    CHECK(u.open(feed + 1, "hostBBBB1234", one, 1, now, other));
    CHECK(sends(u, socket++, v4(2), kPort, request("hostBBBB1234:x"), now, closed).feed ==
          feed + 1);
    // One answer makes room for one
    CHECK(u.answered(worker, kept[0], closed) && has(closed, 100));
    CHECK(sends(u, socket++, v4(1), kPort, mine, now, closed).feed == feed);
    // The server's own end: everything
    u.reset(closed);
    CHECK(u.idle());
    CHECK_EQ(closed.size(), size_t(1 + 1 + kNamedPerOwnerMax + 1));
    const Table::Counters& c2 = u.counters();
    CHECK_EQ(c2.handed, c2.answered + c2.unanswered);
}

/// A try of the way here (network/Tcp443Probe): one connection, on one port,
/// within its time. It is nobody's: never an agent's, and closed here.
void tries()
{
    SECTION("IceMuxTcpCore: a try of the way here is nobody's, and counts once");
    const uint16_t one[] = {kPort};
    const std::string token = "tokenTOKEN123456";
    const Bytes knock = request(token + ":p");
    Table t;
    Sockets closed;
    CHECK_EQ(token.size(), kTokenSize);
    // What cannot be a try
    CHECK_EQ(t.expectToken("fifteenCharsLon", kPort, 0), uint32_t(0));
    CHECK_EQ(t.expectToken("seventeenCharsLon", kPort, 0), uint32_t(0));
    CHECK_EQ(t.expectToken(token, 0, 0), uint32_t(0));
    CHECK(t.idle() && !t.expected());

    // Alone, it is what the port is listened on for
    const uint32_t id = t.expectToken(token, kPort, 0);
    CHECK(id != 0);
    CHECK(t.expected() && !t.anyWindow() && !t.idle());
    CHECK(t.token(id) == Table::Token::Waiting);
    uint16_t ports[8] = {};
    CHECK_EQ(t.listenPorts(ports, 8), size_t(1));
    CHECK_EQ(ports[0], kPort);
    // The same fragment is not two tries; so many at once, and no more
    CHECK_EQ(t.expectToken(token, kOther, 0), uint32_t(0));
    const uint32_t second = t.expectToken("tokenTOKEN654321", kOther, 0);
    CHECK(second != 0 && second != id);
    CHECK_EQ(t.expectToken("tokenTOKEN000000", kPort, 0), uint32_t(0));
    t.endToken(second, closed);
    CHECK(t.token(second) == Table::Token::None);
    CHECK(t.token(0) == Table::Token::None);

    // On another port than the one it was made for: not it
    CHECK(sends(t, 1, v4(1), kOther, knock, 10, closed).token == 0 && has(closed, 1));
    CHECK(t.token(id) == Table::Token::Waiting);
    // A frame that names something else of that length: not it
    CHECK(sends(t, 2, v4(1), kPort, request("tokenTOKEN999999:p"), 10, closed).token == 0);
    CHECK(has(closed, 2));
    // Its own: closed here, as a connection nobody else has an end of
    const Table::Handed came = sends(t, 3, v4(1), kPort, knock, 20, closed);
    CHECK(came.token == id && came.feed == 0 && came.number == 0);
    CHECK(has(closed, 3) && !handedOver(closed, 3));
    CHECK(t.token(id) == Table::Token::Arrived);
    CHECK_EQ(t.counters().tried, uint64_t(1));
    CHECK_EQ(t.counters().handed, uint64_t(0));
    // Once: nothing is expected any more, and a second one is turned away
    CHECK(!t.expected() && !t.idle());
    CHECK_EQ(t.listenPorts(ports, 8), size_t(0));
    CHECK(sends(t, 4, v4(1), kPort, knock, 30, closed).token == 0 && has(closed, 4));
    CHECK_EQ(t.counters().unexpected, uint64_t(1));
    CHECK_EQ(t.counters().tried, uint64_t(1));
    CHECK(t.endedOn(kPort) == End::Tried);
    CHECK(t.endedOn(kOther) == End::Tried && t.endedOn(5349) == End::None);
    t.endToken(id, closed);
    CHECK(t.token(id) == Table::Token::None && t.idle());

    // Its time: a connection that comes after it is not it
    Table late;
    closed.clear();
    const uint32_t slow = late.expectToken(token, kPort, 1000);
    const int place = late.accept(1, v4(1), kPort, 1000 + kTokenMs - 1, closed);
    CHECK(slow != 0 && place >= 0);
    CHECK(give(late, place, framed(knock), 4096) == Table::Step::Whole);
    CHECK(late.route(place, 1000 + kTokenMs, closed).token == 0 && has(closed, 1));
    CHECK(late.token(slow) == Table::Token::Waiting);
    late.expire(1000 + kTokenMs, closed);
    CHECK(late.token(slow) == Table::Token::None && late.idle());
    CHECK(late.endedOn(kPort) == End::Tried);

    // One that came and is never asked about goes too, as long after
    Table kept;
    closed.clear();
    const uint32_t unread = kept.expectToken(token, kPort, 0);
    CHECK(sends(kept, 1, v4(1), kPort, knock, 100, closed).token == unread);
    kept.expire(100 + kTokenMs - 1, closed);
    CHECK(kept.token(unread) == Table::Token::Arrived);
    kept.expire(100 + kTokenMs, closed);
    CHECK(kept.token(unread) == Table::Token::None && kept.idle());

    // Beside an agent: the try's connection is never the agent's, nor the
    // agent's the try's
    Table both;
    closed.clear();
    CHECK(both.open(1, kUfrag, one, 1, 0, ownerOf(1)));
    const uint32_t beside = both.expectToken(token, kPort, 0);
    const Table::Handed forAgent =
        sends(both, 1, v4(1), kPort, request(std::string(kUfrag) + ":x"), 0, closed);
    CHECK(forAgent.feed == 1 && forAgent.token == 0 && closed.empty());
    const Table::Handed forTry = sends(both, 2, v4(2), kPort, knock, 0, closed);
    CHECK(forTry.token == beside && forTry.feed == 0 && has(closed, 2) && closed.size() == 1);
    Table::Held held;
    CHECK(both.held(forAgent.number, &held) && held.socket == 1);
    CHECK_EQ(both.counters().handed, uint64_t(1));
    // What waits when the try is over waits on, for the agent
    const int waits = both.accept(3, v4(3), kPort, 0, closed);
    both.endToken(beside, closed);
    CHECK(waits >= 0 && both.socketAt(waits) == 3);
    // ...and has nobody to go to once the agent expects none either
    both.close(1, closed, End::Connected);
    CHECK(has(closed, 3) && has(closed, 1) && both.endedOn(kPort) == End::Connected);
    // The server's own end takes the tries with it
    const uint32_t last = both.expectToken(token, kPort, 0);
    CHECK(last != 0);
    both.reset(closed);
    CHECK(both.token(last) == Table::Token::None && both.idle());
}

/// An agent that lives in this process (the entry link's tunnel) is numbered
/// apart from the workers' agents, takes its connection itself, and is held
/// to the bounds a worker's agent is.
void thisProcess()
{
    SECTION("IceMuxTcpCore: an agent of this process's own takes its connection itself");
    const uint16_t one[] = {kPort};
    const uint32_t here = kHereFeeds | 1, worker = 7;
    const Bytes knock = request("abcd:peer");
    Table t;
    Sockets closed;
    CHECK(hereFeed(here) && !hereFeed(1));
    // Numbered apart, both ways
    CHECK(!t.open(5, kUfrag, one, 1, 0, kHere));
    CHECK(!t.open(here, kUfrag, one, 1, 0, worker));
    // The fragment its library gave it, four characters at least; a worker's
    // agent is held to what the mux takes
    CHECK(!t.open(here, "abc", one, 1, 0, kHere));
    CHECK(!t.open(5, "abcd", one, 1, 0, worker));
    CHECK(!t.open(here, "sixteenCharsLong", one, 1, 0, kHere));
    CHECK(t.open(here, "abcd", one, 1, 0, kHere));
    CHECK(t.open(1, kUfrag, one, 1, 0, worker));

    const Table::Handed forTunnel = sends(t, 1, v4(1), kPort, knock, 0, closed);
    const Table::Handed forWorker =
        sends(t, 2, v4(2), kPort, request(std::string(kUfrag) + ":peer"), 0, closed);
    CHECK(forTunnel.feed == here && forWorker.feed == 1 && closed.empty());
    // Each is its own's alone: what names the tunnel never reaches a worker,
    // and what names a worker's agent is never taken here
    Table::Held held;
    CHECK(!t.take(kHere, forWorker.number, &held));
    CHECK(!t.take(worker, forTunnel.number, &held));
    CHECK(!t.answered(worker, forTunnel.number, closed));
    CHECK(!t.take(kHere, 0, &held) && !t.take(0, forTunnel.number, &held));
    CHECK(!t.take(kHere, forTunnel.number, nullptr));
    CHECK(closed.empty());
    // Taken: out of the table, and its socket is in nobody's list to close
    CHECK(t.take(kHere, forTunnel.number, &held));
    CHECK(held.socket == 1 && held.feed == here);
    CHECK(held.size == knock.size() && std::equal(knock.begin(), knock.end(), held.frame));
    CHECK(!t.take(kHere, forTunnel.number, &held));
    CHECK(closed.empty());
    CHECK_EQ(t.counters().answered, uint64_t(1));
    // The bounds of a worker's agent: so many it has not answered for
    const Table::Handed second = sends(t, 3, v4(1), kPort, knock, 0, closed);
    const Table::Handed third = sends(t, 4, v4(2), kPort, knock, 0, closed);
    CHECK(second.feed == here && third.feed == here);
    CHECK(sends(t, 5, v4(3), kPort, knock, 0, closed).feed == 0 && has(closed, 5));
    CHECK_EQ(t.counters().bounded, uint64_t(1));
    // One that was named stays for whoever it was named to
    CHECK(t.name(second.number) && !t.take(kHere, second.number, &held));
    // Its end lets go of all it did not take, and of nothing of the worker's
    t.releaseOwner(kHere, closed);
    CHECK(has(closed, 3) && has(closed, 4) && closed.size() == 3);
    CHECK(!t.expects(here) && t.expects(1));
    // And the time a window lasts
    CHECK(t.open(here, "abcd", one, 1, 1000, kHere));
    t.expire(1000 + kWindowMs - 1, closed);
    CHECK(t.expects(here));
    t.expire(1000 + kWindowMs, closed);
    CHECK(!t.expects(here) && t.endedOn(kPort) == End::TimedOut);
}

} // namespace

void run_ice_mux_tcp_core_tests()
{
    windows();
    firstFrame();
    places();
    routing();
    bounds();
    timeouts();
    closing();
    named();
    tries();
    thisProcess();
    everySocketOnce();
}
