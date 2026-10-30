/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

// MWSR's zones on the wire (plan B, docs/design/upscaler-mwsr-b.md §12 c): a
// side block at the head of a video frame's payload, ahead of its bitstream,
// flagged in every chunk header of that frame (FrameSender::kFlagSideBlock).
// The native host's frames carry it only to a client whose /start asked for
// it (`mwsr_map`): an older frontend reads byte 8 as "a keyframe when not
// zero", and never meets the flag.
//
//   [length:2][version:1][cols:1][rows:1][scale:2][codes: (cols × rows + 1) / 2]
//
// Big endian, as the chunk header. `length` is the whole block's, itself
// included, so a reader steps over a version it does not know and finds the
// bitstream. `scale` is the upscale the decision assumed (the capture's width
// over the stream's) in thousandths; the client ignores the grid when its own
// differs by more than 10 %. The codes are 4 bits, two to a byte, the first
// zone in the high half, the zones row by row: 0 the client's law, 1..11 a
// drop under it. 32 × 18 zones make 295 bytes, 140 kbit/s at 60 frames a
// second.
//
// Written by the relay that sends the frame (DataChannelRelay), read by
// frontend/js/stream/MwsrZoneBlock.js.
namespace mwsrzoneblock {

constexpr uint8_t kVersion = 1;
constexpr size_t kHeaderSize = 7;

/// One frame's zones, borrowed from the engine (NativeMediaEngine::FrameView).
struct Zones
{
    const uint8_t* codes = nullptr;
    int cols = 0;
    int rows = 0;
    float scale = 0.0f;
};

/// The block for @p zones, or nothing when there is none to write: no codes,
/// a grid of no zone or past 255 a side, a code past 4 bits, a scale outside
/// what thousandths in 16 bits hold. The frame then goes without a grid.
inline std::vector<uint8_t> write(const Zones& zones)
{
    std::vector<uint8_t> block;
    if (!zones.codes || zones.cols <= 0 || zones.rows <= 0 || zones.cols > 255 || zones.rows > 255)
        return block;
    const long scale = std::lround(static_cast<double>(zones.scale) * 1000.0);
    if (!(scale > 0 && scale <= 0xFFFF)) return block;
    const size_t count = static_cast<size_t>(zones.cols) * static_cast<size_t>(zones.rows);
    for (size_t i = 0; i < count; ++i)
        if (zones.codes[i] > 15) return block;

    const size_t size = kHeaderSize + (count + 1) / 2;
    block.resize(size);
    block[0] = static_cast<uint8_t>(size >> 8);
    block[1] = static_cast<uint8_t>(size & 0xFF);
    block[2] = kVersion;
    block[3] = static_cast<uint8_t>(zones.cols);
    block[4] = static_cast<uint8_t>(zones.rows);
    block[5] = static_cast<uint8_t>(scale >> 8);
    block[6] = static_cast<uint8_t>(scale & 0xFF);
    for (size_t i = 0; i < count; ++i) {
        const uint8_t code = zones.codes[i];
        block[kHeaderSize + i / 2] |= (i % 2 == 0) ? static_cast<uint8_t>(code << 4) : code;
    }
    return block;
}

} // namespace mwsrzoneblock
