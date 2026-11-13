/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { afterEach, describe, it, expect, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { WHEEL_GESTURE_GAP_MS, WheelInverter } from '../js/stream/WheelSource.js';

/**
 * The GameStream scroll packet counts WHEEL_DELTA units (120 = one notch), not
 * browser pixels. Forwarding deltaY raw made a notch 100 units, which Windows
 * hosts absorb but Linux hosts floor away (Sunshine's uinput backend emits
 * REL_WHEEL = amount / 120). The backend also reads the value with
 * QJsonValue::toInt(), which returns its 0 default for a non-integer — so a
 * fractional delta must never reach the wire.
 */

/** Minimal stand-in exposing just what _sendWheel touches. */
function wheelSink() {
    const sent = [];
    return {
        sent,
        _wheelAccum: 0,
        _wheelAccumX: 0,
        webrtc: { send: (m) => sent.push(m) },
        _sendWheel: StreamView.prototype._sendWheel,
    };
}

describe('wheel scroll normalization', () => {
    it('turns one notch into exactly one WHEEL_DELTA, whatever the delta unit', () => {
        // Chromium: 100 CSS px per notch. Firefox: 3 lines per notch.
        expect(StreamView._wheelNotches(100, 0)).toBe(1);
        expect(StreamView._wheelNotches(3, 1)).toBe(1);

        const v = wheelSink();
        v._sendWheel('mousewheel', StreamView._wheelNotches(100, 0), 'y');
        expect(v.sent).toEqual([{ type: 'mousewheel', delta: 120 }]);
    });

    it('sends whole units only, carrying the remainder to the next event', () => {
        const v = wheelSink();
        // A trackpad's fractional pixels: 53.33px ≈ 0.533 notch ≈ 64.0 units.
        v._sendWheel('mousewheel', StreamView._wheelNotches(53.33, 0), 'y');
        expect(v.sent).toHaveLength(1);
        expect(Number.isInteger(v.sent[0].delta)).toBe(true);
        expect(v.sent[0].delta).toBe(63);

        v._sendWheel('mousewheel', StreamView._wheelNotches(53.33, 0), 'y');
        // Nothing is lost: two half-notches add up to a full one.
        expect(v.sent[0].delta + v.sent[1].delta).toBe(127);
    });

    it('drops a sub-unit delta rather than rounding it to zero on the wire', () => {
        const v = wheelSink();
        v._sendWheel('mousewheel', 0.001, 'y'); // 0.12 units
        expect(v.sent).toEqual([]);
        expect(v._wheelAccum).toBeCloseTo(0.12);
    });

    it('keeps the two axes on independent carries', () => {
        const v = wheelSink();
        v._sendWheel('mousewheel', 0.5, 'y');
        v._sendWheel('mousehwheel', 0.5, 'x');
        expect(v.sent).toEqual([
            { type: 'mousewheel', delta: 60 },
            { type: 'mousehwheel', delta: 60 },
        ]);
    });

    it('clamps a wild delta to the signed 16-bit field of the packet', () => {
        const v = wheelSink();
        v._sendWheel('mousewheel', 10000, 'y'); // 1.2M units
        expect(v.sent[0].delta).toBe(32767);
        v._sendWheel('mousehwheel', -10000, 'x');
        expect(v.sent[1].delta).toBe(-32768);
    });
});

/** A stand-in for the view's wheel path, with the setting's inverter or none. */
function wheelView(support = null) {
    const v = wheelSink();
    v._wheelInverter = support ? new WheelInverter(support) : null;
    v._wheelFlushTimer = 0;
    v.handleWheel = StreamView.prototype.handleWheel;
    v._sendWheelTravel = StreamView.prototype._sendWheelTravel;
    return v;
}

/** A wheel event as the view receives it. */
function wheelEvent(deltaX, deltaY, wheelDeltaX, wheelDeltaY, timeStamp) {
    return {
        deltaX,
        deltaY,
        deltaMode: 0,
        wheelDeltaX,
        wheelDeltaY,
        timeStamp,
        preventDefault() {},
    };
}

describe('inverting the notched wheel in the stream', () => {
    afterEach(() => vi.useRealTimers());

    it('sends every event as before when the setting is off', () => {
        const v = wheelView();
        v.handleWheel(wheelEvent(0, 100, 0, -120, 0));
        v.handleWheel(wheelEvent(50, 0, -150, 0, 16));
        expect(v.sent).toEqual([
            { type: 'mousewheel', delta: -120 },
            { type: 'mousehwheel', delta: 60 },
        ]);
    });

    it('turns a Chrome notch around and leaves the trackpad after it', () => {
        const v = wheelView('chromium-mac');
        // A notch: 100 px, wheelDelta a notch away from -3 × 100.
        v.handleWheel(wheelEvent(0, 100, 0, -120, 0));
        // The trackpad, after a pause: wheelDelta = -3 × delta.
        v.handleWheel(wheelEvent(0, 100, 0, -300, WHEEL_GESTURE_GAP_MS + 1));
        expect(v.sent).toEqual([
            { type: 'mousewheel', delta: 120 },
            { type: 'mousewheel', delta: -120 },
        ]);
    });

    it('turns both axes of a notched gesture around', () => {
        const v = wheelView('chromium-mac');
        v.handleWheel(wheelEvent(50, 100, -120, -120, 0));
        expect(v.sent).toEqual([
            { type: 'mousewheel', delta: 120 },
            { type: 'mousehwheel', delta: -60 },
        ]);
    });

    it('sends a Safari step held alone as a notch once the gesture ends', () => {
        vi.useFakeTimers();
        const v = wheelView('safari-mac');
        v.handleWheel(wheelEvent(0, -100, 0, 300, 0));
        expect(v.sent).toEqual([{ type: 'mousewheel', delta: 120 }]);
        // A wheel's opening step: held until its gesture tells what it is.
        v.handleWheel(wheelEvent(0, 4, 0, -12, WHEEL_GESTURE_GAP_MS + 1));
        expect(v.sent).toHaveLength(1);
        vi.advanceTimersByTime(WHEEL_GESTURE_GAP_MS);
        expect(v.sent).toEqual([
            { type: 'mousewheel', delta: 120 },
            { type: 'mousewheel', delta: 4 },
        ]);
    });

    it('sends a held Safari step with the event that tells, and arms no flush after', () => {
        vi.useFakeTimers();
        const v = wheelView('safari-mac');
        v.handleWheel(wheelEvent(0, 4, 0, -12, 0));
        v.handleWheel(wheelEvent(0, 96.5, 0, -289.5, 40));
        expect(v.sent).toEqual([
            { type: 'mousewheel', delta: 4 },
            { type: 'mousewheel', delta: 116 },
        ]);
        vi.advanceTimersByTime(WHEEL_GESTURE_GAP_MS * 2);
        expect(v.sent).toHaveLength(2);
    });
});
