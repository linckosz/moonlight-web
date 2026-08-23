/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin. GPLv3.
 */
#include "test_framework.h"
#include "server/PortFallback.h"

#include <set>

// The ladder HttpServer::start() falls back to when the well-known port is
// denied (unprivileged desktop install). Its whole point is that the port it
// lands on is persisted and reused on the next boot, so what is checked here is
// the property that makes that safe: every rung sits outside the range the OS
// hands out to outgoing connections (32768-60999 on Linux, 49152-65535 on
// Windows) and outside the privileged range. Issue #8: the old ladder started at
// 49443/49080, inside both, and the local address drifted on every restart.
void run_port_fallback_tests()
{
    SECTION("PortFallback");

    CHECK(!PortFallback::kHttps.empty());
    CHECK(!PortFallback::kHttp.empty());

    // No rung can be stolen by an ephemeral socket, nor need privileges.
    for (uint16_t p : PortFallback::kHttps) {
        CHECK(PortFallback::isStable(p));
        CHECK(p > 1023 && p < 32768);
    }
    for (uint16_t p : PortFallback::kHttp) {
        CHECK(PortFallback::isStable(p));
        CHECK(p > 1023 && p < 32768);
    }

    // Distinct rungs, and the two ladders never collide: a second instance
    // walking down them must not land on the port the first one already holds.
    std::set<uint16_t> seen;
    for (uint16_t p : PortFallback::kHttps)
        seen.insert(p);
    for (uint16_t p : PortFallback::kHttp)
        seen.insert(p);
    CHECK_EQ(seen.size(), PortFallback::kHttps.size() + PortFallback::kHttp.size());

    // The ranges the ladder replaces as first choice — kept in the code as a
    // last resort — are exactly what isStable() must reject.
    CHECK(!PortFallback::isStable(49443)); // old HTTPS first fallback
    CHECK(!PortFallback::isStable(49080)); // old HTTP first fallback
    CHECK(!PortFallback::isStable(32768)); // first Linux ephemeral port
    CHECK(!PortFallback::isStable(60999)); // last Linux ephemeral port
    CHECK(!PortFallback::isStable(443));   // privileged
    CHECK(!PortFallback::isStable(0));

    // A port between the two dynamic ranges is stable on Windows but not on
    // Linux, so it must not qualify either.
    CHECK(!PortFallback::isStable(48443)); // the --dev port, Linux-ephemeral

    CHECK(PortFallback::isStable(1024));
    CHECK(PortFallback::isStable(32767));
}
