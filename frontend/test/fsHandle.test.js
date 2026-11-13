/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
/* global document, performance, Event */
import { describe, it, expect, afterEach, vi } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';
import { gestureRows } from '../js/util/shortcutsHelp.js';

/**
 * #62: in CSS fullscreen (iOS) the header is gone. A handle at the top, or a
 * three-finger tap, brings it down for 4 s; a second three-finger tap while it
 * is there opens the keyboard.
 */
function view({ hideHandle = false } = {}) {
    const root = document.createElement('div');
    root.innerHTML = `
        <div class="stream-header"><button id="btn-stream-quit">Stop</button></div>
        <div class="stream-canvas-area"></div>`;
    document.body.appendChild(root);
    const v = Object.create(StreamView.prototype);
    Object.assign(v, {
        _rootEl: root,
        _cssFullscreen: false,
        _quitting: false,
        _installHintShown: true,
        _hideFsHandle: hideHandle,
        _fsHandleEl: null,
        _fsBarTimer: null,
        _fsBarWired: false,
        _kbdVisible: false,
        _mobileFsBtn: null,
        _isDeviceLandscape: () => true,
        _showTransientHint: vi.fn(),
        _toggleVirtualKeyboard: vi.fn(),
    });
    return v;
}

/** A clean three-finger tap, as handleTouchEnd sees its last finger leave. */
function threeFingerTap(v) {
    Object.assign(v, {
        _touchActive: true,
        _touchMaxFingers: 3,
        _touchMoved: false,
        _touchDragging: false,
        _touchStartTime: performance.now(),
        _touchStartX: 10,
        _touchStartY: 10,
        _scrollSamples: [],
        _panSamples: [],
        _touchPadAvailable: () => false,
        _clearLongPress() {},
    });
    const target = document.createElement('div');
    v.handleTouchEnd({
        type: 'touchend',
        target,
        touches: [],
        changedTouches: [{ clientX: 10, clientY: 10 }],
        preventDefault() {},
    });
}

describe('the CSS-fullscreen way back to the header', () => {
    afterEach(() => {
        vi.useRealTimers();
        document.body.innerHTML = '';
    });

    it('puts a handle at the top that brings the header down', () => {
        const v = view();
        v._enterCssFallbackFullscreen();
        const handle = v._rootEl.querySelector('.stream-fs-handle');
        expect(handle).not.toBeNull();
        handle.click();
        expect(v._fsBarShown()).toBe(true);
    });

    it('leaves the handle out when the settings hide it', () => {
        const v = view({ hideHandle: true });
        v._enterCssFallbackFullscreen();
        expect(v._rootEl.querySelector('.stream-fs-handle')).toBeNull();
    });

    it('shows the header at the first three-finger tap, the keyboard at the next', () => {
        vi.useFakeTimers();
        const v = view();
        v._enterCssFallbackFullscreen();
        threeFingerTap(v);
        expect(v._fsBarShown()).toBe(true);
        expect(v._toggleVirtualKeyboard).not.toHaveBeenCalled();
        threeFingerTap(v);
        expect(v._toggleVirtualKeyboard).toHaveBeenCalledTimes(1);
    });

    it('opens the keyboard at once out of fullscreen, as before', () => {
        const v = view();
        threeFingerTap(v);
        expect(v._toggleVirtualKeyboard).toHaveBeenCalledTimes(1);
        expect(v._fsBarShown()).toBe(false);
    });

    it('puts the header away after 4 s, and a press in it gives 4 s more', () => {
        vi.useFakeTimers();
        const v = view();
        v._enterCssFallbackFullscreen();
        v._showFsBar();
        vi.advanceTimersByTime(3000);
        v._rootEl.querySelector('.stream-header').dispatchEvent(new Event('pointerdown'));
        vi.advanceTimersByTime(3000);
        expect(v._fsBarShown()).toBe(true);
        vi.advanceTimersByTime(1100);
        expect(v._fsBarShown()).toBe(false);
    });

    it('keeps the header while the keyboard is up', () => {
        vi.useFakeTimers();
        const v = view();
        v._enterCssFallbackFullscreen();
        v._showFsBar();
        v._kbdVisible = true;
        vi.advanceTimersByTime(9000);
        expect(v._fsBarShown()).toBe(true);
        v._kbdVisible = false;
        vi.advanceTimersByTime(4100);
        expect(v._fsBarShown()).toBe(false);
    });

    it('lets the header go when fullscreen ends', () => {
        const v = view();
        v._updateMobileFsButtonVisibility = () => {};
        v._enterCssFallbackFullscreen();
        v._showFsBar();
        v._exitCssFallbackFullscreen();
        expect(v._fsBarShown()).toBe(false);
    });

    it('names the three-finger tap that brings the controls', () => {
        for (const touchScreen of [false, true]) {
            const row = gestureRows(touchScreen).find((r) => r[0] === 'stream.tcControls');
            expect(row[1]).toBe('stream.tcControlsVal');
        }
    });
});
