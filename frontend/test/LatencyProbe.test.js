/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * LatencyProbe — click-to-photon measurement.
 *
 * Observable behaviour only: what the classifier says of a pixel triple, what
 * a run appends to the results, and when a sample is discarded. The output
 * surface is stubbed at the sampling boundary (_sample is the one place that
 * touches a canvas), never the DOM below it.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
    LatencyProbe,
    looksLikeFlag,
    summarize,
    describePixels,
    FLAG_REGION,
    FLAG_TIMEOUT_MS,
} from '../js/stream/LatencyProbe.js';

const px = (b, w, r) => new Uint8ClampedArray([...b, 255, ...w, 255, ...r, 255]);

/**
 * A model of what the video pipeline does to a colour, so the tolerance of
 * looksLikeFlag is checked against a round trip rather than against numbers
 * someone thought looked plausible.
 *
 * Covers what actually alters the flag's colours between the host's window and
 * the browser's canvas: the limited-range Y'CbCr conversion, the two matrix and
 * range mismatches that happen in the wild, quantisation, and the chroma a
 * sample near a band boundary would share with its neighbour. It is a model,
 * not a measurement of a real encoder — but the three bands are flat saturated
 * primaries, which is the case a codec preserves best.
 */
const MATRIX = {
    bt709: { kr: 0.2126, kb: 0.0722 },
    bt601: { kr: 0.299, kb: 0.114 },
};
const clamp255 = (v) => Math.max(0, Math.min(255, Math.round(v)));

/** RGB 0..255 → limited-range 8-bit Y'CbCr. */
function encode([r, g, b], { kr, kb }) {
    const kg = 1 - kr - kb;
    const y = (kr * r + kg * g + kb * b) / 255;
    return [
        16 + 219 * y,
        128 + 224 * ((b / 255 - y) / (2 * (1 - kb))),
        128 + 224 * ((r / 255 - y) / (2 * (1 - kr))),
    ].map(clamp255);
}

/** Limited-range 8-bit Y'CbCr → RGB 0..255. `full` reads it as if full range. */
function decode([Y, Cb, Cr], { kr, kb }, full = false) {
    const y = full ? Y / 255 : (Y - 16) / 219;
    const cb = (Cb - 128) / (full ? 255 : 224);
    const cr = (Cr - 128) / (full ? 255 : 224);
    const r = y + 2 * (1 - kr) * cr;
    const b = y + 2 * (1 - kb) * cb;
    const g = (y - kr * r - kb * b) / (1 - kr - kb);
    return [r, g, b].map((v) => clamp255(v * 255));
}

const BANDS = { blue: [0, 0, 255], white: [255, 255, 255], red: [255, 0, 0] };

/** Run every band of `bands` through `through`, and pack the result for the test. */
const roundTrip = (through, bands = BANDS) =>
    px(through(bands.blue), through(bands.white), through(bands.red));

describe('looksLikeFlag', () => {
    it('accepts pure blue / white / red', () => {
        expect(looksLikeFlag(px([0, 0, 255], [255, 255, 255], [255, 0, 0]))).toBe(true);
    });

    it('accepts the bands after a limited-range BT.709 round trip', () => {
        const c = (rgb) => decode(encode(rgb, MATRIX.bt709), MATRIX.bt709);
        expect(c(BANDS.blue)).toEqual([1, 0, 255]);
        expect(c(BANDS.red)).toEqual([255, 1, 0]);
        expect(looksLikeFlag(roundTrip(c))).toBe(true);
    });

    it('accepts them through a matrix mismatch, either way round', () => {
        // The classic production mismatch: encoded BT.709, decoded BT.601 or
        // the reverse. It shifts the hue by a few percent and never makes the
        // blue band less blue than the red one.
        for (const [enc, dec] of [
            [MATRIX.bt709, MATRIX.bt601],
            [MATRIX.bt601, MATRIX.bt709],
        ])
            expect(looksLikeFlag(roundTrip((rgb) => decode(encode(rgb, enc), dec)))).toBe(true);
    });

    it('accepts them when limited-range data is read as full range', () => {
        const c = (rgb) => decode(encode(rgb, MATRIX.bt709), MATRIX.bt709, true);
        // White lands on 235 rather than 255 — still well over the threshold.
        expect(c(BANDS.white)).toEqual([235, 235, 235]);
        expect(looksLikeFlag(roundTrip(c))).toBe(true);
    });

    it('survives quantisation far coarser than any usable stream', () => {
        const quant = (v, q) => clamp255(Math.round(v / q) * q);
        for (const q of [8, 16, 32, 48, 64, 80]) {
            const c = (rgb) =>
                decode(
                    encode(rgb, MATRIX.bt709).map((v) => quant(v, q)),
                    MATRIX.bt709,
                );
            expect(looksLikeFlag(roundTrip(c)), `quantisation step ${q}`).toBe(true);
        }
    });

    it('still holds if a sample drifts onto a band boundary in 4:2:0', () => {
        // The tightest case of all, and the reason the probe samples the CENTRE
        // of each band: on a boundary the chroma is the average of two bands,
        // and the blue reading falls to b=137 against a threshold of 120. The
        // geometry keeps the samples 1.5 % of the picture width away from any
        // boundary (28 px at 1920, 9.6 px at 640) against the ~2 px that
        // chroma subsampling shifts, so this is margin, not the operating point.
        const smeared = (rgb, neighbour) => {
            const self = encode(rgb, MATRIX.bt709);
            const other = encode(neighbour, MATRIX.bt709);
            // Luma keeps its own resolution; only the chroma pair is shared.
            return decode(
                [self[0], (self[1] + other[1]) / 2, (self[2] + other[2]) / 2],
                MATRIX.bt709,
            );
        };
        const blue = smeared(BANDS.blue, BANDS.white);
        expect(blue[2]).toBeGreaterThan(120);
        expect(
            looksLikeFlag(
                px(blue, smeared(BANDS.white, BANDS.blue), smeared(BANDS.red, BANDS.white)),
            ),
        ).toBe(true);
    });

    it('does not turn a plain desktop into a flag through the same round trip', () => {
        // The model would be worthless if it made everything pass.
        const c = (rgb) => decode(encode(rgb, MATRIX.bt709), MATRIX.bt709);
        const desktop = { blue: [60, 60, 60], white: [255, 255, 255], red: [60, 60, 60] };
        expect(looksLikeFlag(roundTrip(c, desktop))).toBe(false);
    });

    it('rejects a plain desktop (grey, white, grey)', () => {
        expect(looksLikeFlag(px([60, 60, 60], [255, 255, 255], [60, 60, 60]))).toBe(false);
    });

    it('rejects the bands in the wrong order', () => {
        expect(looksLikeFlag(px([255, 0, 0], [255, 255, 255], [0, 0, 255]))).toBe(false);
    });

    it('rejects a dim, desaturated triple', () => {
        expect(looksLikeFlag(px([40, 40, 110], [150, 150, 150], [110, 40, 40]))).toBe(false);
    });

    it('rejects short or missing input', () => {
        expect(looksLikeFlag(null)).toBe(false);
        expect(looksLikeFlag(new Uint8ClampedArray(8))).toBe(false);
    });
});

describe('summarize', () => {
    it('reports count, median, p90 and range', () => {
        const s = summarize([30, 10, 20, 40, 50]);
        expect(s.n).toBe(5);
        expect(s.median).toBe(30);
        expect(s.p90).toBe(50);
        expect(s.min).toBe(10);
        expect(s.max).toBe(50);
    });

    it('ignores nulls and gives nulls back for an empty set', () => {
        expect(summarize([null, undefined, NaN]).n).toBe(0);
        expect(summarize([]).median).toBeNull();
    });
});

describe('describePixels', () => {
    it('writes the three samples out, in band order', () => {
        expect(describePixels(px([0, 0, 255], [255, 255, 255], [255, 0, 0]))).toBe(
            'blue?0,0,255 white?255,255,255 red?255,0,0',
        );
    });

    it('gives nothing back when there is nothing to describe', () => {
        expect(describePixels(null)).toBeNull();
        expect(describePixels(new Uint8ClampedArray(4))).toBeNull();
    });
});

describe('geometry contract with the host', () => {
    it('keeps the flag at the top of the screen, around the centre', () => {
        expect(FLAG_REGION.top).toBe(0);
        expect(FLAG_REGION.left).toBeLessThan(0.5);
        expect(FLAG_REGION.right).toBeGreaterThan(0.5);
        expect(FLAG_REGION.bottom).toBeLessThanOrEqual(0.1);
        expect(FLAG_TIMEOUT_MS).toBe(200);
    });
});

describe('LatencyProbe.run', () => {
    let now;
    let rafQueue;

    beforeEach(() => {
        now = 1000;
        rafQueue = [];
        vi.useFakeTimers();
        vi.spyOn(performance, 'now').mockImplementation(() => now);
        // rAF callbacks run when time is advanced, like frames would.
        vi.stubGlobal('requestAnimationFrame', (cb) => {
            rafQueue.push(cb);
            return rafQueue.length;
        });
        vi.stubGlobal('cancelAnimationFrame', () => {});
    });

    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    /** Advance the clock by `ms`, running due timers and queued frames. */
    async function tick(ms) {
        now += ms;
        const cbs = rafQueue.splice(0);
        for (const cb of cbs) cb(now);
        await vi.advanceTimersByTimeAsync(0);
        vi.advanceTimersByTime(ms);
        await Promise.resolve();
    }

    function makeProbe(flagAt) {
        const results = [];
        const sendClick = vi.fn();
        const showMark = vi.fn();
        const probe = new LatencyProbe({
            source: () => ({}),
            sendClick,
            showMark,
            requestFrameEvents: null,
            results,
        });
        // Flag visible from `flagAt` ms after the click was sent, for 100 ms.
        let clickAt = null;
        sendClick.mockImplementation(() => {
            clickAt = now;
        });
        probe._sample = () => {
            if (clickAt === null || flagAt === null) return false;
            const dt = now - clickAt;
            return dt >= flagAt && dt < flagAt + 100;
        };
        return { probe, results, sendClick, showMark };
    }

    it('sends the click, paints the mark and records the latency', async () => {
        const { probe, results, sendClick, showMark } = makeProbe(48);
        const p = probe.measureOnce();
        expect(sendClick).toHaveBeenCalledTimes(1);
        expect(showMark).toHaveBeenCalledTimes(1);
        for (let i = 0; i < 10; i++) await tick(8);
        const entry = await p;
        expect(entry.ok).toBe(true);
        expect(entry.latencyMs).toBe(48);
        expect(entry.reason).toBeNull();
        expect(typeof entry.ts).toBe('number');
        expect(results).toEqual([entry]);
    });

    it('drops the sample when the flag never shows within the timeout', async () => {
        const { probe, results } = makeProbe(null);
        const p = probe.measureOnce();
        for (let i = 0; i < 30; i++) await tick(10);
        const entry = await p;
        expect(entry.ok).toBe(false);
        expect(entry.latencyMs).toBeNull();
        expect(entry.reason).toBe('timeout');
        expect(results).toHaveLength(1);
    });

    it('spots the flag from a presented-frame hook between frames', async () => {
        const { probe } = makeProbe(5);
        const p = probe.measureOnce();
        now += 6;
        probe.onFramePresented();
        const entry = await p;
        expect(entry.ok).toBe(true);
        expect(entry.latencyMs).toBe(6);
    });

    it('measures several clicks, spaced, and appends them all', async () => {
        const { probe, results, sendClick } = makeProbe(30);
        const run = probe.run(3, 2000);
        // Each click: a few frames to find the flag, then the spacing.
        for (let c = 0; c < 3; c++) {
            for (let i = 0; i < 8; i++) await tick(8);
            await tick(2000);
        }
        const entries = await run;
        expect(sendClick).toHaveBeenCalledTimes(3);
        expect(entries).toHaveLength(3);
        expect(entries.every((e) => e.ok && e.latencyMs === 32)).toBe(true);
        expect(results).toHaveLength(3);
    });

    it('a dropped sample says what it read and where', async () => {
        // What the Intel bench saw: a uniform grey where the host's screen is
        // white. Without these two fields the entry is just "timeout" and the
        // picture cannot be told apart from a pipeline that never delivered.
        const grey = px([140, 140, 140], [140, 140, 140], [140, 140, 140]);
        const probe = new LatencyProbe({
            source: () => ({}),
            sendClick: vi.fn(),
            samplePixels: () => grey,
            describeSource: () => 'canvas2d 1920x1080',
        });
        const p = probe.measureOnce();
        for (let i = 0; i < 30; i++) await tick(10);
        const entry = await p;
        expect(entry.ok).toBe(false);
        expect(entry.reason).toBe('timeout');
        expect(entry.saw).toBe('blue?140,140,140 white?140,140,140 red?140,140,140');
        expect(entry.via).toBe('renderer · canvas2d 1920x1080');
    });

    it('a successful sample carries no explanation', async () => {
        const { probe } = makeProbe(16);
        const p = probe.measureOnce();
        for (let i = 0; i < 6; i++) await tick(8);
        const entry = await p;
        expect(entry.ok).toBe(true);
        expect(entry.saw).toBeUndefined();
        expect(entry.via).toBeUndefined();
    });

    it('records nothing usable when there is no picture to sample', async () => {
        const probe = new LatencyProbe({ source: () => null, sendClick: vi.fn() });
        const entry = await probe.measureOnce();
        expect(entry.ok).toBe(false);
        expect(entry.reason).toBe('no picture to sample');
    });
});
