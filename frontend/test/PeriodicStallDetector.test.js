/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { PeriodicStallDetector } from '../js/stream/PeriodicStallDetector.js';

const FRAME_MS = 1000 / 60;
const PERIOD_FRAMES = 30; // 30 frames @60fps ≈ 500ms, the AWDL cadence

/**
 * Drive `count` arriving frames at 60fps through the detector.
 * `lateness(i)` is the extra transit delay (ms) for frame i on top of a constant
 * 50ms baseline; `keyframe(i)` marks a frame as a keyframe.
 * Returns the verdicts emitted (one entry per frame, mostly null).
 */
function run(d, count, lateness = () => 0, keyframe = () => false, start = 0) {
    const out = [];
    for (let i = start; i < start + count; i++) {
        const backendTs = 100000 + i * FRAME_MS;
        const now = 5000 + i * FRAME_MS + 50 + lateness(i);
        out.push(d.note(backendTs, now, keyframe(i)));
    }
    return out;
}

/** A 40ms stall every PERIOD_FRAMES frames — the measured AWDL signature. */
const awdl = (i) => (i % PERIOD_FRAMES === 0 ? 40 : 0);

describe('PeriodicStallDetector', () => {
    it('stays silent on a metronomic link', () => {
        const d = new PeriodicStallDetector();
        const verdicts = run(d, 900);
        expect(verdicts.every((v) => v === null)).toBe(true);
        expect(d.verdict).toBe(null);
    });

    it('stays silent on jitter that is not periodic', () => {
        const d = new PeriodicStallDetector();
        // Deterministic but irregular: spikes at intervals of 7, 23, 41, 13...
        // frames, all well inside the plausible period band individually.
        const gaps = [17, 41, 23, 71, 13, 53, 29, 37, 61, 19, 47, 11];
        const hits = new Set();
        let at = 0;
        for (const g of gaps) {
            at += g;
            hits.add(at);
        }
        const verdicts = run(d, 900, (i) => (hits.has(i) ? 40 : 0));
        expect(verdicts.every((v) => v === null)).toBe(true);
        expect(d.verdict).toBe(null);
    });

    it('detects a metronomic stall and reports its period', () => {
        const d = new PeriodicStallDetector();
        run(d, 900, awdl);
        expect(d.verdict).not.toBe(null);
        expect(d.verdict.periodMs).toBeCloseTo(PERIOD_FRAMES * FRAME_MS, 0);
        expect(d.verdict.events).toBeGreaterThanOrEqual(6);
    });

    it('never fires on a periodic keyframe cadence', () => {
        const d = new PeriodicStallDetector();
        // Keyframes are large, so they legitimately take longer to transit. A
        // regular IDR cadence is therefore a perfectly periodic spike train —
        // the textbook false positive this detector must not produce.
        const verdicts = run(
            d,
            900,
            (i) => (i % PERIOD_FRAMES === 0 ? 40 : 0),
            (i) => i % PERIOD_FRAMES === 0,
        );
        expect(verdicts.every((v) => v === null)).toBe(true);
        expect(d.verdict).toBe(null);
    });

    it('holds its verdict until enough events span enough time', () => {
        const d = new PeriodicStallDetector();
        // 3s: past the warmup, but neither MIN_EVENTS nor MIN_SPAN_MS is met.
        run(d, 180, awdl);
        expect(d.verdict).toBe(null);
        run(d, 720, awdl, () => false, 180);
        expect(d.verdict).not.toBe(null);
    });

    it('reports the verdict exactly once', () => {
        const d = new PeriodicStallDetector();
        const verdicts = run(d, 1800, awdl).filter((v) => v !== null);
        expect(verdicts.length).toBe(1);
        expect(verdicts[0].periodMs).toBeCloseTo(PERIOD_FRAMES * FRAME_MS, 0);
    });

    it('ignores frames with no capture stamp', () => {
        const d = new PeriodicStallDetector();
        for (let i = 0; i < 900; i++) expect(d.note(0, i * FRAME_MS, false)).toBe(null);
        expect(d.verdict).toBe(null);
    });

    it('rebases on a discontinuity instead of scoring it as an event', () => {
        const d = new PeriodicStallDetector();
        run(d, 200, awdl); // onsets accumulating, not yet enough to latch
        expect(d.verdict).toBe(null);
        // Host clock jump / session change: the next frame is 5s off.
        expect(d.note(100000 - 5000, 5000 + 200 * FRAME_MS, false)).toBe(null);
        // The onset history is gone, so 3s of the same disturbance no longer
        // gets there — the warmup and the event count start over.
        run(d, 180, awdl, () => false, 201);
        expect(d.verdict).toBe(null);
        // The detector is not deaf though, it only restarted its clock.
        run(d, 540, awdl, () => false, 381);
        expect(d.verdict).not.toBe(null);
    });

    it('reset() clears a latched verdict', () => {
        const d = new PeriodicStallDetector();
        run(d, 900, awdl);
        expect(d.verdict).not.toBe(null);
        d.reset();
        expect(d.verdict).toBe(null);
        expect(run(d, 900).every((v) => v === null)).toBe(true);
    });
});
