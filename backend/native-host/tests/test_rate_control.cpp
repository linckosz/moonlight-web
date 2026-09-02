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

#include "encode/RateControl.h"
#include "native_test_framework.h"

using mw::native::encode::kStillBoost;
using mw::native::encode::kStillMaxKbps;
using mw::native::encode::kVbvFrameRateFloor;
using mw::native::encode::LinkOccupancy;
using mw::native::encode::RefineConvergence;
using mw::native::encode::stillBitrateKbps;
using mw::native::encode::vbvBitsPerFrame;

void run_rate_control_tests()
{
    constexpr uint32_t k55Mbps = 55000000u;

    SECTION("RateControl — at or below 60 fps the VBV is exactly one frame");
    {
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 60), k55Mbps / 60);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 30), k55Mbps / 30);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 20), k55Mbps / 20);
    }

    SECTION("RateControl — above 60 fps the budget stops shrinking");
    {
        const uint32_t floor = k55Mbps / static_cast<uint32_t>(kVbvFrameRateFloor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 61), floor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 120), floor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 144), floor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 165), floor);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 240), floor);
        // The plain division AmfEncoder::setBitrate used to do, and what it cost:
        // a budget 2.4× tighter at 144 Hz than the one init() had chosen.
        CHECK(vbvBitsPerFrame(k55Mbps, 144) > 2 * (k55Mbps / 144));
    }

    SECTION("RateControl — an unknown frame rate reads as 60");
    {
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, 0), k55Mbps / 60);
        CHECK_EQ(vbvBitsPerFrame(k55Mbps, -1), k55Mbps / 60);
    }

    SECTION("RateControl — a re-applied bitrate lands on the same VBV as init()");
    {
        // The still-screen burst raises the bitrate and puts it back through
        // setBitrate(); the budget it restores must be the one the session
        // started with, on every encoder, at every refresh rate.
        constexpr int kRates[] = {30, 60, 120, 144, 165, 240};
        for (int fps : kRates) {
            const uint32_t atInit = vbvBitsPerFrame(k55Mbps, fps);
            const uint32_t restored = vbvBitsPerFrame(k55Mbps, fps);
            CHECK_EQ(restored, atInit);
        }
    }

    SECTION("RateControl — the still-screen budget is three times the stream, capped");
    {
        CHECK_EQ(kStillBoost, 3);
        CHECK_EQ(stillBitrateKbps(20000), 20000 * kStillBoost);
        CHECK_EQ(stillBitrateKbps(55000), 55000 * kStillBoost);
        CHECK_EQ(stillBitrateKbps(200000), kStillMaxKbps);
        CHECK_EQ(stillBitrateKbps(0), 0);
        CHECK_EQ(stillBitrateKbps(-5), -5);
    }

    SECTION("RateControl — one boosted pass is at most 50 ms of link, at any bitrate");
    {
        // The multiplier is a latency: the longest one refinement pass can
        // occupy the link, hence the longest a motion frame right behind it
        // can wait. boost / 60 s, so 50 ms — whatever the stream's rate.
        constexpr int kRates[] = {5000, 20000, 55000, 150000};
        for (int kbps : kRates) {
            const uint32_t passBits =
                vbvBitsPerFrame(static_cast<uint32_t>(stillBitrateKbps(kbps)) * 1000u, 144);
            LinkOccupancy link;
            link.sent(0, passBits / 8, kbps);
            CHECK(link.backlogUs(0) <= 50000);
            CHECK(link.backlogUs(0) >= 49000);
            CHECK(!link.drainedAt(49000));
            CHECK(link.drainedAt(50000));
        }
    }

    SECTION("LinkOccupancy — bytes occupy the link for size / rate, back to back");
    {
        LinkOccupancy link;
        CHECK(link.drainedAt(0));
        CHECK_EQ(link.backlogUs(0), 0);

        // 250 KB at 20 Mbps is 100 ms.
        link.sent(1000, 250000, 20000);
        CHECK_EQ(link.backlogUs(1000), 100000);
        CHECK(!link.drainedAt(100999));
        CHECK(link.drainedAt(101000));

        // A second frame while the first is still going queues behind it,
        // not on top of it.
        link.sent(51000, 250000, 20000);
        CHECK_EQ(link.backlogUs(51000), 150000);
        CHECK(link.drainedAt(201000));

        // Once drained, a new frame starts from now, not from the old end.
        link.sent(500000, 25000, 20000);
        CHECK_EQ(link.backlogUs(500000), 10000);
    }

    SECTION("RefineConvergence — a low-bitrate burst ends on two tiny passes");
    {
        // What the 1 Mbps sessions always did: five passes, the last two of
        // almost nothing. Size alone is enough, with or without a QP.
        RefineConvergence conv;
        using V = RefineConvergence::Verdict;
        CHECK(conv.notePass(30000, -1) == V::Continue);
        CHECK(conv.notePass(9000, -1) == V::Continue);
        CHECK(conv.notePass(3000, -1) == V::Continue);
        CHECK(conv.notePass(500, -1) == V::Continue); // 4th pass: first quiet one
        CHECK(conv.notePass(400, -1) == V::Converged);
        CHECK_EQ(conv.passes, 5);
    }

    SECTION("RefineConvergence — under CBR the QP plateau ends it, size never would");
    {
        // 55 Mbps at 165 fps: every pass padded to ~120 KB, QP falling until
        // the residual is gone, then flat. Two flat passes after the minimum.
        RefineConvergence conv;
        using V = RefineConvergence::Verdict;
        CHECK(conv.notePass(120000, 30) == V::Continue);
        CHECK(conv.notePass(120000, 25) == V::Continue);
        CHECK(conv.notePass(120000, 20) == V::Continue);
        CHECK(conv.notePass(120000, 15) == V::Continue);  // still falling
        CHECK(conv.notePass(120000, 15) == V::Continue);  // flat once
        CHECK(conv.notePass(120000, 16) == V::Converged); // rising counts as flat
        CHECK_EQ(conv.passes, 6);
    }

    SECTION("RefineConvergence — a QP that resumes falling resets the streak");
    {
        RefineConvergence conv;
        using V = RefineConvergence::Verdict;
        for (int i = 0; i < 4; i++)
            CHECK(conv.notePass(120000, 30 - i) == V::Continue);
        CHECK(conv.notePass(120000, 27) == V::Continue); // flat once
        CHECK(conv.notePass(120000, 22) == V::Continue); // fell again: streak reset
        CHECK(conv.notePass(120000, 22) == V::Continue); // flat once
        CHECK(conv.notePass(120000, 21) == V::Capped);   // 8th, still falling: the ceiling
    }

    SECTION("RefineConvergence — no QP and big passes run into the cap");
    {
        RefineConvergence conv;
        using V = RefineConvergence::Verdict;
        for (int i = 1; i < RefineConvergence::kMaxPasses; i++)
            CHECK(conv.notePass(120000, -1) == V::Continue);
        CHECK(conv.notePass(120000, -1) == V::Capped);
        CHECK_EQ(conv.passes, RefineConvergence::kMaxPasses);
    }

    SECTION("RefineConvergence — the cap is at most 400 ms of link");
    {
        // Eight passes of one boosted VBV each, at the stream's own rate.
        constexpr int kRates[] = {5000, 20000, 55000};
        for (int kbps : kRates) {
            const uint32_t passBits =
                vbvBitsPerFrame(static_cast<uint32_t>(stillBitrateKbps(kbps)) * 1000u, 60);
            LinkOccupancy link;
            for (int i = 0; i < RefineConvergence::kMaxPasses; i++)
                link.sent(0, passBits / 8, kbps);
            CHECK(link.backlogUs(0) <= 400000);
        }
    }

    SECTION("RefineConvergence — reset forgets everything");
    {
        RefineConvergence conv;
        conv.notePass(120000, 20);
        conv.notePass(120000, 20);
        conv.reset();
        CHECK_EQ(conv.passes, 0);
        CHECK_EQ(conv.quiet, 0);
        CHECK_EQ(conv.lastQp, -1);
    }

    SECTION("LinkOccupancy — an unknown rate models nothing");
    {
        LinkOccupancy link;
        link.sent(0, 1000000, 0);
        link.sent(0, 1000000, -1);
        CHECK(link.drainedAt(0));
    }

    SECTION("LinkOccupancy — CBR motion frames keep the link level");
    {
        // One interval of link time, sent once per interval: the backlog
        // never grows past one frame, so the gate is invisible to motion and
        // the first refinement pass after it waits at most one interval.
        LinkOccupancy link;
        const int kbps = 55000;
        const size_t frameBytes = vbvBitsPerFrame(kbps * 1000u, 60) / 8;
        int64_t now = 0;
        for (int i = 0; i < 600; i++) {
            link.sent(now, frameBytes, kbps);
            CHECK(link.backlogUs(now) <= 16667);
            now += 16667;
        }
        CHECK(link.drainedAt(now));
    }
}
