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
    FLAG_REGION,
    FLAG_TIMEOUT_MS,
} from '../js/stream/LatencyProbe.js';

const px = (b, w, r) => new Uint8ClampedArray([...b, 255, ...w, 255, ...r, 255]);

describe('looksLikeFlag', () => {
    it('accepts pure blue / white / red', () => {
        expect(looksLikeFlag(px([0, 0, 255], [255, 255, 255], [255, 0, 0]))).toBe(true);
    });

    it('accepts the bands after a lossy encode and chroma subsampling', () => {
        expect(looksLikeFlag(px([18, 12, 231], [238, 240, 236], [226, 20, 30]))).toBe(true);
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

    it('records nothing usable when there is no picture to sample', async () => {
        const probe = new LatencyProbe({ source: () => null, sendClick: vi.fn() });
        const entry = await probe.measureOnce();
        expect(entry.ok).toBe(false);
        expect(entry.reason).toBe('no picture to sample');
    });
});
