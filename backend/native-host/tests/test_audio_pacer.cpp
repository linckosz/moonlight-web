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

#include "audio/AudioPacer.h"
#include "native_test_framework.h"

#include <vector>

using mw::native::audio::AudioPacer;

namespace {

/// `frames` stereo samples where every float carries `value`.
std::vector<float> tone(size_t frames, float value)
{
    return std::vector<float>(frames * AudioPacer::kChannels, value);
}

bool allEqual(const std::vector<float>& v, float value)
{
    for (float f : v)
        if (f != value) return false;
    return true;
}

} // namespace

void run_audio_pacer_tests()
{
    SECTION("AudioPacer — one frame due every 5 ms, none before the clock starts");
    {
        AudioPacer pacer(4);
        CHECK_EQ(pacer.dueFrames(1'000'000), 0); // not started: nothing owed
        pacer.start(1'000'000);
        CHECK_EQ(pacer.dueFrames(1'004'999), 0);
        CHECK_EQ(pacer.dueFrames(1'005'000), 1);
        std::vector<float> out(AudioPacer::kFrameFloats, 1.0f);
        pacer.pop(out.data());
        CHECK_EQ(pacer.nextDueUs(), int64_t(1'010'000));
        // 17 ms after the start: the frames due at 10 and 15 ms are both owed.
        CHECK_EQ(pacer.dueFrames(1'017'000), 2);
    }

    SECTION("AudioPacer — a 10 ms WASAPI burst becomes two 5 ms frames, in order");
    {
        AudioPacer pacer(4);
        pacer.start(0);
        std::vector<float> a = tone(240, 0.25f);
        std::vector<float> b = tone(240, 0.5f);
        std::vector<float> burst;
        burst.insert(burst.end(), a.begin(), a.end());
        burst.insert(burst.end(), b.begin(), b.end());
        pacer.push(burst.data(), 480);
        CHECK_EQ(pacer.queuedFrames(), size_t(2));

        std::vector<float> out(AudioPacer::kFrameFloats);
        CHECK(pacer.pop(out.data()));
        CHECK(allEqual(out, 0.25f));
        CHECK(pacer.pop(out.data()));
        CHECK(allEqual(out, 0.5f));
        CHECK_EQ(pacer.queuedFrames(), size_t(0));
        CHECK_EQ(pacer.underruns(), int64_t(0));
    }

    SECTION("AudioPacer — an empty queue sends silence and counts the underrun");
    {
        AudioPacer pacer(4);
        pacer.start(0);
        std::vector<float> out(AudioPacer::kFrameFloats, 9.0f);
        CHECK(!pacer.pop(out.data()));
        CHECK(allEqual(out, 0.0f));
        CHECK_EQ(pacer.underruns(), int64_t(1));
        // A partial frame is kept for the next tick, not sent as a hole.
        std::vector<float> half = tone(120, 0.7f);
        pacer.push(half.data(), 120);
        CHECK(!pacer.pop(out.data()));
        CHECK_EQ(pacer.underruns(), int64_t(2));
        pacer.push(half.data(), 120);
        CHECK(pacer.pop(out.data()));
        CHECK(allEqual(out, 0.7f));
    }

    SECTION("AudioPacer — the queue cap drops the OLDEST frames, whole frames only");
    {
        AudioPacer pacer(2); // 10 ms of latency at most
        pacer.start(0);
        std::vector<float> one = tone(240, 1.0f);
        std::vector<float> two = tone(240, 2.0f);
        std::vector<float> three = tone(240, 3.0f);
        pacer.push(one.data(), 240);
        pacer.push(two.data(), 240);
        pacer.push(three.data(), 240);
        CHECK_EQ(pacer.queuedFrames(), size_t(2));
        CHECK_EQ(pacer.droppedFrames(), int64_t(1));
        std::vector<float> out(AudioPacer::kFrameFloats);
        pacer.pop(out.data());
        CHECK(allEqual(out, 2.0f)); // the oldest (1.0) is what went
        pacer.pop(out.data());
        CHECK(allEqual(out, 3.0f));
    }

    SECTION("AudioPacer — silence pushed by the endpoint is queued like any sample");
    {
        AudioPacer pacer(4);
        pacer.start(0);
        pacer.pushSilence(240);
        std::vector<float> out(AudioPacer::kFrameFloats, 5.0f);
        CHECK(pacer.pop(out.data())); // real (silent) data, not an underrun
        CHECK(allEqual(out, 0.0f));
        CHECK_EQ(pacer.underruns(), int64_t(0));
    }

    SECTION("AudioPacer — a thread asleep for a second re-anchors instead of bursting");
    {
        AudioPacer pacer(4);
        pacer.start(0);
        // Woke up 1 s late: 200 frames owed on paper, 4 (the queue depth) in
        // fact, and the clock now runs from the present.
        CHECK_EQ(pacer.dueFrames(1'000'000), 4);
        CHECK_EQ(pacer.reanchors(), int64_t(1));
        std::vector<float> out(AudioPacer::kFrameFloats);
        for (int i = 0; i < 4; ++i)
            pacer.pop(out.data());
        // The next frame is due one period after the wake-up, not 995 ms ago.
        CHECK_EQ(pacer.nextDueUs(), int64_t(1'005'000));
        CHECK_EQ(pacer.dueFrames(1'004'000), 0);
        CHECK_EQ(pacer.dueFrames(1'005'000), 1);
    }
}
