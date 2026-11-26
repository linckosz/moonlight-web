/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { Toast } from '../js/ui/Toast.js';

/**
 * IPADPRE = A (26/11/2026): where the lock is never granted (iOS/iPadOS), the
 * refusal is known as gaming mode starts. Before, it was learned at the first
 * click, and until then the moves went out absolute: a game that hides the
 * pointer jumped, and the edge turns did nothing. The first click stays the
 * capture's (P2), and a guest without the mouse sends nothing (P1).
 */
function view({ final = true, mouseAllowed = true, request = vi.fn(() => undefined) } = {}) {
    const inputEl = document.createElement('div');
    inputEl.requestPointerLock = request;
    const hintEl = document.createElement('div');
    hintEl.style.display = 'flex';
    const sent = [];
    const v = Object.create(StreamView.prototype);
    Object.assign(v, {
        _gamingMode: true,
        _mouseAllowed: mouseAllowed,
        _lockRefusalFinal: final,
        _lockFreeLook: final,
        _mouseSensitivity: 1,
        pointerLocked: false,
        inputEl,
        hintEl,
        _disarmPointerCapture() {},
        _syncKeyboardLock() {},
        _updateGamingOverlay() {},
        _sendCursorMode() {},
        _noteMouseSent() {},
        _applyLocalCursor() {},
        _clientCursorMoved() {},
        _clientCursorPlacedAt() {},
        _edgeTurnMove() {},
        _edgeTurnVertActive: () => false,
        _stopEdgeTurn() {},
        _mediaRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
        _sendRelativeMouse: (dx, dy) => sent.push(['rel', dx, dy]),
        _sendToHost: (m) => sent.push(['abs', m.x, m.y]),
        _sendMouseButton: (b, down) => sent.push([down ? 'down' : 'up', b]),
    });
    v.sent = sent;
    v.request = request;
    return v;
}

const move = { movementX: 7, movementY: -3, clientX: 50, clientY: 40, buttons: 0 };
const press = { button: 0, clientX: 50, clientY: 40 };

/** A whole click as the browser gives it: down, up, then click. */
function click(v) {
    const e = { ...press, preventDefault: vi.fn() };
    v._onGamingMouseDown(e);
    v._onGamingMouseUp(e);
    v._onGamingClick(e);
    return e;
}

describe('a lock refusal known before any click', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
        delete document.pointerLockElement;
        StreamView._lockRefusedToastShown = false;
    });

    it('sends the first moves as lock-free look on iOS', () => {
        const toast = vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view();
        v._bindGamingEvents();
        expect(v._pointerLockRefused).toBe(true);
        v._onGamingMouseMove(move);
        expect(v.sent).toEqual([['rel', 7, -3]]);
        // The click to capture is still to come: its hint stays, nothing told yet.
        expect(v.hintEl.style.display).toBe('flex');
        expect(toast).not.toHaveBeenCalled();
    });

    it('keeps the first click from the game, and asks for the lock at it (P2)', () => {
        vi.useFakeTimers();
        const toast = vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view();
        v._bindGamingEvents();
        const first = click(v);
        expect(first.preventDefault).toHaveBeenCalled();
        expect(v.sent).toEqual([]);
        expect(v.request).toHaveBeenCalledTimes(1);
        expect(v.hintEl.style.display).toBe('none');
        expect(toast).toHaveBeenCalledTimes(1);
        vi.advanceTimersByTime(StreamView.LOCK_WATCH_MS);
        // The stream is on: the next click is the game's, and asks nothing.
        click(v);
        expect(v.sent).toEqual([
            ['down', 1],
            ['up', 1],
        ]);
        expect(v.request).toHaveBeenCalledTimes(1);
        expect(toast).toHaveBeenCalledTimes(1);
    });

    it('lets a button pressed before gaming mode go to the game', () => {
        const v = view();
        // Down while desktop mode was on: the up after it is the host's,
        // though the capture's click is still owed.
        v._bindGamingEvents();
        v._onGamingMouseUp(press);
        expect(v._captureClickOwed).toBe(true);
        expect(v.sent).toEqual([['up', 1]]);
    });

    it('sends nothing for a guest without the mouse, moves or clicks (P1)', () => {
        vi.useFakeTimers();
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view({ mouseAllowed: false });
        v._bindGamingEvents();
        expect(v._pointerLockRefused).toBeFalsy();
        v._onGamingMouseMove(move);
        click(v);
        vi.advanceTimersByTime(StreamView.LOCK_WATCH_MS);
        v._onGamingMouseMove(move);
        click(v);
        expect(v.sent).toEqual([]);
        expect(v.request).not.toHaveBeenCalled();
    });

    it('turns no wheel for a guest without the mouse, before or after a click (P1)', () => {
        vi.useFakeTimers();
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view({ mouseAllowed: false });
        const wheel = vi.fn();
        Object.assign(v, {
            _sendWheelTravel: wheel,
            _pinchFromWheel: wheel,
            _realCtrlHeld: () => false,
        });
        v._bindGamingEvents();
        const e = { deltaX: 0, deltaY: 120, deltaMode: 0, timeStamp: 1, preventDefault: vi.fn() };
        v.handleWheel(e);
        click(v);
        vi.advanceTimersByTime(StreamView.LOCK_WATCH_MS);
        v.handleWheel({ ...e, ctrlKey: true });
        v.handleWheel(e);
        expect(wheel).not.toHaveBeenCalled();
        expect(v.sent).toEqual([]);
        // The page never scrolls under the picture all the same.
        expect(e.preventDefault).toHaveBeenCalled();
    });

    it('sends nothing once the mouse is taken back from a guest', () => {
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view();
        v._bindGamingEvents();
        click(v);
        v._mouseAllowed = false;
        v._onGamingMouseMove(move);
        click(v);
        expect(v.sent).toEqual([]);
    });

    it('is known when the owner gives the mouse to a guest on iOS', () => {
        vi.spyOn(console, 'log').mockImplementation(() => {});
        const v = view({ mouseAllowed: false });
        v._bindGamingEvents();
        v._setMouseAllowed(true);
        expect(v._pointerLockRefused).toBe(true);
        v._onGamingMouseMove(move);
        click(v);
        expect(v.sent).toEqual([['rel', 7, -3]]);
    });

    it('changes nothing off iOS: absolute moves until a click is refused', () => {
        const v = view({ final: false });
        v._bindGamingEvents();
        expect(v._pointerLockRefused).toBeFalsy();
        v._onGamingMouseMove(move);
        expect(v.sent).toEqual([['abs', 50, 40]]);
    });

    it('is forgotten if the first click gets the lock after all', () => {
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view();
        v._bindGamingEvents();
        click(v);
        Object.defineProperty(document, 'pointerLockElement', {
            configurable: true,
            get: () => v.inputEl,
        });
        v.handlePointerLockChange();
        expect(v._pointerLockRefused).toBe(false);
        expect(v._captureClickOwed).toBe(false);
        expect(v._mouseFocused).toBe(true);
    });

    it('logs how the first click was refused', () => {
        vi.useFakeTimers();
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const log = vi.spyOn(console, 'log').mockImplementation(() => {});
        const v = view();
        v._bindGamingEvents();
        click(v);
        vi.advanceTimersByTime(StreamView.LOCK_WATCH_MS);
        expect(log.mock.calls.some((c) => /refused again \(no pointerlockchange/.test(c[0]))).toBe(
            true,
        );
    });
});
