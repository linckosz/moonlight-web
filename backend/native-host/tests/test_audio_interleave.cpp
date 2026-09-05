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

#include "audio/AudioInterleave.h"
#include "native_test_framework.h"

#include <vector>

using mw::native::audio::interleaveToStereo;

void run_audio_interleave_tests()
{
    SECTION("AudioInterleave — two planes become one interleaved stereo run");
    {
        const float left[4] = {1.0f, 2.0f, 3.0f, 4.0f};
        const float right[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
        const float* planes[2] = {left, right};
        std::vector<float> out(8, 99.0f);
        interleaveToStereo(planes, 2, 4, out.data());
        CHECK_EQ(out[0], 1.0f);
        CHECK_EQ(out[1], -1.0f);
        CHECK_EQ(out[6], 4.0f);
        CHECK_EQ(out[7], -4.0f);
    }

    SECTION("AudioInterleave — a mono source plays in BOTH ears, not just the left");
    {
        // The bug this guards against is silent and blamed on the headphones:
        // a mono host panned hard left sounds broken to the player.
        const float mono[3] = {0.25f, 0.5f, 0.75f};
        const float* planes[1] = {mono};
        std::vector<float> out(6, 99.0f);
        interleaveToStereo(planes, 1, 3, out.data());
        CHECK_EQ(out[0], 0.25f);
        CHECK_EQ(out[1], 0.25f);
        CHECK_EQ(out[4], 0.75f);
        CHECK_EQ(out[5], 0.75f);
    }

    SECTION("AudioInterleave — a null plane is silence, not a crash");
    {
        const float left[2] = {1.0f, 2.0f};
        const float* planes[2] = {left, nullptr};
        std::vector<float> out(4, 99.0f);
        interleaveToStereo(planes, 2, 2, out.data());
        // A missing right channel falls back on the left rather than on a read
        // through null: one usable ear beats a segfault in a capture callback.
        CHECK_EQ(out[1], 1.0f);
        CHECK_EQ(out[3], 2.0f);

        const float* none[2] = {nullptr, nullptr};
        std::vector<float> quiet(4, 99.0f);
        interleaveToStereo(none, 2, 2, quiet.data());
        CHECK_EQ(quiet[0], 0.0f);
        CHECK_EQ(quiet[3], 0.0f);
    }

    SECTION("AudioInterleave — nothing to do leaves the buffer alone");
    {
        const float left[1] = {1.0f};
        const float* planes[1] = {left};
        std::vector<float> out(2, 99.0f);
        interleaveToStereo(planes, 1, 0, out.data());
        CHECK_EQ(out[0], 99.0f);
        interleaveToStereo(nullptr, 0, 1, out.data());
        // No planes at all: silence, and still no read through null.
        CHECK_EQ(out[0], 0.0f);
        CHECK_EQ(out[1], 0.0f);
        interleaveToStereo(planes, 1, 1, nullptr); // must simply return
        CHECK(true);
    }
}
