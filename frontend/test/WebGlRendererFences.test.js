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
 * WebGlRenderer — the fences behind takeGpuBehind(), the signal of a pass the
 * GPU keeps up with, frames late (EnhancerGovernor, GPU_BEHIND_DEGRADE). The
 * renderer draws against a context that draws nothing and whose fences behave
 * like a GPU: done some time after they are set, and seen done by the page
 * some time after that. Two GPUs are replayed: the 2-CU Radeon of 25/09/2026,
 * whose FSR1 finished ~29 ms late at 60 fps, and the 120 Hz Radeon 780M of
 * 28/10/2026, whose fences reached the page more than a frame after a 0.3 ms
 * copy was done.
 */
/* global console, performance */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
// The renderer's quad probe draws on the device it is handed: its own test
// (mwsrQuadProbe.test.js) has it; here it answers 'native', the texts as they were.
vi.mock('../js/stream/renderers/mwsrQuadProbe.js', () => ({
    mwsrQuadProbeWebGpu: async () => 'native',
    mwsrQuadProbeWebGl: () => 'native',
}));
import { WebGlRenderer } from '../js/stream/renderers/WebGlRenderer.js';

const SYNC_STATUS = 0x9114;
const UNSIGNALED = 0x9118;
const SIGNALED = 0x9119;

/** The renderer's FENCE_MAX_PENDING. */
const MAX_PENDING = 4;

const clock = { now: 0 };

/**
 * A WebGL2 context whose fences are done gpuMs after they are set and seen
 * done statusMs after that; every other call draws nothing.
 */
function fakeGl({ gpuMs, statusMs = 0 }) {
    const live = new Set();
    const stats = { maxLive: 0 };
    const own = {
        SYNC_STATUS,
        SIGNALED,
        UNSIGNALED,
        SYNC_GPU_COMMANDS_COMPLETE: 0x9117,
        isContextLost: () => false,
        getContextAttributes: () => ({}),
        getExtension: () => null,
        fenceSync: () => {
            const sync = { seenAt: clock.now + gpuMs + statusMs };
            live.add(sync);
            stats.maxLive = Math.max(stats.maxLive, live.size);
            return sync;
        },
        getSyncParameter: (sync, pname) =>
            pname === SYNC_STATUS && clock.now >= sync.seenAt ? SIGNALED : UNSIGNALED,
        deleteSync: (sync) => live.delete(sync),
    };
    const gl = new Proxy(own, { get: (t, k) => (k in t ? t[k] : () => ({})) });
    return { gl, live, stats };
}

async function renderer(gpu) {
    const ctx = fakeGl(gpu);
    const canvas = { width: 0, height: 0, getContext: () => ctx.gl, addEventListener() {} };
    const r = await WebGlRenderer.create(canvas, { videoCodec: 'hevc', algo: 'gl-mwsr' });
    return { r, ...ctx };
}

/** Draw one 1080p frame per interval, in ms. */
async function play(r, intervals) {
    for (const dt of intervals) {
        clock.now += dt;
        await r.draw({ displayWidth: 1920, displayHeight: 1080, timestamp: 0, close() {} });
    }
}

const every = (ms, n) => Array(n).fill(ms);
const FPS60 = 1000 / 60;
const FPS120 = 1000 / 120;

describe('WebGlRenderer — the GPU fences', () => {
    beforeEach(() => {
        clock.now = 1000;
        vi.spyOn(performance, 'now').mockImplementation(() => clock.now);
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });
    afterEach(() => vi.restoreAllMocks());

    it('says nothing before a fence could be judged', async () => {
        const { r } = await renderer({ gpuMs: 0.3 });
        expect(r.takeGpuBehind()).toBeNull();
        await play(r, [FPS120]);
        expect(r.takeGpuBehind()).toBeNull();
    });

    it('reads a copy on time at 120 fps although its fence reaches the page a frame late (780M)', async () => {
        // Seen done 10.3 ms after it was set: later than the next frame (8.3 ms),
        // which is when the fence used to be read, and found late every time.
        const { r } = await renderer({ gpuMs: 0.3, statusMs: 10 });
        await play(r, every(FPS120, 120));
        expect(r.takeGpuBehind()).toBe(0);
    });

    it('still sees the GPU of 25/09, ~29 ms late at 60 fps', async () => {
        const { r } = await renderer({ gpuMs: 29 });
        await play(r, every(FPS60, 120));
        expect(r.takeGpuBehind()).toBe(1);
    });

    it('sees the same lag at 120 fps, and two frames queued on the GPU', async () => {
        for (const gpuMs of [29, 17]) {
            const { r } = await renderer({ gpuMs });
            await play(r, every(FPS120, 240));
            expect(r.takeGpuBehind()).toBe(1);
        }
    });

    it('calls late, at either rate, a GPU more than a 60 fps frame behind, not one 120 fps frame behind', async () => {
        for (const fps of [FPS60, FPS120]) {
            const { r } = await renderer({ gpuMs: 9 });
            await play(r, every(fps, 120));
            expect(r.takeGpuBehind()).toBe(0);
        }
    });

    it('leaves the fences of frames back to back for a later draw, and keeps few of them', async () => {
        // A network hiccup: 100 ms without a frame, then eight 0.2 ms apart.
        // Read at those draws, their fences would all be late (seen at 1.3 ms).
        const { r, live, stats } = await renderer({ gpuMs: 0.3, statusMs: 1 });
        await play(r, [...every(FPS60, 10), 100, ...every(0.2, 7), ...every(FPS60, 10)]);
        expect(r.takeGpuBehind()).toBe(0);
        expect(stats.maxLive).toBeLessThanOrEqual(MAX_PENDING + 1);
        expect(live.size).toBeLessThanOrEqual(MAX_PENDING);
    });

    it('deletes the fences still waiting when it goes', async () => {
        const { r, live } = await renderer({ gpuMs: 0.3 });
        await play(r, every(FPS120, 10));
        expect(live.size).toBeGreaterThan(0);
        r.dispose();
        expect(live.size).toBe(0);
    });
});
