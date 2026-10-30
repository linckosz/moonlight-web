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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

// A video frame cut into the DataChannel's chunks: the 17-byte header and the
// cut itself, with nothing of the channel, so that the tests reach them
// (FrameSender sends the chunks; the frontend joins them in _onVideoChunk).
//
//   [frame_id:4][chunk_index:2][total_chunks:2][flags:1][payload_size:4][backend_ts:4]
//
// All multi-byte fields big endian, the same frame_id, flags and backend_ts in
// every chunk of a frame. The flags byte was is_keyframe, 0 or 1, until the
// side block: bit 0 a keyframe, bit 1 a side block ahead of the bitstream
// (MwsrZoneBlock.h). A frontend older than the side block reads the byte as
// "keyframe when not zero", so only a client that asked for one gets one.
namespace framechunks {

constexpr size_t kHeaderSize = 17;
/// The most one SCTP message takes.
constexpr size_t kMaxPayload = 16000;
constexpr uint8_t kFlagKeyframe = 0x01;
constexpr uint8_t kFlagSideBlock = 0x02;

using Chunk = std::vector<std::byte>;

/// Write the 17-byte chunk header at @p dst.
inline void writeHeader(std::byte* dst, uint32_t frameId, uint16_t chunkIdx, uint16_t totalChunks,
                        uint8_t flags, uint32_t payloadSize, uint32_t backendTs)
{
    dst[0] = static_cast<std::byte>((frameId >> 24) & 0xFF);
    dst[1] = static_cast<std::byte>((frameId >> 16) & 0xFF);
    dst[2] = static_cast<std::byte>((frameId >> 8) & 0xFF);
    dst[3] = static_cast<std::byte>(frameId & 0xFF);

    dst[4] = static_cast<std::byte>((chunkIdx >> 8) & 0xFF);
    dst[5] = static_cast<std::byte>(chunkIdx & 0xFF);

    dst[6] = static_cast<std::byte>((totalChunks >> 8) & 0xFF);
    dst[7] = static_cast<std::byte>(totalChunks & 0xFF);

    dst[8] = static_cast<std::byte>(flags);

    dst[9] = static_cast<std::byte>((payloadSize >> 24) & 0xFF);
    dst[10] = static_cast<std::byte>((payloadSize >> 16) & 0xFF);
    dst[11] = static_cast<std::byte>((payloadSize >> 8) & 0xFF);
    dst[12] = static_cast<std::byte>(payloadSize & 0xFF);

    // Same value for all chunks of a frame.
    dst[13] = static_cast<std::byte>((backendTs >> 24) & 0xFF);
    dst[14] = static_cast<std::byte>((backendTs >> 16) & 0xFF);
    dst[15] = static_cast<std::byte>((backendTs >> 8) & 0xFF);
    dst[16] = static_cast<std::byte>(backendTs & 0xFF);
}

/// Cut one frame into chunks, header included, reading @p data once (see
/// FrameSender::buildFragments). @p sideBlock, when given, goes ahead of
/// @p data as if one with it, the chunks cut across the two, and every chunk
/// says so (kFlagSideBlock). Nothing for an empty frame.
inline std::vector<Chunk> cut(const uint8_t* data, size_t size, bool isKeyframe, uint32_t frameId,
                              uint32_t backendTs, size_t maxPayload = kMaxPayload,
                              const uint8_t* sideBlock = nullptr, size_t sideBlockSize = 0)
{
    std::vector<Chunk> chunks;
    if (!data || size == 0) return chunks;
    if (!sideBlock) sideBlockSize = 0;

    const size_t total = sideBlockSize + size;
    const uint8_t flags = static_cast<uint8_t>((isKeyframe ? kFlagKeyframe : 0) |
                                               (sideBlockSize ? kFlagSideBlock : 0));

    // Never above what one SCTP message takes, never so small that a large
    // keyframe would need more chunks than the 16-bit count holds.
    size_t payloadMax = std::min(maxPayload, kMaxPayload);
    payloadMax = std::max<size_t>(payloadMax, 1024);
    payloadMax = std::max<size_t>(payloadMax, (total + 65534) / 65535);
    const size_t totalChunks = (total + payloadMax - 1) / payloadMax;
    chunks.reserve(totalChunks);

    for (size_t chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        const size_t offset = chunkIdx * payloadMax;
        const size_t payloadSize = std::min(payloadMax, total - offset);

        Chunk bin(kHeaderSize + payloadSize);
        writeHeader(bin.data(), frameId, static_cast<uint16_t>(chunkIdx),
                    static_cast<uint16_t>(totalChunks), flags, static_cast<uint32_t>(payloadSize),
                    backendTs);
        std::byte* out = bin.data() + kHeaderSize;
        size_t from = offset;
        size_t left = payloadSize;
        if (from < sideBlockSize) {
            const size_t n = std::min(left, sideBlockSize - from);
            std::memcpy(out, sideBlock + from, n);
            out += n;
            from += n;
            left -= n;
        }
        if (left > 0) std::memcpy(out, data + (from - sideBlockSize), left);
        chunks.push_back(std::move(bin));
    }
    return chunks;
}

} // namespace framechunks
