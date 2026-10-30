/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { mwsrZoneDrop, readMwsrZoneBlock } from '../js/stream/MwsrZoneBlock.js';
import { WebRtcDataChannel } from '../js/api/WebRtcDataChannel.js';

// The block as backend/src/streaming/MwsrZoneBlock.h writes it.
function block(codes, cols, rows, scale, version = 1) {
    const size = 7 + ((cols * rows + 1) >> 1);
    const b = new Uint8Array(size);
    b[0] = size >> 8;
    b[1] = size & 0xff;
    b[2] = version;
    b[3] = cols;
    b[4] = rows;
    const s = Math.round(scale * 1000);
    b[5] = s >> 8;
    b[6] = s & 0xff;
    for (let i = 0; i < cols * rows; i++) b[7 + (i >> 1)] |= i & 1 ? codes[i] : codes[i] << 4;
    return b;
}

function concat(...parts) {
    const out = new Uint8Array(parts.reduce((n, p) => n + p.length, 0));
    let at = 0;
    for (const p of parts) {
        out.set(p, at);
        at += p.length;
    }
    return out;
}

// The chunks as backend/src/streaming/FrameChunks.h cuts them.
function chunks(payload, frameId, flags, size) {
    const total = Math.ceil(payload.length / size);
    const out = [];
    for (let k = 0; k < total; k++) {
        const part = payload.subarray(k * size, Math.min(payload.length, (k + 1) * size));
        const c = new Uint8Array(17 + part.length);
        const v = new DataView(c.buffer);
        v.setUint32(0, frameId);
        v.setUint16(4, k);
        v.setUint16(6, total);
        v.setUint8(8, flags);
        v.setUint32(9, part.length);
        v.setUint32(13, 4242);
        c.set(part, 17);
        out.push(c.buffer);
    }
    return out;
}

describe('MwsrZoneBlock — the side block of the native host', () => {
    it('reads 32 x 18 codes, high half first, and the scale', () => {
        const codes = Array.from({ length: 576 }, (_, i) => i % 12);
        const bitstream = new Uint8Array([0, 0, 0, 1, 0x65]);
        const split = readMwsrZoneBlock(concat(block(codes, 32, 18, 2560 / 1920), bitstream));
        expect(split.zones.cols).toBe(32);
        expect(split.zones.rows).toBe(18);
        expect(split.zones.scale).toBe(1.333);
        expect(Array.from(split.zones.codes)).toEqual(codes);
        expect(Array.from(split.bitstream)).toEqual(Array.from(bitstream));
    });

    it('steps over a version it does not know, and finds the bitstream', () => {
        const b = block([1, 2, 3, 4], 2, 2, 2, 9);
        const split = readMwsrZoneBlock(concat(b, new Uint8Array([7, 8])));
        expect(split.zones).toBeNull();
        expect(Array.from(split.bitstream)).toEqual([7, 8]);
    });

    it('gives up on a length that cannot be right', () => {
        expect(readMwsrZoneBlock(new Uint8Array([0, 3, 1, 1, 1, 0, 1]))).toBeNull();
        expect(readMwsrZoneBlock(new Uint8Array([1, 0, 1, 1, 1, 0, 1, 0]))).toBeNull();
        expect(readMwsrZoneBlock(new Uint8Array([0]))).toBeNull();
    });

    it('drops as the host decides them: none for the law', () => {
        expect(mwsrZoneDrop(0)).toBe(0);
        expect(mwsrZoneDrop(1)).toBeCloseTo(0.1, 6);
        expect(mwsrZoneDrop(11)).toBeCloseTo(1.35, 6);
    });
});

describe('WebRtcDataChannel — a frame with its zones', () => {
    beforeEach(() => {
        vi.spyOn(performance, 'now').mockReturnValue(100000);
    });
    afterEach(() => {
        vi.restoreAllMocks();
    });

    function transport() {
        const t = new WebRtcDataChannel('wss://host.invalid/signal');
        t.dataChannels.input = { readyState: 'open', send: vi.fn() };
        t._lastIdrRequestTime = -1e6;
        t.onVideo = vi.fn();
        return t;
    }

    it('hands the decoder the bitstream alone, and the zones beside it', () => {
        const t = transport();
        const codes = Array.from({ length: 576 }, (_, i) => (i * 7) % 12);
        const bitstream = new Uint8Array(3000).map((_, i) => (i * 13) & 0xff);
        // Chunks of 1024: the block and the bitstream cut across each other.
        for (const c of chunks(concat(block(codes, 32, 18, 2), bitstream), 5, 0x03, 1024))
            t._onVideoChunk(c);
        expect(t.onVideo).toHaveBeenCalledTimes(1);
        const [frame, key, ts, id, zones] = t.onVideo.mock.calls[0];
        expect(Array.from(frame)).toEqual(Array.from(bitstream));
        expect(key).toBe(true);
        expect(ts).toBe(4242);
        expect(id).toBe(5);
        expect(Array.from(zones.codes)).toEqual(codes);
        expect(zones.scale).toBe(2);
        expect(t.stats.mwsrZoneFrames).toBe(1);
    });

    it('reads a frame without the flag as it always did', () => {
        const t = transport();
        const bitstream = new Uint8Array([9, 9, 9]);
        for (const c of chunks(bitstream, 6, 0x01, 1024)) t._onVideoChunk(c);
        for (const c of chunks(bitstream, 7, 0x00, 1024)) t._onVideoChunk(c);
        const calls = t.onVideo.mock.calls;
        expect(calls.map((c) => [Array.from(c[0]), c[1], c[4]])).toEqual([
            [[9, 9, 9], true, null],
            [[9, 9, 9], false, null],
        ]);
        expect(t.stats.mwsrZoneFrames).toBe(0);
    });

    it('drops a frame whose block hides its bitstream, and asks for a keyframe', () => {
        const t = transport();
        t.onFrameLoss = vi.fn();
        for (const c of chunks(new Uint8Array([0xff, 0xff, 1, 2, 3]), 8, 0x02, 1024))
            t._onVideoChunk(c);
        expect(t.onVideo).not.toHaveBeenCalled();
        expect(t.onFrameLoss).toHaveBeenCalledWith(8, false);
        expect(t.stats.framesDropped).toBe(1);
        expect(
            t.dataChannels.input.send.mock.calls.filter((c) => c[0].includes('requestidr')).length,
        ).toBe(1);
    });
});
