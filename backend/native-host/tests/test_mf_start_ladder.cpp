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

#include "native_test_framework.h"

#include "encode/RateControl.h"
#include "encode/windows/MfStartLadder.h"

#include <string>

using namespace mw::native::encode;

// What a hardware Media Foundation transform is asked again when it refuses to
// start (MfStartLadder.h, A121). Pure: every platform runs it.

void run_mf_start_ladder_tests()
{
    SECTION("Media Foundation — a refused start asked again: 60 fps, then a wider VBV");

    // The report's case: 120 fps, the one-frame rule.
    {
        const auto ladder = mfStartLadder(120, 0);
        CHECK_EQ(ladder.size(), static_cast<size_t>(3));
        CHECK(ladder[0].declaredFps == 120 && ladder[0].vbvFrames == 0);
        CHECK(std::string(ladder[0].change).empty());
        CHECK(ladder[1].declaredFps == 60 && ladder[1].vbvFrames == 0);
        CHECK(ladder[2].declaredFps == 60 && ladder[2].vbvFrames == kMfRetryVbvFrames);
        CHECK(!std::string(ladder[2].change).empty());
    }
    // A stream at 60 fps or less keeps its cadence: only the buffer is tried.
    {
        const auto at60 = mfStartLadder(60, 0);
        CHECK_EQ(at60.size(), static_cast<size_t>(2));
        CHECK(at60[1].declaredFps == 60 && at60[1].vbvFrames == kMfRetryVbvFrames);
        const auto at30 = mfStartLadder(30, 0);
        CHECK(at30.size() == 2 && at30[1].declaredFps == 30);
    }
    // A VBV the bench already widened is not narrowed, nor tried again.
    {
        const auto wide = mfStartLadder(144, 8);
        CHECK_EQ(wide.size(), static_cast<size_t>(2));
        CHECK(wide[1].declaredFps == 60 && wide[1].vbvFrames == 8);
        CHECK_EQ(mfStartLadder(60, 4).size(), static_cast<size_t>(1));
    }
    // An unknown rate is the default 60.
    CHECK_EQ(mfStartLadder(0, 0)[0].declaredFps, 60);

    // The bitrate a slower-declared transform is given: what the wire carries
    // comes out as asked — 15 Mbit/s at 120 fps asked as 7.5 for "60 fps".
    CHECK_EQ(mfDeclaredKbps(15000, 60, 120), 7500);
    CHECK_EQ(mfDeclaredKbps(15000, 120, 120), 15000);
    CHECK_EQ(mfDeclaredKbps(15000, 60, 144), 6250);
    CHECK_EQ(mfDeclaredKbps(1, 60, 240), 1);
    CHECK_EQ(mfDeclaredKbps(15000, 0, 120), 15000);
    // The VBV is sized at the stream's rate (MfEncoder::init): the report's
    // 30 KB at 120 fps stays 30 KB declared at 60, and the last rung is
    // deeper, four frames of the stream.
    {
        const auto ladder = mfStartLadder(120, 0);
        const uint32_t bits = 15000u * 1000u;
        CHECK_EQ(vbvBits(bits, 120, ladder[0].vbvFrames) / 8, static_cast<uint32_t>(31250));
        CHECK_EQ(vbvBits(bits, 120, ladder[1].vbvFrames), vbvBits(bits, 120, ladder[0].vbvFrames));
        CHECK_EQ(vbvBits(bits, 120, ladder[2].vbvFrames) / 8, static_cast<uint32_t>(62500));
    }
}
