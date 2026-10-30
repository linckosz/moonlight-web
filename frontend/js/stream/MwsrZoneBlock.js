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

/**
 * MWSR's zones off the wire (plan B, docs/design/upscaler-mwsr-b.md §12 c):
 * the side block the native host puts at the head of a frame's payload when
 * this page asked for it at /start (`mwsr_map`), flagged by bit 1 of byte 8 of
 * every chunk header of that frame. Written by backend/src/streaming/
 * MwsrZoneBlock.h:
 *
 *   [length:2][version:1][cols:1][rows:1][scale:2][codes: (cols × rows + 1) / 2]
 *
 * Big endian. `length` counts the whole block, so a version this page does not
 * know is stepped over and the bitstream still found. `scale` is the upscale
 * the host's decision assumed, in thousandths. The codes are 4 bits, two to a
 * byte, high half first, the zones row by row: 0 the sharpness law, 1..11 a
 * drop under it (MWSR_ZONE_DROP).
 */

/** Byte 8 of the chunk header: bit 0 a keyframe, bit 1 a side block. */
export const CHUNK_FLAG_KEYFRAME = 0x01;
export const CHUNK_FLAG_SIDE_BLOCK = 0x02;

const VERSION = 1;
const HEADER_SIZE = 7;

/**
 * The drop a code takes off the sharpness law, as the weight across an edge
 * (the native host's mwsrzones::dropOf).
 * @param {number} code 0..11
 * @returns {number}
 */
export function mwsrZoneDrop(code) {
    return code <= 0 ? 0 : 0.1 + (code - 1) * 0.125;
}

/**
 * Split an assembled frame whose chunks carried the side-block flag into its
 * zones and its bitstream.
 *
 * `zones` is null when the block is of a version this page does not read (the
 * frame then takes the law); the whole result is null when the block's length
 * cannot be right (shorter than its header, or past the frame): the bitstream
 * cannot be found, and the frame is to be treated as damaged.
 *
 * @param {Uint8Array} frame the payloads of the frame's chunks, joined
 * @returns {{zones: {cols: number, rows: number, scale: number, codes: Uint8Array}|null,
 *            bitstream: Uint8Array}|null}
 */
export function readMwsrZoneBlock(frame) {
    if (!frame || frame.length < 2) return null;
    const length = (frame[0] << 8) | frame[1];
    if (length < HEADER_SIZE || length > frame.length) return null;
    const bitstream = frame.subarray(length);
    if (frame[2] !== VERSION) return { zones: null, bitstream };

    const cols = frame[3];
    const rows = frame[4];
    const count = cols * rows;
    const scale = ((frame[5] << 8) | frame[6]) / 1000;
    if (count === 0 || scale <= 0 || length < HEADER_SIZE + ((count + 1) >> 1)) {
        return { zones: null, bitstream };
    }
    const codes = new Uint8Array(count);
    for (let i = 0; i < count; i++) {
        const byte = frame[HEADER_SIZE + (i >> 1)];
        codes[i] = i & 1 ? byte & 0x0f : byte >> 4;
    }
    return { zones: { cols, rows, scale, codes }, bitstream };
}
