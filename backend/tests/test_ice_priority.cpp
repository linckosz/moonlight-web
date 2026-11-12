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

} // namespace

void run_ice_priority_tests()
{
    testSrflxPriorityRanksBelowHost();
    testWithPriorityRewritesOnlyThePriority();
    testWithPriorityLeavesAMalformedLineAlone();
    testPublicPortOfAHostCandidate();
}
