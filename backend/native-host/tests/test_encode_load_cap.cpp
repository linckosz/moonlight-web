/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "encode/EncodeLoadCap.h"
#include "native_test_framework.h"

#include <cstdio>

using namespace mw::native::encode;

namespace {

constexpr int64_t kMs = 1000;
/// One frame at 60 fps.
constexpr int64_t kInterval = 16667;

/// Feed `seconds` worth of frames that each took `encodeMs`, and say whether the
/// cap asked for a rebuild along the way.
bool feed(EncodeLoadCap& cap, double encodeMs, int seconds, int64_t& nowUs)
{
    bool changed = false;
    const int frames = 60 * seconds;
    for (int i = 0; i < frames; ++i) {
        nowUs += kInterval;
        if (cap.note(static_cast<int64_t>(encodeMs * kMs), kInterval, nowUs)) changed = true;
    }
    return changed;
}

} // namespace

void run_encode_load_cap_tests()
{
    SECTION("EncodeLoadCap — a comfortable encoder is left alone");
    {
        EncodeLoadCap cap;
        int64_t now = 0;
        cap.start(now);
        // 3 ms a frame is what a hardware encoder costs: nothing to do, ever.
        CHECK(!feed(cap, 3.0, 10, now));
        CHECK_EQ(cap.percent(), 100);
        CHECK_EQ(cap.changes, 0);
    }

    SECTION("EncodeLoadCap — an encoder at the frame interval gives up pixels, not frames");
    {
        EncodeLoadCap cap;
        int64_t now = 0;
        cap.start(now);
        // The measured N95 at 1080p: 39,7 ms a frame against a 16,7 ms
        // interval. One window is enough to step down — and one window is one
        // step, never two.
        CHECK(feed(cap, 39.7, 1, now));
        CHECK_EQ(cap.percent(), 75);
        CHECK(feed(cap, 33.5, 1, now)); // 720p-ish, still too slow
        CHECK_EQ(cap.percent(), 50);
        // The floor holds: a machine that cannot keep up at half size is not
        // helped by a quarter, and the picture would stop being worth sending.
        CHECK(!feed(cap, 19.8, 10, now));
        CHECK_EQ(cap.percent(), 50);
        CHECK_EQ(cap.changes, 2);
    }

    SECTION("EncodeLoadCap — one slow second is not enough to resize the picture");
    {
        EncodeLoadCap cap;
        int64_t now = 0;
        cap.start(now);
        // A burst: ten hard frames inside an otherwise easy window. The mean
        // over the window stays under the threshold, so nothing moves — a
        // keyframe and a decoder reconfiguration are too expensive to spend on
        // a stutter. (Ten × 40 ms plus fifty × 2 ms is 8,3 ms of mean against a
        // 13,3 ms threshold.)
        for (int i = 0; i < 10; ++i) {
            now += kInterval;
            CHECK(!cap.note(40 * kMs, kInterval, now));
        }
        for (int i = 0; i < 50; ++i) {
            now += kInterval;
            CHECK(!cap.note(2 * kMs, kInterval, now));
        }
        CHECK_EQ(cap.percent(), 100);
    }

    SECTION("EncodeLoadCap — it climbs back, but only after sustained comfort");
    {
        EncodeLoadCap cap;
        int64_t now = 0;
        cap.start(now);
        CHECK(feed(cap, 39.7, 1, now));
        CHECK_EQ(cap.percent(), 75);

        // Three comfortable windows are not enough: going back up is the
        // change that can start the cycle again, so it needs four.
        CHECK(!feed(cap, 4.0, 3, now));
        CHECK_EQ(cap.percent(), 75);
        // The fourth does it.
        CHECK(feed(cap, 4.0, 1, now));
        CHECK_EQ(cap.percent(), 100);
    }

    SECTION("EncodeLoadCap — comfort interrupted starts the climb over");
    {
        EncodeLoadCap cap;
        int64_t now = 0;
        cap.start(now);
        CHECK(feed(cap, 39.7, 1, now));
        CHECK_EQ(cap.percent(), 75);
        // Three comfortable windows, then a middling one — neither slow enough
        // to step down nor fast enough to count as comfort. That is the dead
        // band between the two thresholds, and it breaks the run.
        CHECK(!feed(cap, 4.0, 3, now));
        CHECK(!feed(cap, 10.0, 1, now)); // 60 % of the interval
        CHECK_EQ(cap.percent(), 75);
        // The four have to be found again from here: three still do nothing.
        CHECK(!feed(cap, 4.0, 3, now));
        CHECK_EQ(cap.percent(), 75);
        CHECK(feed(cap, 4.0, 1, now));
        CHECK_EQ(cap.percent(), 100);
    }

    SECTION("EncodeLoadCap — comfort at the small size is not proof for the large one");
    {
        EncodeLoadCap cap;
        int64_t now = 0;
        cap.start(now);
        CHECK(feed(cap, 39.7, 1, now));
        CHECK_EQ(cap.percent(), 75);
        // Down to the floor, where the pixel ratio to the rung above is 2,25.
        CHECK(feed(cap, 33.5, 1, now));
        CHECK_EQ(cap.percent(), 50);
        // 5,5 ms at half size is comfortable (< 5,8 ms) and predicts 12,4 ms
        // at 75 %: over the 10,8 ms climb line. It stays down, for ever.
        CHECK(!feed(cap, 5.5, 30, now));
        CHECK_EQ(cap.percent(), 50);
        // 4 ms predicts 9 ms: that one may climb, after the usual four.
        CHECK(feed(cap, 4.0, 4, now));
        CHECK_EQ(cap.percent(), 75);
    }

    SECTION("EncodeLoadCap — a climb undone at once makes the next one wait twice as long");
    {
        EncodeLoadCap cap;
        int64_t now = 0;
        cap.start(now);
        CHECK(feed(cap, 39.7, 1, now));
        CHECK_EQ(cap.percent(), 75);
        // The reporter's guest: a still desktop costs nothing, the first
        // moving window costs the full price. Four quiet seconds, a climb, and
        // straight back down.
        CHECK(feed(cap, 1.0, 4, now));
        CHECK_EQ(cap.percent(), 100);
        CHECK(feed(cap, 39.7, 1, now));
        CHECK_EQ(cap.percent(), 75);
        // Now eight quiet windows are needed, not four.
        CHECK(!feed(cap, 1.0, 7, now));
        CHECK_EQ(cap.percent(), 75);
        CHECK(feed(cap, 1.0, 1, now));
        CHECK_EQ(cap.percent(), 100);
        // Failing again: sixteen.
        CHECK(feed(cap, 39.7, 1, now));
        CHECK(!feed(cap, 1.0, 15, now));
        CHECK_EQ(cap.percent(), 75);
        CHECK(feed(cap, 1.0, 1, now));
        CHECK_EQ(cap.percent(), 100);
        // A climb that holds through its probation clears the record: after
        // ten easy seconds at full size, a later step down needs four again.
        CHECK(!feed(cap, 1.0, 11, now));
        CHECK(feed(cap, 39.7, 1, now));
        CHECK_EQ(cap.percent(), 75);
        CHECK(feed(cap, 1.0, 4, now));
        CHECK_EQ(cap.percent(), 100);
    }

    SECTION("EncodeLoadCap — no interval, no cap");
    {
        EncodeLoadCap cap;
        int64_t now = 0;
        cap.start(now);
        // A stream with no paced interval has nothing to be late against; the
        // cap must not invent a reason to shrink the picture.
        for (int i = 0; i < 600; ++i) {
            now += kInterval;
            CHECK(!cap.note(80 * kMs, 0, now));
        }
        CHECK_EQ(cap.percent(), 100);
    }

    SECTION("EncodeLoadCap — the scaled size stays even, and never collapses");
    {
        CHECK_EQ(EncodeLoadCap::scaled(1920, 100), 1920);
        CHECK_EQ(EncodeLoadCap::scaled(1920, 75), 1440);
        CHECK_EQ(EncodeLoadCap::scaled(1080, 75), 810);
        CHECK_EQ(EncodeLoadCap::scaled(1920, 50), 960);
        CHECK_EQ(EncodeLoadCap::scaled(1080, 50), 540);
        // 4:2:0 needs even dimensions; an odd result would be rejected by every
        // encoder in this engine.
        CHECK_EQ(EncodeLoadCap::scaled(1366, 75) % 2, 0);
        CHECK_EQ(EncodeLoadCap::scaled(1050, 75) % 2, 0);
        // A degenerate input must not produce a zero-sized picture.
        CHECK_EQ(EncodeLoadCap::scaled(2, 50), 2);
    }
}
