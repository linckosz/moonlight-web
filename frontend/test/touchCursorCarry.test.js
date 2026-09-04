/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { StreamViewTouch } from '../js/ui/StreamViewTouch.js';

/**
 * Trackpad mode turns finger pixels into RELATIVE host deltas, and the host
 * takes integers. Each touchmove used to be rounded on its own, so a delta
 * below half a pixel became 0 — and, worse, was still sent: an endless run of
 * {dx:0, dy:0} that the host injected faithfully while the cursor stood still.
 *
 * Reported 04/09/2026 from an iPhone: the worker log counted 12 injected events
 * for a whole gesture where the pointer never moved, against 247 for the same
 * drag performed quickly. The fraction has to be carried between events, the
 * way _emitScroll has always carried the wheel's.
 */

/** Minimal stand-in exposing just what handleTouchMove's 1-finger path touches. */
function touchSink(sensitivity, zoom) {
    const sent = [];
    return {
        sent,
        webrtc: { send: (m) => sent.push(m) },
        handleTouchMove: StreamViewTouch.prototype.handleTouchMove,
        _clearLongPress: () => {},
        _touchActive: true,
        _touchScreen: false, // trackpad mode, not touchscreen mode
        _touchMaxFingers: 1,
        _touchDragging: false,
        _touchFingerCount: 1,
        _lastMoveFingerCount: 1,
        _touchTapThreshold: 8,
        _touchMoved: false,
        _touchStartX: 0,
        _touchStartY: 0,
        _touchLastX: 0,
        _touchLastY: 0,
        _touchSensitivity: sensitivity,
        _zoom: zoom,
        _moveAccumX: 0,
        _moveAccumY: 0,
    };
}

/** One touchmove to (x, y), with the DOM surface the handler inspects. */
function move(view, x, y) {
    view.handleTouchMove({
        preventDefault() {},
        target: { closest: () => null },
        touches: [{ clientX: x, clientY: y }],
    });
}

describe('touch trackpad cursor', () => {
    it('carries the fraction instead of rounding each event to nothing', () => {
        // Sensitivity 0.4: every single-pixel step is a sub-pixel host delta.
        const v = touchSink(0.4, 1);
        for (let i = 1; i <= 4; i++) move(v, i, 0);

        // Four steps × 0.4 = 1.6 host pixels: one whole pixel goes out, and the
        // 0.6 left over stays on the books rather than being thrown away.
        expect(v.sent).toEqual([{ type: 'mousemove', dx: 1, dy: 0 }]);
        expect(v._moveAccumX).toBeCloseTo(0.6, 6);

        // Two more steps push the carry over the next whole pixel.
        move(v, 5, 0);
        move(v, 6, 0);
        expect(v.sent).toHaveLength(2);
        expect(v.sent[1]).toEqual({ type: 'mousemove', dx: 1, dy: 0 });
    });

    it('never sends a zero move', () => {
        const v = touchSink(0.4, 1);
        move(v, 1, 0); // 0.4 px — below a whole one
        expect(v.sent).toHaveLength(0);
    });

    it('still moves when zoomed in, where sensitivity is divided by up to 3', () => {
        // zoom 8 → zoomSlow 0.3. This is the state that made the bug stick
        // across sessions: the zoom is persisted in the settings.
        const v = touchSink(2.2, 8);
        for (let i = 1; i <= 3; i++) move(v, i, i);
        const totalX = v.sent.reduce((n, m) => n + m.dx, 0);
        expect(totalX).toBeGreaterThan(0);
        for (const m of v.sent) {
            expect(Number.isInteger(m.dx)).toBe(true);
            expect(Number.isInteger(m.dy)).toBe(true);
        }
    });

    it('drops the carry when the gesture comes back to one finger', () => {
        const v = touchSink(0.4, 1);
        move(v, 1, 0);
        expect(v._moveAccumX).toBeCloseTo(0.4, 6);

        // A second finger was down and has just been lifted. This frame's delta
        // is deliberately skipped (the finger that remains is somewhere else
        // entirely, and its delta would read as a jump) — so the carry that
        // belonged to the interrupted gesture has to go with it.
        v._lastMoveFingerCount = 2;
        move(v, 400, 300);
        expect(v.sent).toHaveLength(0);
        expect(v._moveAccumX).toBe(0);
        expect(v._moveAccumY).toBe(0);

        // And the pointer resumes from where the finger now is: the next move
        // is worth its own 5 px, not the 400 the finger was teleported across.
        move(v, 405, 300);
        expect(v.sent).toEqual([{ type: 'mousemove', dx: 2, dy: 0 }]);
    });
});
