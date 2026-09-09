/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * Back-pressure measured in time rather than in bytes (SendBacklog.h).
 *
 * The properties that matter are the two failure modes the byte threshold sat
 * between: it must NOT fire on a keyframe that is merely large and drains
 * normally (that path ends in an IDR storm), and it MUST fire on a link that
 * stays backed up (that path ends in half a second of invisible latency).
 */

#include "../src/streaming/SendBacklog.h"

#include "test_framework.h"

namespace {

constexpr size_t kBig = 300 * 1024; // a 1440p HEVC keyframe
constexpr size_t kIdle = 8 * 1024;  // one fragment in flight, healthy stream

} // namespace

void run_send_backlog_tests()
{
    SECTION("SendBacklog — a drained buffer never sheds");
    {
        SendBacklog b;
        // An empty buffer, and a buffer holding a fragment or two, are both
        // "drained": aging from the first byte would call a healthy stream a
        // backlog.
        for (int64_t t = 0; t < 5000; t += 10) {
            CHECK(!b.note(0, t));
            CHECK(!b.note(kIdle, t));
            CHECK(!b.backedUp());
            CHECK(b.ageMs(t) == 0);
        }
    }

    SECTION("SendBacklog — a keyframe burst is free");
    {
        SendBacklog b;
        // A keyframe makes the buffer deep for a moment. On a healthy link it
        // drains well inside the tolerance, and nothing may be dropped for it —
        // this is the case that made the old byte threshold unusable.
        CHECK(!b.note(kBig, 1000));
        CHECK(!b.note(kBig / 2, 1060)); // still going out
        CHECK(!b.note(kIdle, 1120));    // drained: 120 ms, a real 20 Mbit/s figure
        CHECK(!b.backedUp());

        // And the next keyframe starts a fresh window rather than inheriting
        // the previous one's age.
        CHECK(!b.note(kBig, 1130));
        CHECK(b.ageMs(1130) == 0);
        CHECK(!b.note(kBig, 1300)); // 170 ms in, still under tolerance
    }

    SECTION("SendBacklog — a sustained backlog sheds");
    {
        SendBacklog b;
        CHECK(!b.note(kBig, 0)); // window opens, no age yet
        CHECK(!b.note(kBig, SendBacklog::kToleranceMs));
        // Strictly greater than the tolerance is what sheds.
        CHECK(b.note(kBig, SendBacklog::kToleranceMs + 1));
        CHECK(b.backedUp());
        CHECK(b.ageMs(SendBacklog::kToleranceMs + 1) == SendBacklog::kToleranceMs + 1);
        // It keeps shedding while the link stays behind.
        CHECK(b.note(kBig, SendBacklog::kToleranceMs + 500));
    }

    SECTION("SendBacklog — draining closes the window");
    {
        SendBacklog b;
        b.note(kBig, 0);
        CHECK(b.note(kBig, 400)); // shedding
        CHECK(!b.note(0, 410));   // the link caught up
        CHECK(!b.backedUp());
        // A new backlog is judged on its own age, not on the old one.
        CHECK(!b.note(kBig, 420));
        CHECK(!b.note(kBig, 600));
        CHECK(b.note(kBig, 700));
    }

    SECTION("SendBacklog — reset is a fresh start");
    {
        SendBacklog b;
        b.note(kBig, 0);
        CHECK(b.note(kBig, 400));
        b.reset(); // a keyframe got through
        CHECK(!b.backedUp());
        CHECK(!b.note(kBig, 401));
        CHECK(!b.note(kBig, 600));
    }

    SECTION("SendBacklog — a clock going backwards cannot shed");
    {
        SendBacklog b;
        CHECK(!b.note(kBig, 10000));
        // A sample stamped before the one that opened the window must not read
        // as a ten-second backlog.
        CHECK(!b.note(kBig, 9000));
        CHECK(b.ageMs(9000) == 0);
        CHECK(!b.note(kBig, 9100));
        CHECK(b.note(kBig, 9400));
    }
}
