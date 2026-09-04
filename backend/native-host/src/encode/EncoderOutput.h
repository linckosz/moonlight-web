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

namespace mw::native::encode {

/// One encoded frame, pointing into the encoder's own output buffer.
///
/// Valid only until the next call on this encoder: the buffer is released as
/// soon as the caller is done with it, which is what keeps the GPU→CPU copy at
/// exactly one per frame instead of one plus an allocation.
///
/// Platform-neutral on purpose — moved out of the D3D11-typed encoder interface
/// when the Linux encoder arrived (04/09/2026): every encoder on every OS
/// answers in this shape, which is what lets the session loop and the sender be
/// written once.
struct EncoderOutput
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool keyframe = false;

    /// The average quantizer the encoder reports for this frame, or -1 when it
    /// does not say. The objective proxy for quality in the benchmarks: at a
    /// fixed bitrate, a lower QP is a sharper picture. H.264/HEVC report a QP
    /// (0–51); AV1 reports a q-index (0–255) — same direction, other scale.
    int avgQp = -1;
};

} // namespace mw::native::encode
