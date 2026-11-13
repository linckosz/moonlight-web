/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * A trackpad pinch zooms on the host as Ctrl+wheel (issue #52, v0.4.4): one
 * notch for every 10 % of scale, in high-resolution units, Ctrl held for the
 * whole pinch, never turned around with the wheel, and nothing on a Mac host.
 * Chrome sends a pinch as Ctrl wheel events, Safari as gesture events.
 */
import { afterEach, describe, it, expect, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { WheelInverter, WHEEL_GESTURE_GAP_MS } from '../js/stream/WheelSource.js';
import {
    PINCH_GAP_MS,
    PINCH_NOTCH,
    PINCH_STEP,
    PinchToWheel,
    chromePinchLog,
} from '../js/stream/PinchZoom.js';

const UA = {
    chromeMac:
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36',
    safariMac:
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.0 Safari/605.1.15',
};

/** A stand-in for the view's wheel, gesture and key paths. */
function pinchView({ support = null, hostOs = 'Windows' } = {}) {
    const sent = [];
    const beats = [];
    const v = {
        sent,
        beats,
        webrtc: { send: (m) => sent.push(m) },
        host: { hostOs },
        _heldPhysKeys: new Map(),
        _heldMouseButtons: new Set(),
        _gamepadManager: null,
        _wheelAccum: 0,
        _wheelAccumX: 0,
        _wheelInverter: support ? new WheelInverter(support) : null,
        _wheelFlushTimer: 0,
        _pinch: null,
        _pinchWheel: new PinchToWheel(),
        _composedKey: null,
        _appleKeyboard: false,
        _metaTapCodes: null,
        _swallowKeyUpCodes: null,
        _sendToHost: (m) => beats.push(m),
    };
    for (const name of [
        'handleWheel',
        'handleGesture',
        '_realCtrlHeld',
        '_pinchFromWheel',
        '_pinchZoom',
        '_endPinch',
        '_sendWheelTravel',
        '_sendWheel',
        '_sendKeyEvent',
        '_sendInputState',
        '_releaseAllPhysKeys',
    ])
        v[name] = StreamView.prototype[name];
    return v;
}

/** A Chrome pinch event: Ctrl set, no Ctrl key, deltaY = -100 × ln(scale). */
function pinch(deltaY, timeStamp = 0, extra = {}) {
    return {
        deltaX: 0,
        deltaY,
        deltaMode: 0,
        ctrlKey: true,
        wheelDeltaX: 0,
        wheelDeltaY: deltaY < 0 ? 120 : -120,
        timeStamp,
        getModifierState: (k) => k === 'Control',
        preventDefault() {},
        ...extra,
    };
}

function wheel(deltaY, wheelDeltaY, timeStamp = 0, ctrlKey = false) {
    return {
        deltaX: 0,
        deltaY,
        deltaMode: 0,
        ctrlKey,
        wheelDeltaX: 0,
        wheelDeltaY,
        timeStamp,
        preventDefault() {},
    };
}

function gesture(type, scale) {
    return { type, scale, rotation: 0, preventDefault() {} };
}

const wheelSum = (sent) =>
    sent.filter((m) => m.type === 'mousewheel').reduce((a, m) => a + m.delta, 0);
/** The units a scale stands for, give or take the unit a carry keeps back. */
const unitsFor = (scale) => (PINCH_NOTCH * Math.log(scale)) / PINCH_STEP;

describe('a pinch, as host wheel units', () => {
    it('turns 10 % of scale into one notch, in the zoom-in direction', () => {
        const p = new PinchToWheel();
        let total = 0;
        for (let i = 0; i < 10; i++) total += p.push(Math.log(1.1));
        expect(Math.abs(total - 1200)).toBeLessThanOrEqual(1);
    });

    it('zooms out for a scale under 1', () => {
        expect(new PinchToWheel().push(Math.log(0.5))).toBe(Math.trunc(unitsFor(0.5)));
        expect(new PinchToWheel().push(Math.log(0.5))).toBeLessThan(0);
    });

    it('carries what does not make a unit, and drops nothing', () => {
        const p = new PinchToWheel();
        expect(p.push(0.0005)).toBe(0); // 0.63 unit
        expect(p.push(0.0005)).toBe(1); // 1.26
        let total = 1;
        for (let i = 0; i < 98; i++) total += p.push(0.0005);
        expect(Math.abs(total - (PINCH_NOTCH * 0.05) / PINCH_STEP)).toBeLessThan(1);
    });

    it('ignores a scale that is not a number', () => {
        expect(new PinchToWheel().push(NaN)).toBe(0);
        expect(new PinchToWheel().push(Infinity)).toBe(0);
    });

    it('reads Chrome deltaY as -100 × ln(scale)', () => {
        expect(chromePinchLog(-100 * Math.log(2))).toBeCloseTo(Math.log(2), 12);
        expect(chromePinchLog(50)).toBeCloseTo(-0.5, 12);
    });
});

describe('a pinch in the stream', () => {
    afterEach(() => vi.useRealTimers());

    it('holds Ctrl on the host and turns the wheel, then lets Ctrl go once the pinch stops', () => {
        vi.useFakeTimers();
        const v = pinchView();
        v.handleWheel(pinch(-5, 0));
        v.handleWheel(pinch(-5, 16));
        expect(v.sent[0]).toMatchObject({
            type: 'keydown',
            code: 'ControlRight',
            keyCode: 0x11,
            ctrlKey: true,
        });
        expect(v.sent.slice(1).every((m) => m.type === 'mousewheel' && m.delta > 0)).toBe(true);
        expect(Math.abs(wheelSum(v.sent) - unitsFor(Math.exp(0.1)))).toBeLessThan(1);
        expect(v.sent.some((m) => m.type === 'keyup')).toBe(false);
        vi.advanceTimersByTime(PINCH_GAP_MS);
        expect(v.sent.at(-1)).toMatchObject({
            type: 'keyup',
            code: 'ControlRight',
            ctrlKey: false,
        });
        expect(v._pinch).toBe(null);
    });

    it('zooms out when the fingers close', () => {
        const v = pinchView();
        v.handleWheel(pinch(10));
        expect(wheelSum(v.sent)).toBe(Math.trunc(unitsFor(Math.exp(-0.1))));
        expect(wheelSum(v.sent)).toBeLessThan(0);
    });

    it('does not press Ctrl for a pinch too small to turn the wheel', () => {
        const v = pinchView();
        v.handleWheel(pinch(-0.05)); // 0.63 unit
        expect(v.sent).toEqual([]);
    });

    it('sends a real Ctrl+wheel as the wheel it is, Ctrl already down on the host', () => {
        const v = pinchView();
        v._heldPhysKeys.set('ControlLeft', { type: 'keyup', code: 'ControlLeft', keyCode: 0x11 });
        v.handleWheel(wheel(100, -120, 0, true));
        expect(v.sent).toEqual([{ type: 'mousewheel', delta: -120 }]);
    });

    it('takes getModifierState for no proof of a Ctrl key: Chrome sets it for a pinch', () => {
        const v = pinchView();
        v.handleWheel(pinch(-20));
        expect(v.sent[0]).toMatchObject({ type: 'keydown', code: 'ControlRight' });
    });

    it('is never turned around by the wheel setting, Chrome or Safari', () => {
        for (const support of ['chromium-mac', 'safari-mac']) {
            const off = pinchView();
            const on = pinchView({ support });
            off.handleWheel(pinch(-20));
            on.handleWheel(pinch(-20));
            expect(on.sent).toEqual(off.sent);
            expect(wheelSum(on.sent)).toBeGreaterThan(0);
        }
    });

    it('leaves the wheel setting to tell the next scroll anew', () => {
        const v = pinchView({ support: 'chromium-mac' });
        v.handleWheel(pinch(-20, 0));
        v._endPinch();
        v.sent.length = 0;
        // A trackpad scroll right after: wheelDelta = -3 × delta, not turned around.
        v.handleWheel(wheel(100, -300, 40));
        expect(v.sent).toEqual([{ type: 'mousewheel', delta: -120 }]);
    });

    it('sends first a Safari wheel step the setting was holding', () => {
        const v = pinchView({ support: 'safari-mac' });
        v.handleWheel(wheel(4, -12, 0));
        expect(v.sent).toEqual([]);
        v.handleWheel(pinch(-20, 40));
        expect(v.sent[0].type).toBe('mousewheel');
        expect(v.sent[1]).toMatchObject({ type: 'keydown', code: 'ControlRight' });
        expect(v._wheelInverter.holding).toBe(false);
    });

    it('keeps Ctrl in the heartbeat while the pinch lasts, and out of it after', () => {
        const v = pinchView();
        v.handleWheel(pinch(-20));
        v._sendInputState();
        expect(v.beats.at(-1).keys).toEqual([
            expect.objectContaining({ code: 'ControlRight', ctrlKey: true }),
        ]);
        v._endPinch();
        v._sendInputState();
        expect(v.beats.at(-1).keys).toEqual([]);
    });

    it('lets Ctrl go when the stream lets every key go, and presses it again if the pinch goes on', () => {
        const v = pinchView();
        v.handleWheel(pinch(-20, 0));
        v._releaseAllPhysKeys();
        expect(v.sent.at(-1)).toMatchObject({ type: 'keyup', code: 'ControlRight' });
        expect(v._pinch).toBe(null);
        v.handleWheel(pinch(-20, 16));
        expect(v.sent.filter((m) => m.type === 'keydown')).toHaveLength(2);
    });

    it('lets Ctrl go when the view stops in the middle of a pinch', () => {
        const v = pinchView();
        v.handleWheel(pinch(-20));
        v._endPinch();
        expect(v.sent.at(-1)).toMatchObject({ type: 'keyup', code: 'ControlRight' });
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('does nothing on a Mac host, no scroll either', () => {
        vi.useFakeTimers();
        const v = pinchView({ hostOs: 'macOS' });
        v.handleWheel(pinch(-20, 0));
        v.handleGesture(gesture('gesturestart', 1));
        v.handleGesture(gesture('gesturechange', 2));
        v.handleGesture(gesture('gestureend', 2));
        vi.advanceTimersByTime(PINCH_GAP_MS);
        expect(v.sent).toEqual([]);
    });

    it('a Chrome pinch is over after PINCH_GAP_MS, the wheel gesture gap', () => {
        expect(PINCH_GAP_MS).toBe(WHEEL_GESTURE_GAP_MS);
    });
});

describe('a Safari pinch in the stream', () => {
    it('zooms by the scale its gesture events carry, Ctrl held from start to end', () => {
        const v = pinchView();
        v.handleGesture(gesture('gesturestart', 1.001));
        v.handleGesture(gesture('gesturechange', 1.1));
        v.handleGesture(gesture('gesturechange', 1.21));
        expect(v.sent[0]).toMatchObject({ type: 'keydown', code: 'ControlRight' });
        expect(v.sent.some((m) => m.type === 'keyup')).toBe(false);
        v.handleGesture(gesture('gestureend', 1.21));
        expect(Math.abs(wheelSum(v.sent) - 240)).toBeLessThanOrEqual(1);
        expect(v.sent.at(-1)).toMatchObject({ type: 'keyup', code: 'ControlRight' });
    });

    it('keeps the page from zooming', () => {
        const v = pinchView();
        const e = { ...gesture('gesturestart', 1), preventDefault: vi.fn() };
        v.handleGesture(e);
        expect(e.preventDefault).toHaveBeenCalled();
    });

    it('takes one source per pinch: a Ctrl wheel during the gesture is left out', () => {
        const v = pinchView();
        v.handleGesture(gesture('gesturestart', 1));
        v.handleGesture(gesture('gesturechange', 1.5));
        const before = v.sent.length;
        v.handleWheel(pinch(-50));
        expect(v.sent).toHaveLength(before);
        v.handleGesture(gesture('gestureend', 1.5));
        expect(Math.abs(wheelSum(v.sent) - unitsFor(1.5))).toBeLessThan(1);
    });

    it('ignores a gesture that never started', () => {
        const v = pinchView();
        v.handleGesture(gesture('gesturechange', 2));
        v.handleGesture(gesture('gestureend', 2));
        expect(v.sent).toEqual([]);
    });

    it('is listened to in Safari on a Mac, although ongesturechange is not in window', () => {
        expect('ongesturechange' in globalThis).toBe(false);
        expect(StreamView.listensForGestures(UA.safariMac, 0)).toBe(true);
    });

    it('is left to the touch screen on an iPad, and not listened to elsewhere', () => {
        expect(StreamView.listensForGestures(UA.safariMac, 5)).toBe(false);
        expect(StreamView.listensForGestures(UA.chromeMac, 0)).toBe(false);
    });
});
