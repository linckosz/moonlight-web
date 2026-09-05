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

namespace mw::native::audio {

/// Planar float32 → interleaved stereo, which is the one shape the rest of the
/// audio path understands.
///
/// WASAPI hands over interleaved samples and needs none of this. Core Audio —
/// and so ScreenCaptureKit — hands over one buffer PER CHANNEL, and PipeWire
/// can do either. So the conversion belongs here rather than in each capture,
/// and being pure arithmetic it is the part of the platform glue that can be
/// tested on any machine.
///
/// What it decides, and why:
///  - one plane is a mono source, and it is DUPLICATED rather than panned left:
///    a mono host played only in one ear is a bug the player would blame on
///    their headphones;
///  - more than two planes (a 5.1 endpoint) keeps the first two, which are
///    front-left and front-right in every layout Core Audio hands out — a real
///    downmix is a matrix this pipeline has never needed, and taking the fronts
///    is honest silence about the surrounds rather than a wrong sum;
///  - a missing plane is silence on that channel, not a read through a null
///    pointer.
inline void interleaveToStereo(const float* const* planes, int planeCount, size_t frames,
                               float* out)
{
    if (!out || frames == 0) return;

    const float* left = (planes && planeCount > 0) ? planes[0] : nullptr;
    const float* right = (planes && planeCount > 1) ? planes[1] : left;
    if (!right) right = left;

    for (size_t i = 0; i < frames; ++i) {
        out[i * 2] = left ? left[i] : 0.0f;
        out[i * 2 + 1] = right ? right[i] : 0.0f;
    }
}

/// Interleaved float32 with any channel count → interleaved stereo, by the
/// same three decisions. PipeWire negotiates interleaved F32 for a capture
/// stream and is asked for two channels, but what it hands over is what it
/// negotiated — a mono monitor, a 5.1 sink — and the answer to "not stereo"
/// belongs here, tested, rather than in a real-time callback.
inline void interleavedToStereo(const float* in, int channels, size_t frames, float* out)
{
    if (!out || frames == 0) return;
    if (!in || channels <= 0) {
        for (size_t i = 0; i < frames * 2; ++i)
            out[i] = 0.0f;
        return;
    }
    const size_t stride = static_cast<size_t>(channels);
    for (size_t i = 0; i < frames; ++i) {
        const float* s = in + i * stride;
        out[i * 2] = s[0];
        out[i * 2 + 1] = channels >= 2 ? s[1] : s[0];
    }
}

} // namespace mw::native::audio
