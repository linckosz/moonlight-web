/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { FramePacer } from '../js/stream/FramePacer.js';

const FRAME_MS = 1000 / 60;

/**
 * Drive `count` frames at 60fps through the pacer.
 * `lateness(i)` returns the extra transit delay (ms) for frame i on top of a
 * constant 50ms baseline. Returns the per-frame hold actually applied.
 */
function run(pacer, count, lateness = () => 0, startCapture = 100000, startNow = 5000) {
    const holds = [];
    for (let i = 0; i < count; i++) {
        const backendTs = startCapture + i * FRAME_MS;
        const now = startNow + i * FRAME_MS + 50 + lateness(i);
        holds.push(pacer.schedule(backendTs, now) - now);
    }
    return holds;
}

describe('FramePacer', () => {
    it('presents immediately when there is no capture stamp', () => {
        const p = new FramePacer();
        expect(p.schedule(0, 1234)).toBe(1234);
        expect(p.schedule(undefined, 1234)).toBe(1234);
        expect(p.targetMs).toBe(0);
    });

    it('holds nothing on a metronomic link (bit-for-bit the old behavior)', () => {
        const p = new FramePacer();
        const holds = run(p, 300); // 5s, zero jitter
        expect(holds.every((h) => h === 0)).toBe(true);
        expect(p.targetMs).toBe(0);
        expect(p.stats.held).toBe(0);
    });

    it('grows a reserve covering the late tail once jitter appears', () => {
        const p = new FramePacer();
        run(p, 120, (i) => (i % 2 ? 20 : 0)); // 2s of ±20ms jitter
        // p95 of the excess is ~20ms, plus the safety margin.
        expect(p.targetMs).toBeGreaterThan(18);
        expect(p.targetMs).toBeLessThanOrEqual(20 * 1.15 + 0.5);
    });

    it('spends the reserve on late frames and the wait on early ones', () => {
        const p = new FramePacer();
        run(p, 120, (i) => (i % 2 ? 20 : 0));
        const target = p.targetMs;
        const base = 100000 + 200 * FRAME_MS;
        const nowEarly = 5000 + 200 * FRAME_MS + 50;
        // A frame on the best path waits the whole reserve...
        expect(p.schedule(base, nowEarly) - nowEarly).toBeCloseTo(target, 1);
        // ...one that already lost the reserve in the network goes straight out.
        expect(p.schedule(base, nowEarly + target) - (nowEarly + target)).toBe(0);
    });

    it('never exceeds MAX', () => {
        const p = new FramePacer({ MAX: 40 });
        run(p, 240, (i) => (i % 2 ? 400 : 0));
        expect(p.targetMs).toBeLessThanOrEqual(40);
    });

    it('bumps immediately on an underrun', () => {
        const p = new FramePacer();
        p.noteUnderrun(1000);
        expect(p.targetMs).toBe(8);
        p.noteUnderrun(1000);
        expect(p.targetMs).toBe(16);
        expect(p.stats.underruns).toBe(2);
    });

    it('decays back to exactly zero after sustained calm', () => {
        const p = new FramePacer();
        run(p, 120, (i) => (i % 2 ? 20 : 0));
        expect(p.targetMs).toBeGreaterThan(0);
        // 6s of clean link after the jitter burst.
        run(p, 360, () => 0, 100000 + 120 * FRAME_MS, 5000 + 120 * FRAME_MS);
        expect(p.targetMs).toBe(0);
    });

    it('holds the whole reserve through a short lull between bursts', () => {
        const p = new FramePacer();
        run(p, 120, (i) => (i % 2 ? 20 : 0));
        const peak = p.targetMs;
        // 1s of calm: still inside WINDOW_MS, so the tail estimate — and the
        // reserve — must not move. Releasing here would re-expose the very next
        // burst, which is the common case on a link that jitters in waves.
        run(p, 60, () => 0, 100000 + 120 * FRAME_MS, 5000 + 120 * FRAME_MS);
        expect(p.targetMs).toBe(peak);
    });

    it('decays gradually once the window has cleared, not in one step', () => {
        const p = new FramePacer();
        run(p, 120, (i) => (i % 2 ? 20 : 0));
        const peak = p.targetMs;
        // 2.5s of calm: the window is purged (~2s) and DECAY_STEP(2ms) per
        // DECAY_INTERVAL(250ms) has had ~2 steps to act — moving, but far from 0.
        run(p, 150, () => 0, 100000 + 120 * FRAME_MS, 5000 + 120 * FRAME_MS);
        expect(p.targetMs).toBeLessThan(peak);
        expect(p.targetMs).toBeGreaterThan(peak - 10);
    });

    it('rebases on a discontinuity instead of pegging the reserve', () => {
        const p = new FramePacer();
        run(p, 120);
        // Host clock jumps (uint32 wrap / session change): one huge delta.
        const now = 5000 + 200 * FRAME_MS;
        expect(p.schedule(100000 - 5000, now)).toBe(now); // rebased, presented now
        expect(p.targetMs).toBe(0);
        expect(p.stats.lateTailMs).toBe(0);
    });

    it('reset() restores the clean-link state', () => {
        const p = new FramePacer();
        run(p, 120, (i) => (i % 2 ? 20 : 0));
        expect(p.targetMs).toBeGreaterThan(0);
        p.reset();
        expect(p.targetMs).toBe(0);
        expect(p.stats.held).toBe(0);
        expect(run(p, 60).every((h) => h === 0)).toBe(true);
    });
});
