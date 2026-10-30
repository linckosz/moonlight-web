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
 * drop under it (mwsrZoneDrop).
 *
 * The WebGPU mwsr kernel reads them (WebGpuRenderer.noteFrameZones): one
 * texel per zone, its drop (mwsrZoneTexels), wherever the page draws at the
 * scale the host decided them for (mwsrZonesFit).
 */

/** Byte 8 of the chunk header: bit 0 a keyframe, bit 1 a side block. */
export const CHUNK_FLAG_KEYFRAME = 0x01;
export const CHUNK_FLAG_SIDE_BLOCK = 0x02;

const VERSION = 1;
const HEADER_SIZE = 7;

/** The codes a host of this version writes: 0 the law, 1..11 a drop. */
export const MWSR_ZONE_CODES = 12;

/**
 * The scales the zones are read at: the client's, the smaller axis, within
 * a tenth of the host's (plan B §12 c: a shrunk window, a zoom, a capture of
 * a physical screen, and the tree's statistics no longer see the client's
 * pixels), and never under ×1.3. The bench measured from ×1.33 up; below, the
 * kernel's strength falls to 0 at 1:1, and the drop over it, of the across
 * weight, would grow without a bound.
 */
export const MWSR_ZONE_SCALE_TOLERANCE = 0.1;
export const MWSR_ZONE_MIN_SCALE = 1.3;

/**
 * The drop a code takes off the sharpness law, as the weight across an edge
 * (the native host's mwsrzones::dropOf). A code past 11 is a later host's:
 * the law here.
 * @param {number} code 0..15
 * @returns {number}
 */
export function mwsrZoneDrop(code) {
    return code > 0 && code < MWSR_ZONE_CODES ? 0.1 + (code - 1) * 0.125 : 0;
}

// IEEE binary16 of 0 or of a normal half (2^-14 to 65504), to nearest, ties to
// even: the drops' range.
function halfBits(x) {
    if (x === 0) return 0;
    const e = Math.floor(Math.log2(x));
    const v = (x / 2 ** e - 1) * 1024;
    let m = Math.round(v);
    if (m - v === 0.5 && m & 1) m -= 1;
    return m === 1024 ? (e + 16) << 10 : ((e + 15) << 10) | m;
}

const DROP_HALF = Uint16Array.from({ length: 16 }, (_, code) => halfBits(mwsrZoneDrop(code)));

/**
 * The zones' texels for an r16float texture of cols × rows: each zone's drop,
 * as binary16 bits, row by row.
 * @param {Uint8Array} codes
 * @returns {Uint16Array}
 */
export function mwsrZoneTexels(codes) {
    const out = new Uint16Array(codes.length);
    for (let i = 0; i < codes.length; i++) out[i] = DROP_HALF[codes[i] & 15];
    return out;
}

/**
 * Whether a frame's zones may be read where the page draws it at `scale`
 * (output / input, the smaller axis): see MWSR_ZONE_SCALE_TOLERANCE. A frame
 * whose zones do not fit takes the law.
 * @param {{scale: number}} zones
 * @param {number} scale
 * @returns {boolean}
 */
export function mwsrZonesFit(zones, scale) {
    return (
        scale >= MWSR_ZONE_MIN_SCALE &&
        Math.abs(scale - zones.scale) <= MWSR_ZONE_SCALE_TOLERANCE * zones.scale
    );
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
