/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * Three-finger pan on a zoomed display keeps sliding after the fingers leave
 * (phone-like inertia). The glide must decay, must not fight the pan clamp
 * (an axis pinned against the image edge is done), and must never start from a
 * stale or barely-moving release — a slow drag ends exactly where it stopped.
 */

let clock = 0;
/** @type {Array<() => void>} */
let frames = [];

/** Minimal stand-in exposing just what _startPanMomentum touches. `limit` is
 *  the pan clamp _applyZoomTransform enforces (± px on both axes). */
function panSink(limit = 1e6) {
    return {
        _zoom: 2,
        _panX: 0,
        _panY: 0,
        _panSamples: [],
        _panMomentumRaf: null,
        _applyZoomTransform() {
            this._panX = Math.max(-limit, Math.min(limit, this._panX));
            this._panY = Math.max(-limit, Math.min(limit, this._panY));
        },
        _startPanMomentum: StreamView.prototype._startPanMomentum,
        _stopPanMomentum: StreamView.prototype._stopPanMomentum,
    };
}

/** Feed a flick: `steps` centroid samples spanning `ms`, travelling (dx, dy). */
function flick(v, dx, dy, ms = 100, steps = 5) {
    for (let i = 0; i <= steps; i++) {
        v._panSamples.push({
            t: clock + (ms * i) / steps,
            x: (dx * i) / steps,
            y: (dy * i) / steps,
        });
    }
    clock += ms;
}

/** Advance one animation frame (16.67 ms of the fake clock). */
function tick() {
    const due = frames;
    frames = [];
    clock += 16.67;
    for (const cb of due) cb();
}

describe('three-finger pan momentum', () => {
    beforeEach(() => {
        clock = 1000;
        frames = [];
        vi.spyOn(performance, 'now').mockImplementation(() => clock);
        vi.stubGlobal('requestAnimationFrame', (cb) => {
            frames.push(cb);
            return frames.length;
        });
        vi.stubGlobal('cancelAnimationFrame', () => {
            frames = [];
        });
    });
    afterEach(() => {
        vi.restoreAllMocks();
        vi.unstubAllGlobals();
    });

    it('keeps sliding after the release, with a decaying velocity', () => {
        const v = panSink();
        flick(v, 200, 0); // 200 px right in 100 ms
        v._startPanMomentum();
        expect(v._panMomentumRaf).not.toBeNull();

        tick();
        const first = v._panX;
        expect(first).toBeGreaterThan(0); // it moved on its own
        tick();
        const second = v._panX - first;
        tick();
        const third = v._panX - first - second;
        // Each frame travels less than the one before, and always forward.
        expect(second).toBeGreaterThan(0);
        expect(third).toBeGreaterThan(0);
        expect(third).toBeLessThan(second);
        expect(second).toBeLessThan(first);
    });

    it('carries both axes and dies out on its own', () => {
        const v = panSink();
        flick(v, 150, -150);
        v._startPanMomentum();
        for (let i = 0; i < 400 && frames.length; i++) tick();
        expect(frames).toHaveLength(0);
        expect(v._panMomentumRaf).toBeNull();
        expect(v._panX).toBeGreaterThan(0);
        expect(v._panY).toBeLessThan(0);
    });

    it('stops instead of grinding against the pan clamp', () => {
        const v = panSink(20); // edge 20 px away
        flick(v, 300, 300);
        v._startPanMomentum();
        for (let i = 0; i < 400 && frames.length; i++) tick();
        expect(v._panX).toBe(20);
        expect(v._panY).toBe(20);
        expect(v._panMomentumRaf).toBeNull();
    });

    it('does not glide on a slow drag, a stale release, or a single sample', () => {
        const slow = panSink();
        flick(slow, 4, 0); // 4 px in 100 ms — a placement, not a flick
        slow._startPanMomentum();
        expect(slow._panMomentumRaf).toBeNull();

        const stale = panSink();
        flick(stale, 200, 0);
        clock += 200; // fingers rested on screen before lifting
        stale._startPanMomentum();
        expect(stale._panMomentumRaf).toBeNull();

        const lone = panSink();
        lone._panSamples.push({ t: clock, x: 0, y: 0 });
        lone._startPanMomentum();
        expect(lone._panMomentumRaf).toBeNull();
    });

    it('is cancelled by _stopPanMomentum (a new touch interrupts the glide)', () => {
        const v = panSink();
        flick(v, 200, 0);
        v._startPanMomentum();
        tick();
        const stoppedAt = v._panX;
        v._stopPanMomentum();
        expect(v._panMomentumRaf).toBeNull();
        tick();
        expect(v._panX).toBe(stoppedAt);
    });
});
