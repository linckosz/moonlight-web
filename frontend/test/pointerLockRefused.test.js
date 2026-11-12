/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { Toast } from '../js/ui/Toast.js';
import { t } from '../js/i18n/i18n.js';

/**
 * Gaming mode on a browser that never grants the pointer lock (Safari on an
 * iPad with a trackpad, 27/10/2026): a click asks for the lock, and once the
 * browser has said no, clicks go to the host and the hint stops asking.
 */
function view({ request }) {
    const inputEl = document.createElement('div');
    inputEl.requestPointerLock = request;
    const hintEl = document.createElement('div');
    hintEl.style.display = 'flex';
    const v = Object.create(StreamView.prototype);
    Object.assign(v, {
        _gamingMode: true,
        _mouseAllowed: true,
        pointerLocked: false,
        inputEl,
        hintEl,
        _disarmPointerCapture() {},
        _syncKeyboardLock() {},
        _updateGamingOverlay() {},
        _sendCursorMode() {},
    });
    return v;
}

describe('a refused pointer lock', () => {
    afterEach(() => {
        vi.useRealTimers();
        vi.restoreAllMocks();
        delete document.pointerLockElement;
        StreamView._lockRefusedToastShown = false;
    });

    it('is taken at its silence after a click, once', () => {
        vi.useFakeTimers();
        const toast = vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view({ request: () => undefined });
        v._lockPointerOnClick();
        expect(v._pointerLockRefused).toBeFalsy();
        vi.advanceTimersByTime(StreamView.LOCK_WATCH_MS);
        expect(v._pointerLockRefused).toBe(true);
        expect(v.hintEl.style.display).toBe('none');
        v._pointerLockRefusedForGood('again');
        expect(toast).toHaveBeenCalledTimes(1);
    });

    it('is taken at a pointerlockerror that follows a click', () => {
        vi.useFakeTimers();
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view({ request: () => undefined });
        v._lockPointerOnClick();
        v._handlePointerLockError();
        expect(v._pointerLockRefused).toBe(true);
    });

    it('means nothing without a click (the first picture asks on its own)', () => {
        const v = view({ request: () => undefined });
        v._handlePointerLockError();
        expect(v._pointerLockRefused).toBeFalsy();
    });

    it('covers a browser with no lock at all', async () => {
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view({ request: undefined });
        v._lockPointerOnClick();
        await Promise.resolve();
        await Promise.resolve();
        expect(v._pointerLockRefused).toBe(true);
    });

    it('sends the motion relative with the lab key, absolute without', () => {
        const sent = [];
        const v = view({ request: () => undefined });
        Object.assign(v, {
            _pointerLockRefused: true,
            _sendRelativeMouse: (dx, dy) => sent.push(['rel', dx, dy]),
            _sendToHost: (m) => sent.push(['abs', m.x, m.y]),
            _noteMouseSent() {},
            _applyLocalCursor() {},
            _mediaRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
        });
        v._bindGamingEvents();
        v._lockFreeLook = true;
        v._onGamingMouseMove({ movementX: 7, movementY: -3, clientX: 50, clientY: 50 });
        v._lockFreeLook = false;
        v._onGamingMouseMove({ movementX: 7, movementY: -3, clientX: 50, clientY: 50 });
        expect(sent).toEqual([
            ['rel', 7, -3],
            ['abs', 50, 50],
        ]);
    });

    it('is asked again at the next click off iOS (a refusal may be a moment)', () => {
        vi.useFakeTimers();
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const request = vi.fn(() => undefined);
        const v = view({ request });
        Object.assign(v, {
            _mediaRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
            _sendToHost() {},
        });
        v._bindGamingEvents();
        v._onGamingClick({ preventDefault() {}, clientX: 1, clientY: 1 });
        vi.advanceTimersByTime(StreamView.LOCK_WATCH_MS);
        expect(v._pointerLockRefused).toBe(true);
        v._onGamingClick({ preventDefault() {}, clientX: 1, clientY: 1 });
        expect(request).toHaveBeenCalledTimes(2);
    });

    it('is final on iOS: the next click is only a click', () => {
        vi.useFakeTimers();
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const request = vi.fn(() => undefined);
        const v = view({ request });
        Object.assign(v, {
            _mediaRect: () => ({ left: 0, top: 0, width: 100, height: 100 }),
            _sendToHost() {},
        });
        v._lockRefusalFinal = true;
        v._bindGamingEvents();
        v._onGamingClick({ preventDefault() {}, clientX: 1, clientY: 1 });
        vi.advanceTimersByTime(StreamView.LOCK_WATCH_MS);
        v._onGamingClick({ preventDefault() {}, clientX: 1, clientY: 1 });
        expect(request).toHaveBeenCalledTimes(1);
    });

    it('says the game gets movement when lock-free look is on', () => {
        const toast = vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view({ request: () => undefined });
        v._lockFreeLook = true;
        v._pointerLockRefusedForGood('test');
        expect(toast.mock.calls[0][0]).toBe(t('stream.pointerLockRefusedLookFree'));
        expect(t('stream.pointerLockRefusedLookFree')).not.toBe(t('stream.pointerLockRefused'));
    });

    it('is forgotten when the lock comes after all', () => {
        vi.useFakeTimers();
        vi.spyOn(Toast, 'info').mockImplementation(() => {});
        const v = view({ request: () => undefined });
        v._lockPointerOnClick();
        vi.advanceTimersByTime(StreamView.LOCK_WATCH_MS);
        Object.defineProperty(document, 'pointerLockElement', {
            configurable: true,
            get: () => v.inputEl,
        });
        v.handlePointerLockChange();
        expect(v._pointerLockRefused).toBe(false);
        expect(v._mouseFocused).toBe(true);
    });
});
