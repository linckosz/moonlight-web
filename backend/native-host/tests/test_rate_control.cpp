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

    SECTION("RateControl — the still-screen budget is six times the stream, capped");
    {
        CHECK_EQ(stillBitrateKbps(20000), 20000 * kStillBoost);
        CHECK_EQ(stillBitrateKbps(55000), 55000 * kStillBoost);
        CHECK_EQ(stillBitrateKbps(100000), kStillMaxKbps);
        CHECK_EQ(stillBitrateKbps(0), 0);
        CHECK_EQ(stillBitrateKbps(-5), -5);
    }
}
