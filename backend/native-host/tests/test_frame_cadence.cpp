/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "core/FrameCadence.h"
#include "native_test_framework.h"

#include <vector>

using mw::native::FrameCadence;

namespace {

/// Drive a cadence with presents at a fixed refresh rate for @p seconds, the
/// way the capture loop does: a present that is admitted is encoded on the spot,
/// a held one is encoded when it falls due, and a newer present replaces it.
/// Returns the encode times, in µs.
struct Sim
{
    std::vector<int64_t> encodedAt;
    std::vector<int64_t> ageUs; // present → encode, per encoded frame
    int presents = 0;
};

Sim simulate(int fps, double refreshHz, double seconds, int64_t startUs = 0)
{
    FrameCadence cadence(fps);
    Sim sim;
    const double presentStep = 1e6 / refreshHz;
    int64_t heldPresentUs = 0;
    for (double t = 0; t < seconds * 1e6; t += presentStep) {
        const int64_t presentUs = startUs + static_cast<int64_t>(t);
        // The loop wakes for the due time BEFORE the next present when a
        // picture is held and its wait ends first.
        if (cadence.holding()) {
            const int64_t dueAt = presentUs + cadence.waitUs(presentUs);
            if (dueAt <= presentUs) {
                cadence.release(dueAt);
                sim.encodedAt.push_back(dueAt);
                sim.ageUs.push_back(dueAt - heldPresentUs);
            }
        }
        sim.presents++;
        if (cadence.admit(presentUs)) {
            sim.encodedAt.push_back(presentUs);
            sim.ageUs.push_back(0);
        } else {
            heldPresentUs = presentUs;
        }
    }
    return sim;
}

} // namespace

void run_frame_cadence_tests()
{
    SECTION("FrameCadence — zero means every present");
    {
        FrameCadence off(0);
        CHECK(!off.enabled());
        for (int i = 0; i < 10; ++i)
            CHECK(off.admit(i * 6060));
        CHECK(!off.holding());
        CHECK_EQ(off.superseded(), int64_t(0));

        FrameCadence negative(-5);
        CHECK(!negative.enabled());
        CHECK(negative.admit(0));
    }

    SECTION("FrameCadence — 165 Hz presents into a 60 fps stream");
    {
        // Sub-microsecond drift is not what is being tested: a 60 fps interval
        // is 16666 µs and the presents step 6060.6.
        const Sim sim = simulate(60, 165.0, 10.0);
        // 165 presents/s in, 60 frames/s out: a present within the slack of a
        // due time goes through early, but the grid advances by its interval
        // rather than restarting on it, so the rate holds — within a frame
        // either way for the edges of the run.
        CHECK(sim.presents >= 1649 && sim.presents <= 1651);
        CHECK(sim.encodedAt.size() >= 599 && sim.encodedAt.size() <= 601);

        // A regular cadence: every interval between two encodes is one stream
        // interval, give or take the slack that lets a nearly-due present through.
        int64_t minGap = 1 << 30, maxGap = 0;
        for (size_t i = 1; i < sim.encodedAt.size(); ++i) {
            const int64_t gap = sim.encodedAt[i] - sim.encodedAt[i - 1];
            if (gap < minGap) minGap = gap;
            if (gap > maxGap) maxGap = gap;
        }
        CHECK(minGap >= 16666 - FrameCadence::kSlackUs);
        CHECK(maxGap <= 16666 + FrameCadence::kSlackUs);

        // The frame encoded at each due time is the LAST present before it, so
        // no encoded picture is older than one present interval (6.06 ms) plus
        // the slack.
        int64_t maxAge = 0;
        for (int64_t age : sim.ageUs)
            if (age > maxAge) maxAge = age;
        CHECK(maxAge <= 6061 + FrameCadence::kSlackUs);
        CHECK(maxAge > 0); // something WAS held, this was not a pass-through
    }

    SECTION("FrameCadence — a display slower than the stream is never throttled");
    {
        // 60 Hz presents into a 120 fps stream: every present is due when it
        // arrives, nothing is held, nothing is superseded.
        const Sim sim = simulate(120, 60.0, 5.0);
        CHECK_EQ(sim.encodedAt.size(), size_t(sim.presents));
        for (int64_t age : sim.ageUs)
            CHECK_EQ(age, int64_t(0));
    }

    SECTION("FrameCadence — the last present wins, and the loser is counted");
    {
        FrameCadence c(60); // 16666 µs
        CHECK(c.admit(0));  // first frame goes straight through
        CHECK(!c.admit(6000));
        CHECK(c.holding());
        CHECK_EQ(c.superseded(), int64_t(0));
        CHECK(!c.admit(12000)); // replaces the one from 6000
        CHECK_EQ(c.superseded(), int64_t(1));
        CHECK(!c.due(15000));
        CHECK_EQ(c.waitUs(15000), int64_t(1666));
        CHECK(c.due(16666));
        c.release(16700);
        CHECK(!c.holding());
        // The grid advanced by one interval from where it was, not from the
        // slightly late release.
        CHECK_EQ(c.waitUs(16700), int64_t(33332 - 16700));
    }

    SECTION("FrameCadence — a present within the slack of due goes through now");
    {
        FrameCadence c(60);
        CHECK(c.admit(0));
        // Due at 16666; a present at 15800 is within kSlackUs of it.
        CHECK(c.admit(15800));
        CHECK(!c.holding());
        // …and the grid restarted on it.
        CHECK_EQ(c.waitUs(15800), int64_t(16666));
    }

    SECTION("FrameCadence — a present that beats the wake-up keeps the grid");
    {
        // Held picture, due at 16666. A present at 16400 (within the slack) or
        // at 17500 (the loop had not woken yet) goes through in its place; the
        // held one is counted as not carried, and the grid ADVANCES to 33332
        // rather than restarting on the present — otherwise every such present
        // would push the next due time back and the rate would sag below the
        // setting (measured: 56 fps for 60 on a 165 Hz display).
        FrameCadence early(60);
        CHECK(early.admit(0));
        CHECK(!early.admit(6000));
        CHECK(early.admit(16400));
        CHECK(!early.holding());
        CHECK_EQ(early.superseded(), int64_t(1));
        CHECK_EQ(early.waitUs(16400), int64_t(33332 - 16400));

        FrameCadence late(60);
        CHECK(late.admit(0));
        CHECK(!late.admit(12000));
        CHECK(late.admit(17500));
        CHECK_EQ(late.superseded(), int64_t(1));
        CHECK_EQ(late.waitUs(17500), int64_t(33332 - 17500));
    }

    SECTION("FrameCadence — after a still screen the first change is immediate");
    {
        FrameCadence c(60);
        CHECK(c.admit(0));
        // Two seconds of nothing, then a present: not held for a tick of a grid
        // nobody was keeping.
        CHECK(c.admit(2000000));
        CHECK(!c.holding());
        CHECK_EQ(c.waitUs(2000000), int64_t(16666));
    }

    SECTION("FrameCadence — a missed interval re-anchors instead of bursting");
    {
        FrameCadence c(60);
        CHECK(c.admit(0));
        CHECK(!c.admit(6000));
        // The loop was stalled for 50 ms past the due time (16666).
        c.release(66666);
        // The next due is one interval from now, not three overdue ticks.
        CHECK_EQ(c.waitUs(66666), int64_t(16666));
    }

    SECTION("FrameCadence — drop forgets the held picture");
    {
        FrameCadence c(60);
        CHECK(c.admit(0));
        CHECK(!c.admit(6000));
        c.drop();
        CHECK(!c.holding());
        CHECK(!c.due(20000));
    }
}
