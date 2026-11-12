/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';
import { StreamViewPen } from '../js/ui/StreamViewPen.js';
import { StreamView } from '../js/ui/StreamView.js';

/** A pointer event as the input layer receives it (jsdom has no PointerEvent). */
function pointer(type, fields = {}) {
    const e = new Event(type, { bubbles: true, cancelable: true });
    return Object.assign(e, {
        pointerType: 'pen',
        pointerId: 1,
        clientX: 100,
        clientY: 50,
        buttons: 0,
        pressure: 0,
        tiltX: 0,
        tiltY: 0,
        ...fields,
    });
}

/** A touch event; its touches are plain objects, which is all the filter reads. */
function touchEvent(type, changed, touches = changed) {
    const e = new Event(type, { bubbles: true, cancelable: true });
    return Object.assign(e, { changedTouches: changed, touches });
}

describe('the pen on the picture', () => {
    let view;
    let sent;
    let layer;
    let other;

    beforeEach(() => {
        layer = document.createElement('div');
        other = document.createElement('button');
        document.body.append(layer, other);
        sent = [];
        view = Object.create(StreamViewPen.prototype);
        Object.assign(view, {
            inputEl: layer,
            _rawPointer: false,
            _gamingMode: false,
            _mouseAllowed: true,
            _quitting: false,
            // A 200×100 picture at the top left of the page.
            _mediaRect: () => ({ left: 0, top: 0, width: 200, height: 100 }),
            _sendToHost: (m) => sent.push(m),
        });
        view._bindPen();
    });

    afterEach(() => {
        view._unbindPen();
        layer.remove();
        other.remove();
        vi.restoreAllMocks();
    });

    it('stays the mouse until the relay says the host takes a pen', () => {
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        expect(sent).toEqual([]);
        expect(view._penOwnsMouseEvent()).toBe(false);
        expect(view._lastPointerType).toBe('pen');
    });

    it('sends the stroke, and keeps the browser from making mouse events of it', () => {
        view._setPenSupported(true);
        const down = pointer('pointerdown', { buttons: 1, pressure: 0.5 });
        layer.dispatchEvent(down);
        layer.dispatchEvent(pointer('pointermove', { buttons: 1, pressure: 0.7, clientX: 150 }));
        layer.dispatchEvent(pointer('pointerup', { buttons: 0, clientX: 150 }));
        expect(sent.map((m) => m.event)).toEqual(['down', 'move', 'up']);
        expect(sent[0]).toMatchObject({ type: 'pen', x: 0.5, y: 0.5, pressure: 0.5, tool: 'pen' });
        expect(sent[1]).toMatchObject({ x: 0.75, pressure: 0.7 });
        expect(down.defaultPrevented).toBe(true);
    });

    it('lets the mouse path have a real mouse back as soon as it moves', () => {
        view._setPenSupported(true);
        layer.dispatchEvent(pointer('pointermove'));
        expect(view._penOwnsMouseEvent()).toBe(true);
        layer.dispatchEvent(pointer('pointermove', { pointerType: 'mouse' }));
        expect(view._penOwnsMouseEvent()).toBe(false);
    });

    it('sends every coalesced sample of a move', () => {
        view._setPenSupported(true);
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        const samples = [110, 120, 130].map((clientX) => ({
            clientX,
            clientY: 50,
            buttons: 1,
            pressure: 0.5,
            tiltX: 0,
            tiltY: 30,
        }));
        layer.dispatchEvent(
            pointer('pointermove', { buttons: 1, clientX: 130, getCoalescedEvents: () => samples }),
        );
        const moves = sent.filter((m) => m.event === 'move');
        expect(moves.map((m) => m.x)).toEqual([0.55, 0.6, 0.65]);
        expect(moves[0]).toMatchObject({ tilt: 30, rotation: 0 });
    });

    it('holds the heartbeat open while the pen is down, and only then', () => {
        view._setPenSupported(true);
        layer.dispatchEvent(pointer('pointermove'));
        expect(view._penHeld()).toBe(false);
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        expect(view._penHeld()).toBe(true);
        layer.dispatchEvent(pointer('pointerup', { buttons: 0 }));
        expect(view._penHeld()).toBe(false);
    });

    it('lifts a pen still down when the page lets go of it', () => {
        view._setPenSupported(true);
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        view._penLift();
        expect(sent.map((m) => m.event)).toEqual(['down', 'up', 'leave']);
        view._penLift();
        expect(sent.length).toBe(3);
    });

    it('lifts it too when the host stops taking a pen', () => {
        view._setPenSupported(true);
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        view._setPenSupported(false);
        expect(sent.map((m) => m.event)).toEqual(['down', 'up', 'leave']);
        layer.dispatchEvent(pointer('pointermove', { buttons: 1, pressure: 0.5 }));
        expect(sent.length).toBe(3);
    });

    it('is the mouse again in gaming mode, and for a viewer without the mouse', () => {
        view._setPenSupported(true);
        view._gamingMode = true;
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        view._gamingMode = false;
        view._mouseAllowed = false;
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        expect(sent).toEqual([]);
    });

    it('takes a lost pointer capture under a pen still down for the pen gone', () => {
        view._setPenSupported(true);
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        layer.dispatchEvent(pointer('lostpointercapture'));
        expect(sent.map((m) => m.event)).toEqual(['down', 'up', 'leave']);
    });

    it('keeps a hovering pen over the picture after its capture ends with the up', () => {
        view._setPenSupported(true);
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        layer.dispatchEvent(pointer('pointerup', { buttons: 0 }));
        layer.dispatchEvent(pointer('lostpointercapture'));
        expect(sent.map((m) => m.event)).toEqual(['down', 'up']);
        layer.dispatchEvent(pointer('pointerleave'));
        expect(sent.map((m) => m.event)).toEqual(['down', 'up', 'leave']);
    });

    it('leaves the motion to raw reports while they arrive', () => {
        view._unbindPen();
        view._rawPointer = true;
        view._bindPen();
        view._setPenSupported(true);
        layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
        layer.dispatchEvent(
            pointer('pointerrawupdate', { buttons: 1, pressure: 0.5, clientX: 120 }),
        );
        layer.dispatchEvent(pointer('pointermove', { buttons: 1, pressure: 0.5, clientX: 120 }));
        expect(sent.map((m) => m.event)).toEqual(['down', 'move']);
    });

    describe('its touches, which the trackpad does not see', () => {
        const stylus = (identifier, target, extra = {}) => ({
            identifier,
            target,
            clientX: 100,
            clientY: 50,
            touchType: 'stylus',
            ...extra,
        });
        const finger = (identifier, target) => ({
            identifier,
            target,
            clientX: 20,
            clientY: 20,
            touchType: 'direct',
        });

        it('passes every touch while the pen is not the host’s', () => {
            const e = touchEvent('touchstart', [stylus(1, layer)]);
            expect(view._touchesBesidePen(e)).toBe(e);
        });

        it('swallows an event that was the pen’s alone, from start to end', () => {
            view._setPenSupported(true);
            const start = touchEvent('touchstart', [stylus(1, layer)]);
            expect(view._touchesBesidePen(start)).toBeNull();
            expect(start.defaultPrevented).toBe(true);
            expect(view._touchesBesidePen(touchEvent('touchmove', [stylus(1, layer)]))).toBeNull();
            expect(
                view._touchesBesidePen(touchEvent('touchend', [stylus(1, layer)], [])),
            ).toBeNull();
            // Gone with its end: a later touch with the same number is a finger.
            const next = touchEvent('touchstart', [finger(1, layer)]);
            expect(view._touchesBesidePen(next)).toBe(next);
        });

        it('gives the trackpad the fingers beside the pen', () => {
            view._setPenSupported(true);
            view._touchesBesidePen(touchEvent('touchstart', [stylus(1, layer)]));
            const f = finger(2, layer);
            const both = touchEvent('touchstart', [f], [stylus(1, layer), f]);
            const seen = view._touchesBesidePen(both);
            expect(seen).not.toBeNull();
            expect(Array.from(seen.touches)).toEqual([f]);
            expect(Array.from(seen.changedTouches)).toEqual([f]);
            expect(seen.type).toBe('touchstart');
        });

        it('knows the pen’s touch by its point when the browser does not say stylus', () => {
            view._setPenSupported(true);
            layer.dispatchEvent(pointer('pointerdown', { buttons: 1, pressure: 0.5 }));
            const t = stylus(5, layer, { touchType: undefined });
            expect(view._touchesBesidePen(touchEvent('touchstart', [t]))).toBeNull();
            const far = stylus(6, layer, { touchType: undefined, clientX: 160 });
            const e = touchEvent('touchstart', [far], [t, far]);
            expect(Array.from(view._touchesBesidePen(e).changedTouches)).toEqual([far]);
        });

        it('leaves a pen on a button to be a finger there', () => {
            view._setPenSupported(true);
            const e = touchEvent('touchstart', [stylus(1, other)]);
            expect(view._touchesBesidePen(e)).toBe(e);
        });
    });
});

describe('the held-input heartbeat with a pen down', () => {
    it('beats while the pen is down, closes once after', () => {
        const beats = [];
        const v = {
            _heldPhysKeys: new Map(),
            _heldMouseButtons: new Set(),
            _gamepadManager: null,
            _gamingMode: false,
            _lastBeatHadState: false,
            _penStroke: { contact: true },
            _sendInputState: StreamView.prototype._sendInputState,
            _sendToHost: (m) => beats.push(m),
        };
        v._sendInputState();
        v._sendInputState();
        expect(beats.length).toBe(2);
        v._penStroke.contact = false;
        v._sendInputState();
        v._sendInputState();
        expect(beats.length).toBe(3);
        expect(beats[2]).toMatchObject({ type: 'inputstate', keys: [], buttons: 0 });
    });
});
