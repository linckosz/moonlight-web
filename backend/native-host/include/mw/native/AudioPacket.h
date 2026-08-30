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

#include <cstddef>
#include <cstdint>

namespace mw::native {

/// One encoded Opus packet, ready to ride the existing RTP audio track.
///
/// Same lifetime rule as EncodedFrame: `data` is valid only inside the
/// callback.
///
/// The engine produces exactly what the current pipeline already carries —
/// stereo Opus at 48 kHz — so the browser's AudioWorklet path is untouched.
struct AudioPacket
{
    const uint8_t* data = nullptr;
    size_t size = 0;

    /// Samples per channel in this packet at 48 kHz: 240 for a 5 ms frame, 480
    /// for 10 ms. The relay advances the RTP timestamp by exactly this, which
    /// is what keeps the browser's NetEq from time-stretching (robotic audio)
    /// on a jittery arrival clock.
    int samplesPerChannel = 240;

    /// Steady-clock microseconds at which the underlying PCM was captured.
    int64_t capturedUs = 0;
};

} // namespace mw::native
