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
