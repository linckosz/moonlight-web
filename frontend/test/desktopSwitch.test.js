/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, afterEach } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * Ctrl+Alt+Shift+Left/Right (Ctrl+Option+Cmd+Left/Right on a Mac client) moves
 * the HOST one desktop over. The combo never reaches the host: it is caught
 * client-side and replaced by the chord that host's own OS uses for desktops.
 *
 * The delicate part is not the chord but what is already down when it fires.
 * The user is holding three modifiers, and those went to the host as ordinary
 * keystrokes long before the arrow arrived — so the chord has to clear them
 * first, and swallow the physical release that comes later.
 */

const VK_CONTROL = 0x11;
const VK_MENU = 0x12; // Alt
const VK_LWIN = 0x5b;
const VK_LEFT = 0x25;
const VK_RIGHT = 0x27;

/** Minimal stand-in exposing just what the desktop-switch path touches. */
function keySink(hostOs, overrides = {}) {
    const sent = [];
    return {
        sent,
        host: hostOs === undefined ? null : { hostOs },
        webrtc: { send: (m) => sent.push(m) },
        _heldPhysKeys: new Map(),
        _metaTapCodes: new Set(),
        _swallowKeyUpCodes: new Set(),
        _appleKeyboard: false,
        _quitting: false,
        _kbdCapture: null,
        _locksSynced: true,
        _cssFullscreen: false,
        _clipboardEnabled: false,
        _layoutMap: null,
        _gamingMode: false,
        _pendingPasteKey: null,
        _suppressPasteKeyUpCode: null,
        handleKeyDown: StreamView.prototype.handleKeyDown,
        handleKeyUp: StreamView.prototype.handleKeyUp,
        sendDesktopSwitch: StreamView.prototype.sendDesktopSwitch,
        _releaseHeldModifiers: StreamView.prototype._releaseHeldModifiers,
        _sendKeyEvent: StreamView.prototype._sendKeyEvent,
        _forgetHeldKey: StreamView.prototype._forgetHeldKey,
        _holdsThroughStall: StreamView.prototype._holdsThroughStall,
        _releaseKeysHeldUnderMeta: StreamView.prototype._releaseKeysHeldUnderMeta,
        _sendPendingPasteKey: StreamView.prototype._sendPendingPasteKey,
        _syncLockState: () => {},
        ...overrides,
    };
}

/** A KeyboardEvent-shaped plain object (see macMetaKeys.test.js). */
function ev(code, key, mods = {}) {
    return {
        target: {},
        code,
        key,
        keyCode: 0,
        repeat: false,
        ctrlKey: false,
        shiftKey: false,
        altKey: false,
        metaKey: false,
        preventDefault() {},
        ...mods,
    };
}

/** The Win/Linux combo: Ctrl+Alt+Shift+arrow. */
const winCombo = { ctrlKey: true, altKey: true, shiftKey: true };

const pairs = (v) => v.sent.map((m) => [m.type, m.keyCode]);

afterEach(() => vi.unstubAllGlobals());

describe('the chord that goes out, by host OS', () => {
    it('sends Ctrl+Win+Right for a Windows host', () => {
        const v = keySink('windows');
        v.sendDesktopSwitch(1);

        expect(pairs(v)).toEqual([
            ['keydown', VK_CONTROL],
            ['keydown', VK_LWIN],
            ['keydown', VK_RIGHT],
            ['keyup', VK_RIGHT],
            ['keyup', VK_LWIN],
            ['keyup', VK_CONTROL],
        ]);
        // The Windows key is a modifier on the wire too: the host reads the
        // bitmask, not just the VK codes.
        expect(v.sent.every((m) => m.metaKey === true && m.altKey === false)).toBe(true);
        // Nothing left hanging for the held-input heartbeat to re-assert.
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('sends Ctrl+Alt+Left for a Linux host', () => {
        const v = keySink('linux');
        v.sendDesktopSwitch(-1);

        expect(pairs(v)).toEqual([
            ['keydown', VK_CONTROL],
            ['keydown', VK_MENU],
            ['keydown', VK_LEFT],
            ['keyup', VK_LEFT],
            ['keyup', VK_MENU],
            ['keyup', VK_CONTROL],
        ]);
        expect(v.sent.every((m) => m.metaKey === false && m.altKey === true)).toBe(true);
    });

    it('sends Ctrl+Left alone for a macOS host', () => {
        const v = keySink('macos');
        v.sendDesktopSwitch(-1);

        expect(pairs(v)).toEqual([
            ['keydown', VK_CONTROL],
            ['keydown', VK_LEFT],
            ['keyup', VK_LEFT],
            ['keyup', VK_CONTROL],
        ]);
    });

    it('falls back to the Windows chord when the host OS is unknown or absent', () => {
        for (const os of ['unknown', undefined]) {
            const v = keySink(os);
            v.sendDesktopSwitch(1);
            expect(pairs(v)).toEqual([
                ['keydown', VK_CONTROL],
                ['keydown', VK_LWIN],
                ['keydown', VK_RIGHT],
                ['keyup', VK_RIGHT],
                ['keyup', VK_LWIN],
                ['keyup', VK_CONTROL],
            ]);
        }
    });
});

describe('the modifiers the user is holding', () => {
    /**
     * Press Ctrl, then Alt, then Shift — as a real chord arrives.
     *
     * Only the first two reach the host: the Shift press already completes a
     * triple-modifier combo, and those are held back client-side (the guard at
     * the end of the combo block). So the host holds Ctrl+Alt, and that is what
     * has to be cleared before the chord.
     */
    function holdWinCombo(v) {
        v.handleKeyDown(ev('ControlLeft', 'Control', { ctrlKey: true }));
        v.handleKeyDown(ev('AltLeft', 'Alt', { ctrlKey: true, altKey: true }));
        v.handleKeyDown(ev('ShiftLeft', 'Shift', winCombo));
    }

    it('releases them on the host before the chord, so it arrives clean', () => {
        const v = keySink('windows');
        holdWinCombo(v);
        v.sent.length = 0;

        v.handleKeyDown(ev('ArrowRight', 'ArrowRight', winCombo));

        // The releases first, then the chord — never Ctrl+Alt+Win+Right.
        expect(pairs(v).slice(0, 2)).toEqual([
            ['keyup', VK_CONTROL],
            ['keyup', VK_MENU],
        ]);
        expect(pairs(v).slice(2)).toEqual([
            ['keydown', VK_CONTROL],
            ['keydown', VK_LWIN],
            ['keydown', VK_RIGHT],
            ['keyup', VK_RIGHT],
            ['keyup', VK_LWIN],
            ['keyup', VK_CONTROL],
        ]);
    });

    it('swallows their physical keyup, which is still to come', () => {
        const v = keySink('windows');
        holdWinCombo(v);
        v.handleKeyDown(ev('ArrowRight', 'ArrowRight', winCombo));
        v.sent.length = 0;

        v.handleKeyUp(ev('AltLeft', 'Alt'));
        v.handleKeyUp(ev('ControlLeft', 'Control'));

        expect(v.sent).toEqual([]);
        expect(v._swallowKeyUpCodes.size).toBe(0);
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('leaves a non-modifier key alone', () => {
        const v = keySink('windows');
        v.handleKeyDown(ev('KeyW', 'w'));
        v.sent.length = 0;

        v.sendDesktopSwitch(1);
        expect(v.sent.some((m) => m.code === 'KeyW')).toBe(false);
        expect(v._heldPhysKeys.has('KeyW')).toBe(true);
    });
});

describe('the three-finger swipe (touch)', () => {
    /** Stand-in for the touch path's 3-finger branch. */
    function touchSink(zoom = 1) {
        const switches = [];
        return {
            switches,
            // Mid-gesture state: the fingers are already down and the handler
            // has already seen three of them (a change in count reseeds and
            // skips a frame).
            _touchActive: true,
            _lastMoveFingerCount: 3,
            _touchFingerCount: 3,
            _seedMultiTouch: () => {},
            _zoom: zoom,
            _panX: 0,
            _panY: 0,
            _panSamples: [],
            _pinchPrevCx: null,
            _pinchPrevCy: null,
            _touchMoved: false,
            _threeFingerDx: 0,
            _threeFingerDy: 0,
            _threeFingerSwitched: false,
            _clearLongPress: () => {},
            // Stands in for the real tolerance check: travel from the first
            // centroid of the gesture, so a short drag still reads as a tap.
            _multiStartX: null,
            _multiStartY: null,
            _multiMovedBeyondTol(cx, cy, dist, tol) {
                if (this._multiStartX == null) {
                    this._multiStartX = cx;
                    this._multiStartY = cy;
                }
                return Math.hypot(cx - this._multiStartX, cy - this._multiStartY) > tol;
            },
            _applyZoomTransform: () => {},
            sendDesktopSwitch: (dir) => switches.push(dir),
            handleTouchMove: StreamView.prototype.handleTouchMove,
        };
    }

    /** Drag three fingers by (dx, dy) per step, `steps` times. */
    function swipe(v, dx, dy, steps) {
        let x = 200;
        let y = 300;
        for (let i = 0; i < steps; i++) {
            x += dx;
            y += dy;
            const touches = [
                { clientX: x, clientY: y },
                { clientX: x + 40, clientY: y },
                { clientX: x + 80, clientY: y },
            ];
            touches.length = 3;
            v.handleTouchMove({
                // Not a UI button and not the stats card: the handler asks the
                // target itself, so it has to answer.
                target: { closest: () => null },
                touches,
                preventDefault() {},
            });
        }
    }

    it('moves one desktop right when the fingers go left', () => {
        const v = touchSink();
        swipe(v, -12, 0, 10);
        expect(v.switches).toEqual([1]);
    });

    it('moves one desktop left when the fingers go right', () => {
        const v = touchSink();
        swipe(v, 12, 0, 10);
        expect(v.switches).toEqual([-1]);
    });

    it('switches once per gesture, however far the fingers travel', () => {
        const v = touchSink();
        swipe(v, -12, 0, 40);
        expect(v.switches).toEqual([1]);
    });

    it('stays silent below the threshold — the 3-finger tap still opens the keyboard', () => {
        const v = touchSink();
        swipe(v, -5, 0, 4); // ~20px, under the 60px threshold
        expect(v.switches).toEqual([]);
        expect(v._touchMoved).toBe(false);
    });

    it('ignores a mostly-vertical drag', () => {
        const v = touchSink();
        swipe(v, -6, -20, 10);
        expect(v.switches).toEqual([]);
    });

    it('pans instead of switching when the display is zoomed in', () => {
        const v = touchSink(2);
        swipe(v, -12, 0, 10);
        expect(v.switches).toEqual([]);
        expect(v._panX).toBeLessThan(0);
    });
});

describe('the combo that triggers it', () => {
    it('never forwards the arrow keystroke itself', () => {
        const v = keySink('windows');
        v.handleKeyDown(ev('ArrowLeft', 'ArrowLeft', winCombo));
        // Everything sent is synthetic (no e.code): the user's own ArrowLeft
        // press stays client-side.
        expect(v.sent.some((m) => m.code === 'ArrowLeft')).toBe(false);
    });

    it('maps Left to the previous desktop and Right to the next one', () => {
        const left = keySink('windows');
        left.handleKeyDown(ev('ArrowLeft', 'ArrowLeft', winCombo));
        expect(pairs(left)[2]).toEqual(['keydown', VK_LEFT]);

        const right = keySink('windows');
        right.handleKeyDown(ev('ArrowRight', 'ArrowRight', winCombo));
        expect(pairs(right)[2]).toEqual(['keydown', VK_RIGHT]);
    });

    it('ignores auto-repeat: one held arrow must not run through every desktop', () => {
        const v = keySink('windows');
        v.handleKeyDown(ev('ArrowRight', 'ArrowRight', { ...winCombo, repeat: true }));
        expect(v.sent).toEqual([]);
    });

    it('takes Ctrl+Option+Cmd on a Mac client', () => {
        vi.stubGlobal('navigator', { platform: 'MacIntel' });
        const v = keySink('windows');
        v.handleKeyDown(
            ev('ArrowRight', 'ArrowRight', { metaKey: true, altKey: true, ctrlKey: true }),
        );
        expect(pairs(v)).toEqual([
            ['keydown', VK_CONTROL],
            ['keydown', VK_LWIN],
            ['keydown', VK_RIGHT],
            ['keyup', VK_RIGHT],
            ['keyup', VK_LWIN],
            ['keyup', VK_CONTROL],
        ]);
    });

    it('leaves a bare arrow key alone', () => {
        const v = keySink('windows');
        v.handleKeyDown(ev('ArrowRight', 'ArrowRight'));
        expect(pairs(v)).toEqual([['keydown', VK_RIGHT]]);
        expect(v._heldPhysKeys.has('ArrowRight')).toBe(true);
    });
});
