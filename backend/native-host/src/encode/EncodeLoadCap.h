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

#pragma once

#include <cstdint>

// When the CPU cannot encode fast enough, give up PIXELS — never the frame rate.
//
// ── The decision this encodes, and the measurement behind it ────────────────
//
// On a machine with no hardware encoder the software one is the whole cost of a
// frame. Measured on an N95 (four Alder Lake-N cores, OpenH264, real moving
// content — bench §8i):
//
//     1920×1080   encode 39,7 ms mean   →  24,7 fps reachable
//     1280×720    encode 33,5 ms        →  29,1 fps
//      960×540    encode 19,8 ms        →  48,8 fps
//
// Two things fall out of that table. Fewer pixels do NOT buy proportionally
// less time — four times fewer pixels bought two times less — because part of
// the cost is per frame rather than per pixel. And no size reaches 60 fps on
// that machine, so this is not a cure: it moves the ceiling.
//
// Which of the two ceilings to hit was a product decision, taken by Bruno on
// 08/09/2026: **the resolution gives way before the frame rate**, because the
// encode time IS the latency. A synchronous pipeline with one frame in flight
// adds its encode duration to every frame that goes out; halving the picture
// halves that. A sharp picture 40 ms late is worse, for the player this project
// is built for, than a softer one 20 ms late.
//
// ── Why a ladder and not a continuous scale ─────────────────────────────────
//
// Every change costs a keyframe and makes the client reconfigure its decoder.
// A continuous controller would spend its life doing that. Three rungs, wide
// hysteresis, and a floor: below half the linear size the picture stops being
// worth streaming, and a machine that still cannot keep up at 540p is one where
// the honest answer is "this is as good as it gets".

namespace mw::native::encode {

/// Watches encode times and says what fraction of the full picture to encode.
///
/// Pure: fed one number per frame and a clock, it decides. That is what lets
/// the policy — the part a user would otherwise have to tune by hand — be
/// tested without an encoder, a GPU or a display.
struct EncodeLoadCap
{
    /// Rungs, coarsest last. 75 % of the linear size is 56 % of the pixels,
    /// 50 % is a quarter — the two steps that matter between "full" and "as
    /// small as this is allowed to get".
    static constexpr int kRungs[3] = {100, 75, 50};

    /// Step down when the mean encode time of a window exceeds this share of
    /// the frame interval. Not 100 %: at parity the encoder is already the
    /// whole budget, and every jitter spike is a dropped frame.
    static constexpr int kDownPercentOfInterval = 80;

    /// Step back up only well under the interval, so a machine sitting near the
    /// threshold does not oscillate. The gap between the two is the hysteresis.
    static constexpr int kUpPercentOfInterval = 35;

    /// Everything is judged over one-second windows: long enough that a single
    /// stutter cannot resize the picture, short enough to react.
    static constexpr int64_t kWindowUs = 1000 * 1000;

    /// Down acts on ONE such window — falling behind is the failure this exists
    /// to prevent, and a second window of confirmation is a second of the
    /// latency we are removing. Up needs four CONSECUTIVE ones, because going
    /// back up is the change that can start the cycle again; a single window in
    /// between that is neither slow nor comfortable breaks the run.
    static constexpr int kUpWindows = 4;

    /// Where we are on the ladder.
    int rung = 0;
    /// How often it moved, for the session log.
    int changes = 0;

    int64_t windowStartUs = 0;
    int64_t encodeSumUs = 0;
    int frames = 0;
    /// Consecutive comfortable windows. Reset by anything else.
    int comfortWindows = 0;

    void start(int64_t nowUs)
    {
        rung = 0;
        changes = 0;
        comfortWindows = 0;
        resetWindow(nowUs);
    }

    /// The share of the full picture to encode right now, in percent.
    int percent() const { return kRungs[rung]; }

    /// Feed one encoded frame. Returns true when the caller must rebuild the
    /// pipeline at the new percent() — which costs a keyframe, so it is said
    /// once and only when the evidence is in.
    ///
    /// @param encodeUs how long the encoder took on this frame.
    /// @param intervalUs one frame at the stream's rate. Zero disables the cap
    ///        (an unpaced stream has no interval to be late against).
    bool note(int64_t encodeUs, int64_t intervalUs, int64_t nowUs)
    {
        if (intervalUs <= 0) return false;
        encodeSumUs += encodeUs;
        ++frames;

        if (nowUs - windowStartUs < kWindowUs) return false;
        if (frames <= 0) {
            resetWindow(nowUs);
            return false;
        }

        const int64_t mean = encodeSumUs / frames;
        const int64_t downLimit = intervalUs * kDownPercentOfInterval / 100;
        const int64_t upLimit = intervalUs * kUpPercentOfInterval / 100;
        constexpr int kLastRung = static_cast<int>(sizeof(kRungs) / sizeof(kRungs[0])) - 1;
        resetWindow(nowUs);

        if (mean > downLimit) {
            comfortWindows = 0;
            // At the floor there is nothing left to give: a machine that
            // cannot keep up at half the linear size is not helped by a
            // quarter, and the picture would stop being worth sending.
            if (rung >= kLastRung) return false;
            ++rung;
            ++changes;
            return true;
        }

        if (mean < upLimit && rung > 0) {
            if (++comfortWindows < kUpWindows) return false;
            comfortWindows = 0;
            --rung;
            ++changes;
            return true;
        }

        // Neither slow nor comfortable — the dead band between the two
        // thresholds. It is the hysteresis: it breaks a run of comfort without
        // costing a step down.
        if (mean >= upLimit) comfortWindows = 0;
        return false;
    }

    /// Apply the rung to a size, keeping it even (every encoder here wants even
    /// dimensions, and 4:2:0 chroma needs them).
    static int scaled(int full, int percent)
    {
        const int v = full * percent / 100;
        return v < 2 ? 2 : (v & ~1);
    }

private:
    void resetWindow(int64_t nowUs)
    {
        windowStartUs = nowUs;
        encodeSumUs = 0;
        frames = 0;
    }
};

} // namespace mw::native::encode
