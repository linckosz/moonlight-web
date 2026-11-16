/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
/* global document */
import { describe, it, expect } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * IPADAIM: on a tablet the page draws the host's pointer itself. With the lock
 * refused (Safari on iPadOS), lock-free look sent the motion to the host but
 * left that drawing where it was, so the viewer had nothing to aim with. The
 * drawing now starts where the host's pointer was put and follows the same
 * motion the host is sent.
 */
function view() {
    const inputEl = document.createElement('div');
    const sent = [];
    const v = Object.create(StreamView.prototype);
    Object.assign(v, {
        inputEl,
        _gamingMode: true,
        _mouseAllowed: true,
        _mouseFocused: false,
        pointerLocked: false,
        _pointerLockRefused: false,
        _lockFreeLook: true,
        _lockRefusalFinal: true,
        _mouseSensitivity: 1,
        _mouseCarryX: 0,
        _mouseCarryY: 0,
        _hostCursorScale: 1,
        _clientCursorPos: null,
        _clientCursorEl: null,
        _clientCursorActive: () => true,
        _pictureWidth: () => 1920,
        _pictureHeight: () => 1080,
        _mediaRect: () => ({ left: 0, top: 0, width: 960, height: 540 }),
        _sendToHost: (m) => sent.push(m),
        _noteMouseSent() {},
        _applyLocalCursor() {},
        _disarmPointerCapture() {},
        _lockPointerOnClick() {},
    });
    v._bindGamingEvents();
    return { v, sent };
}

describe('lock-free look on a tablet', () => {
    it('starts the drawn pointer where the click put the host one', () => {
        const { v, sent } = view();
        v._onGamingClick({ preventDefault() {}, clientX: 480, clientY: 270 });
        expect(sent[0]).toMatchObject({ type: 'mousemove', x: 480, y: 270 });
        // Half of a 960×540 picture is the middle of a 1920×1080 frame.
        expect(v._clientCursorPos).toEqual({ x: 960, y: 540 });
    });

    it('moves the drawn pointer by the motion the host is sent', () => {
        const { v, sent } = view();
        v._onGamingClick({ preventDefault() {}, clientX: 480, clientY: 270 });
        v._pointerLockRefused = true;
        sent.length = 0;
        v._onGamingMouseMove({ movementX: 30, movementY: -20, clientX: 0, clientY: 0 });
        expect(sent).toEqual([{ type: 'mousemove', dx: 30, dy: -20 }]);
        expect(v._clientCursorPos).toEqual({ x: 990, y: 520 });
    });

    it('keeps the drawn pointer inside the picture', () => {
        const { v } = view();
        v._onGamingClick({ preventDefault() {}, clientX: 950, clientY: 10 });
        v._pointerLockRefused = true;
        v._onGamingMouseMove({ movementX: 500, movementY: -500, clientX: 0, clientY: 0 });
        expect(v._clientCursorPos).toEqual({ x: 1919, y: 0 });
    });

    it("puts the drawn pointer back on the host's word at every report", () => {
        const { v } = view();
        v._onGamingClick({ preventDefault() {}, clientX: 480, clientY: 270 });
        // Before the refusal the host's word comes only once (the finger and
        // the absolute moves own the position).
        v._clientCursorHostSaid({ x: 10, y: 10, visible: true });
        expect(v._clientCursorPos).toEqual({ x: 960, y: 540 });
        v._pointerLockRefused = true;
        v._onGamingMouseMove({ movementX: 30, movementY: 0, clientX: 0, clientY: 0 });
        // Windows' acceleration took the real pointer further than the delta.
        v._clientCursorHostSaid({ x: 1100, y: 540, visible: true });
        expect(v._clientCursorPos).toEqual({ x: 1100, y: 540 });
        // A hidden pointer says nothing about where it is.
        v._clientCursorHostSaid({ x: 0, y: 0, visible: false });
        expect(v._clientCursorPos).toEqual({ x: 1100, y: 540 });
    });

    it('keeps a report from off the picture on its edge', () => {
        const { v } = view();
        v._onGamingClick({ preventDefault() {}, clientX: 480, clientY: 270 });
        v._pointerLockRefused = true;
        v._clientCursorHostSaid({ x: 5000, y: -40, visible: true });
        expect(v._clientCursorPos).toEqual({ x: 1919, y: 0 });
    });
});

/**
 * IPADHYB: lock-free look sends deltas only while the host hides its pointer.
 * With it shown (a desktop, a menu), the moves go out absolute, so the host's
 * pointer stays under the viewer's own, which iPadOS draws and the page can
 * neither move nor hide.
 */
describe('lock-free look follows whether the host shows its pointer', () => {
    function refused({ drawn = true, shown = true } = {}) {
        const r = view();
        r.v._onGamingClick({ preventDefault() {}, clientX: 480, clientY: 270 });
        r.v._pointerLockRefused = true;
        r.v._hostDrawsCursor = drawn;
        r.v._hostCursorVisible = shown;
        r.sent.length = 0;
        return r;
    }
    const move = (v, x, y, buttons = 0) =>
        v._onGamingMouseMove({ movementX: 7, movementY: 3, clientX: x, clientY: y, buttons });

    it('moves the host pointer to the viewer one while it is shown', () => {
        const { v, sent } = refused();
        move(v, 240, 135);
        expect(sent).toEqual([
            { type: 'mousemove', x: 240, y: 135, referenceWidth: 960, referenceHeight: 540 },
        ]);
        expect(v._clientCursorPos).toEqual({ x: 480, y: 270 });
        // The host's word is late there: the viewer's pointer owns the place.
        v._clientCursorHostSaid({ x: 10, y: 10, visible: true });
        expect(v._clientCursorPos).toEqual({ x: 480, y: 270 });
    });

    it('sends deltas while the host hides its pointer', () => {
        const { v, sent } = refused({ shown: false });
        move(v, 240, 135);
        expect(sent).toEqual([{ type: 'mousemove', dx: 7, dy: 3 }]);
    });

    it('keeps deltas when the host never says whether it shows it', () => {
        const { v, sent } = refused({ drawn: false, shown: false });
        move(v, 240, 135);
        expect(sent).toEqual([{ type: 'mousemove', dx: 7, dy: 3 }]);
    });

    it('switches with the host pointer, but not in the middle of a press', () => {
        const { v, sent } = refused();
        move(v, 240, 135, 1);
        // Windows hides the pointer during some drags: the drag stays absolute.
        v._hostCursorVisible = false;
        move(v, 250, 140, 1);
        expect(sent.map((m) => 'x' in m)).toEqual([true, true]);
        // Buttons up: the hidden pointer means a game's camera, deltas.
        move(v, 260, 145, 0);
        expect(sent[2]).toEqual({ type: 'mousemove', dx: 7, dy: 3 });
        // Shown again: back under the viewer's pointer.
        v._hostCursorVisible = true;
        move(v, 300, 200, 0);
        expect(sent[3]).toMatchObject({ x: 300, y: 200 });
    });
});
