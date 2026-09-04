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

namespace mw::native {

/// The stream's cadence, chosen against the CLIENT's refresh rate.
///
/// ── The beat ────────────────────────────────────────────────────────────────
///
/// A client that presents on its vsync — tearing off, or a browser that cannot
/// tear — paints one frame per refresh at most, and only at the refresh. A
/// 60 fps stream on a 144 Hz screen then lands on ticks 2.4 refreshes apart:
/// some frames stay up for two refreshes, some for three, and the eye reads
/// the alternation as judder although not one frame was lost. The same stream
/// on a 120 Hz screen is perfectly regular — 60 divides 120.
///
/// So when the client says it presents on vsync, the stream runs at an INTEGER
/// DIVISOR of the client's refresh instead of the exact setting: 144 Hz and
/// 60 set give 72 (every second refresh), 165 Hz gives 55 (every third), 120 Hz
/// gives 60 itself. The setting is a wish about smoothness and link budget,
/// not a contract, and a cadence within a fifth of it that lands on the
/// client's grid is the smoother picture at the same cost.
///
/// ── When it does not apply ──────────────────────────────────────────────────
///
/// - The client tears (Chromium desktop, the default): frames are painted the
///   moment they are decoded, there is no client grid to beat against, and the
///   plain gate at the setting is the lowest latency. The caller does not ask.
/// - The setting is 0, the host's own rate: every present is encoded and the
///   cadence is the display's — nothing to align.
/// - No divisor lands within the window: the setting stands.
/// - The divisor asks for more than the host's display produces (60 Hz host,
///   144 Hz client, 60 set → 72): the host cannot make frames it does not
///   have, so the setting stands and the gate does what it always did.
///
/// Pure, so the choice is testable and the log can say exactly why.
struct AlignedCadence
{
    /// True when the cadence below differs from "the setting, as it is" for a
    /// reason worth a log line — including the case where the setting already
    /// IS a divisor (the log then says the alignment costs nothing).
    bool aligned = false;
    /// The stream's frame interval. Exact: an integer number of client
    /// refresh periods when aligned, 1 s / setting otherwise. In nanoseconds
    /// for the gate (FrameCadence::fromIntervalNs), in microseconds for the
    /// log and for anything that speaks the loop's unit.
    int64_t intervalNs = 0;
    int64_t intervalUs = 0;
    /// The rate the encoder's budget is dimensioned for — the interval above,
    /// rounded to whole frames per second.
    int fps = 0;
    /// How many client refreshes each frame stays up for. 0 when not aligned.
    int divisor = 0;
};

/// How far from the setting an aligned cadence may land, in percent, either
/// way. A fifth: 72 for 60 (144 Hz) and 48 for 60 (also 144 Hz) are both in;
/// 55 for 60 (165 Hz) is; 41 for 60 (165 Hz) is not.
inline constexpr int kAlignWindowPercent = 20;

/// Choose the stream's cadence for a client presenting on vsync at
/// @p clientMilliHz, given the viewer's @p settingFps and the host display's
/// @p displayHz. See the struct for the rules; the nearest divisor wins, the
/// faster one on a tie.
inline AlignedCadence alignCadence(int settingFps, int clientMilliHz, int displayHz)
{
    AlignedCadence out;
    if (settingFps <= 0) return out;
    out.intervalUs = 1000000 / settingFps;
    out.intervalNs = out.intervalUs * 1000;
    out.fps = settingFps;
    if (clientMilliHz <= 0) return out;

    const int64_t low = static_cast<int64_t>(settingFps) * 1000 * (100 - kAlignWindowPercent) / 100;
    const int64_t high =
        static_cast<int64_t>(settingFps) * 1000 * (100 + kAlignWindowPercent) / 100;

    int bestDivisor = 0;
    int64_t bestMilliFps = 0;
    int64_t bestDistance = 0;
    for (int n = 1;; ++n) {
        const int64_t milliFps = clientMilliHz / n;
        if (milliFps < low) break;
        if (milliFps > high) continue;
        int64_t distance = milliFps - static_cast<int64_t>(settingFps) * 1000;
        if (distance < 0) distance = -distance;
        // Nearest wins; on a tie the faster cadence, because a viewer who
        // asked for 60 and can have 72 for the same smoothness is better
        // served than one handed 48.
        if (bestDivisor == 0 || distance < bestDistance) {
            bestDivisor = n;
            bestMilliFps = milliFps;
            bestDistance = distance;
        }
    }
    if (bestDivisor == 0) return out;

    // The host cannot present faster than its own display. A rounded
    // comparison: a 165 Hz display serving a 165 Hz client at divisor 1 is
    // "every present", which the gate already is at the display's rate.
    const int fps = static_cast<int>((bestMilliFps + 500) / 1000);
    if (displayHz > 0 && fps > displayHz) return out;

    out.aligned = true;
    out.divisor = bestDivisor;
    out.intervalNs = static_cast<int64_t>(bestDivisor) * 1000000000000LL / clientMilliHz;
    out.intervalUs = out.intervalNs / 1000;
    out.fps = fps;
    return out;
}

} // namespace mw::native
