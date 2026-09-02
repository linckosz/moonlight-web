/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "mw/native/StageStats.h"

#include <cstdint>
#include <string>

using namespace mw::native;

// The histogram is what every latency figure of the native path is read
// through. If its geometry is wrong, every p99 in every log is wrong in the
// same direction and nobody notices — so the geometry is checked directly.

void run_stage_stats_tests()
{
    SECTION("StageStats — histogram geometry");
    {
        // Each octave has eight buckets, and a value lands in the bucket whose
        // range contains it: the upper edge of its bucket is the first value
        // that lands in the next one.
        //
        // Below 8 µs an eighth of an octave is less than a microsecond, so
        // several buckets are unreachable with integer durations; there the
        // edge only has to land strictly above its own bucket.
        for (int b = 0; b < LatencyHistogram::kBuckets - 1; ++b) {
            const int64_t edge = LatencyHistogram::upperEdgeUs(b);
            if (b >= 3 * LatencyHistogram::kPerOctave)
                CHECK(LatencyHistogram::bucketOf(edge) == b + 1);
            else
                CHECK(LatencyHistogram::bucketOf(edge) > b);
            CHECK(LatencyHistogram::bucketOf(edge - 1) <= b);
        }
        // Landmarks, in µs.
        CHECK_EQ(LatencyHistogram::bucketOf(0), 0);
        CHECK_EQ(LatencyHistogram::bucketOf(1), 0);
        CHECK_EQ(LatencyHistogram::bucketOf(8), 3 * 8); // 2^3
        CHECK_EQ(LatencyHistogram::bucketOf(15), 3 * 8 + 7);
        CHECK_EQ(LatencyHistogram::bucketOf(1024), 10 * 8);        // 2^10
        CHECK_EQ(LatencyHistogram::bucketOf(1000000), 19 * 8 + 7); // 1 s: 2^19·(1+7/8)…
        // …and past the top the last bucket absorbs everything.
        CHECK_EQ(LatencyHistogram::bucketOf(int64_t(1) << 40), LatencyHistogram::kBuckets - 1);
    }

    SECTION("StageStats — percentiles overstate, never understate");
    {
        LatencyHistogram h;
        CHECK_EQ(h.percentileUs(0.99), int64_t(0));
        CHECK_EQ(h.meanUs(), int64_t(0));

        // 1000 samples, 1..1000 µs: the true p50 is 500, p95 is 950, p99 is 990.
        for (int64_t v = 1; v <= 1000; ++v)
            h.add(v);
        CHECK_EQ(h.count(), int64_t(1000));
        CHECK_EQ(h.meanUs(), int64_t(500));
        CHECK_EQ(h.maxUs(), int64_t(1000));

        const int64_t p50 = h.percentileUs(0.50);
        const int64_t p95 = h.percentileUs(0.95);
        const int64_t p99 = h.percentileUs(0.99);
        // At or above the true value, and within one bucket (12.5 % of the
        // octave's base, so ≤ 9.4 % of the value) of it.
        CHECK(p50 >= 500 && p50 <= 560);
        CHECK(p95 >= 950 && p95 <= 1040);
        CHECK(p99 >= 990 && p99 <= 1000); // clamped to the maximum
        CHECK_EQ(h.percentileUs(1.0), int64_t(1000));

        // A single sample: every percentile IS that sample's bucket edge, and
        // never above the sample itself.
        LatencyHistogram one;
        one.add(3460);
        CHECK_EQ(one.percentileUs(0.5), int64_t(3460));
        CHECK_EQ(one.percentileUs(0.99), int64_t(3460));

        // A distribution with a fat tail: 99 fast frames and one slow one. Over
        // 100 samples the p99 is the 99th, still a fast frame; only the maximum
        // sees the slow one. Over 1000 samples with ten slow ones, p99 does.
        LatencyHistogram tail;
        for (int i = 0; i < 99; ++i)
            tail.add(200);
        tail.add(50000);
        CHECK(tail.percentileUs(0.95) < 300);
        CHECK(tail.percentileUs(0.99) < 300);
        CHECK_EQ(tail.percentileUs(1.0), int64_t(50000));
        CHECK_EQ(tail.maxUs(), int64_t(50000));

        LatencyHistogram tail2;
        for (int i = 0; i < 990; ++i)
            tail2.add(200);
        for (int i = 0; i < 10; ++i)
            tail2.add(50000);
        CHECK(tail2.percentileUs(0.95) < 300);
        CHECK(tail2.percentileUs(0.99) < 300);               // rank 990: the last fast one
        CHECK_EQ(tail2.percentileUs(0.995), int64_t(50000)); // rank 995: slow

        // Negative durations (a clock that stepped) count as zero, not as a
        // crash and not as a huge unsigned value.
        LatencyHistogram neg;
        neg.add(-5);
        CHECK_EQ(neg.count(), int64_t(1));
        CHECK_EQ(neg.maxUs(), int64_t(0));
    }

    SECTION("StageStats — window and session horizons");
    {
        StageStats s;
        s.record(Stage::Encode, 3000);
        s.record(Stage::Encode, 4000);
        s.record(Stage::Total, 6000);

        auto w = s.takeWindow();
        CHECK_EQ(w[static_cast<size_t>(Stage::Encode)].count, int64_t(2));
        CHECK_EQ(w[static_cast<size_t>(Stage::Encode)].meanUs, int64_t(3500));
        CHECK_EQ(w[static_cast<size_t>(Stage::Acquire)].count, int64_t(0));

        // Taking the window empties it; the session keeps everything.
        auto w2 = s.takeWindow();
        CHECK_EQ(w2[static_cast<size_t>(Stage::Encode)].count, int64_t(0));
        auto sess = s.session();
        CHECK_EQ(sess[static_cast<size_t>(Stage::Encode)].count, int64_t(2));
        CHECK_EQ(s.sessionFrames(), int64_t(1)); // one Total recorded

        // An out-of-range stage is ignored rather than indexing past the array.
        s.record(Stage::Count, 1);
        CHECK_EQ(s.sessionFrames(), int64_t(1));

        s.reset();
        CHECK_EQ(s.sessionFrames(), int64_t(0));
    }

    SECTION("StageStats — one-line description");
    {
        StageStats s;
        CHECK(StageStats::describe(s.session()).empty());

        s.record(Stage::Acquire, 60);
        s.record(Stage::Encode, 3460);
        s.record(Stage::Total, 4120);
        const std::string line = StageStats::describe(s.session());
        // Stages with no samples are left out; the rest read in pipeline order
        // in milliseconds, with the frame count at the end.
        CHECK(line.find("acquire 0.06/") == 0);
        CHECK(line.find("convert") == std::string::npos);
        CHECK(line.find("encode 3.46/") != std::string::npos);
        CHECK(line.find("total 4.12/") != std::string::npos);
        CHECK(line.find("(n=1)") != std::string::npos);
    }
}
