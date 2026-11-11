/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * A modifier whose key-up the local OS ate stays down on the host.
 *
 * Press the Windows key and the Start menu takes the focus; the browser sees
 * the keydown and never the keyup. Blur catches most of it, not all. On a
 * macOS host the damage then outlives the slip, because there a modifier is
 * not a key the OS holds for us but a flag the host puts on every event it
 * injects: Command left down turned the Space bar into Cmd+Space, and
 * Spotlight opened instead of the character jumping.
 *
 * Every keyboard event states the whole modifier state, so the truth is on
 * each one of them. These tests pin that the client reads it and releases
 * what it wrongly believes is held — and that it does not touch a chord the
 * viewer is genuinely holding.
 */

const VK_SPACE = 0x20;
const VK_W = 0x57;
const VK_LWIN = 0x5b;
const VK_LCTRL = 0x11;

/** Minimal stand-in exposing just what the keyboard path touches. */
function keySink(overrides = {}) {
    const sent = [];
    return {
        sent,
        webrtc: { send: (m) => sent.push(m) },
        _heldPhysKeys: new Map(),
        _metaTapCodes: new Set(),
        _swallowKeyUpCodes: new Set(),
        // A PC keyboard: none of the Apple Cmd workarounds are in play.
        _appleKeyboard: false,
        _quitting: false,
        _pendingClipboardWrite: null,
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
        _sendKeyEvent: StreamView.prototype._sendKeyEvent,
        _reconcileModifiers: StreamView.prototype._reconcileModifiers,
        _markAltGrCtrl: StreamView.prototype._markAltGrCtrl,
        _forgetHeldKey: StreamView.prototype._forgetHeldKey,
        _holdsThroughStall: StreamView.prototype._holdsThroughStall,
        _releaseKeysHeldUnderMeta: StreamView.prototype._releaseKeysHeldUnderMeta,
        _releaseHeldMeta: StreamView.prototype._releaseHeldMeta,
        _sendPendingPasteKey: StreamView.prototype._sendPendingPasteKey,
        ...overrides,
    };
}

/** A KeyboardEvent-shaped plain object, with no getModifierState: the shape
 *  the older browsers and the soft keyboard's own events have. */
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

/** The same, with the normative getModifierState a real browser provides. */
function evState(code, key, held = [], mods = {}) {
    const e = ev(code, key, mods);
    e.getModifierState = (name) => held.includes(name);
    return e;
}

/** The Windows key going down and its key-up never arriving. */
function stickMeta(v) {
    v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));
    v.sent.length = 0;
}

describe('a modifier the local OS stopped talking about', () => {
    it('is released before the keystroke that gives it away', () => {
        const v = keySink();
        stickMeta(v);

        v.handleKeyDown(ev('Space', ' '));

        // The release goes out FIRST: the host must not see this Space with
        // Command still on it, which is the whole bug.
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keyup', VK_LWIN],
            ['keydown', VK_SPACE],
        ]);
        expect(v._heldPhysKeys.has('MetaLeft')).toBe(false);
    });

    it('is cleared by a repeating movement key, with nothing asked of the player', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyW', 'w'));
        stickMeta(v);

        // OS auto-repeat of the key already held: the branch that replays it
        // sits BELOW the reconciliation on purpose.
        v.handleKeyDown(ev('KeyW', 'w', { repeat: true }));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keyup', VK_LWIN],
            ['keydown', VK_W],
        ]);
    });

    it('is released on the way up too, above the branches that return early', () => {
        const v = keySink();
        v.handleKeyDown(ev('Space', ' '));
        stickMeta(v);

        v.handleKeyUp(ev('Space', ' '));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keyup', VK_LWIN],
            ['keyup', VK_SPACE],
        ]);
    });

    it('takes getModifierState over the event flags when the browser has it', () => {
        const v = keySink();
        v.handleKeyDown(evState('ControlLeft', 'Control', ['Control'], { ctrlKey: true }));
        v.sent.length = 0;

        // The flags still say Control — getModifierState is the truth.
        v.handleKeyDown(evState('Space', ' ', [], { ctrlKey: true }));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keyup', VK_LCTRL],
            ['keydown', VK_SPACE],
        ]);
    });

    it('leaves a chord the viewer is really holding alone', () => {
        const v = keySink();
        stickMeta(v);

        v.handleKeyDown(ev('Space', ' ', { metaKey: true }));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keydown', VK_SPACE]]);
        expect(v._heldPhysKeys.has('MetaLeft')).toBe(true);
    });

    it('does not release the modifier whose own event this is', () => {
        const v = keySink();
        stickMeta(v);

        // Meta finally coming up: one key-up, not two.
        v.handleKeyUp(ev('MetaLeft', 'Meta'));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keyup', VK_LWIN]]);
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('never speaks for Caps Lock, which is a lock and not a held key', () => {
        const v = keySink();
        v.handleKeyDown(ev('CapsLock', 'CapsLock'));
        v.sent.length = 0;

        v.handleKeyDown(evState('Space', ' ', ['CapsLock']));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keydown', VK_SPACE]]);
        expect(v._heldPhysKeys.has('CapsLock')).toBe(true);
    });

    it('leaves a toolbar-latched modifier latched', () => {
        const v = keySink();
        // The soft keyboard latches a modifier with no physical key behind it,
        // so _sendKeyEvent files it under its numeric id.
        v._sendKeyEvent.call(v, { type: 'keydown', keyCode: VK_LWIN, code: '', key: 'Meta' });
        v.sent.length = 0;

        v.handleKeyDown(ev('Space', ' '));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keydown', VK_SPACE]]);
        expect(v._heldPhysKeys.has(VK_LWIN)).toBe(true);
    });

    it('does not arm a swallow token, which would eat the next real release', () => {
        const v = keySink();
        stickMeta(v);
        v.handleKeyDown(ev('Space', ' '));
        expect(v._swallowKeyUpCodes.size).toBe(0);

        // The viewer presses and releases the Windows key for real: both edges
        // reach the host.
        v.sent.length = 0;
        v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));
        v.handleKeyUp(ev('MetaLeft', 'Meta'));
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', VK_LWIN],
            ['keyup', VK_LWIN],
        ]);
    });
});

/** Minimal stand-in for the held-input heartbeat. */
function beatSink(overrides = {}) {
    const sent = [];
    return {
        sent,
        _heldPhysKeys: new Map(),
        _heldMouseButtons: new Set(),
        _gamepadManager: null,
        _gamingMode: false,
        _lastBeatHadState: false,
        _sendInputState: StreamView.prototype._sendInputState,
        _sendToHost: (m) => sent.push(m),
        ...overrides,
    };
}

describe('the closing beat of the held-input heartbeat', () => {
    it('says once that nothing is held, then goes quiet', () => {
        const v = beatSink();
        v._heldPhysKeys.set('KeyW', { keyCode: VK_W, code: 'KeyW', key: 'w' });
        v._sendInputState();
        expect(v.sent.length).toBe(1);
        expect(v.sent[0].keys.length).toBe(1);

        // The key comes up, and the next beat carries the empty state — this is
        // what makes the host let go of anything it still believes is held.
        v._heldPhysKeys.clear();
        v._sendInputState();
        expect(v.sent.length).toBe(2);
        expect(v.sent[1]).toMatchObject({ type: 'inputstate', keys: [], buttons: 0 });

        // And then nothing, beat after beat.
        v._sendInputState();
        v._sendInputState();
        expect(v.sent.length).toBe(2);
    });

    it('still says nothing at all when nothing was ever held', () => {
        const v = beatSink();
        v._sendInputState();
        v._sendInputState();
        expect(v.sent).toEqual([]);
    });

    it('keeps the forced opening beat, which arms the host watchdog', () => {
        const v = beatSink();
        v._sendInputState(true);
        expect(v.sent.length).toBe(1);
        expect(v.sent[0].keys).toEqual([]);
        // A forced beat with nothing held must not then owe a closing one.
        v._sendInputState();
        expect(v.sent.length).toBe(1);
    });

    // A stall holds the beats back in the channel's queue, ahead of a release
    // made meanwhile: the host tells them from a fresh one by when they were
    // written, and presses nothing again on their word.
    it('says when it was written, on the page clock in whole ms', () => {
        const v = beatSink();
        v._heldPhysKeys.set('KeyE', { keyCode: 0x45, code: 'KeyE', key: 'e' });
        const before = globalThis.performance.now();
        v._sendInputState();
        const after = globalThis.performance.now();
        const t = v.sent[0].t;
        expect(Number.isInteger(t)).toBe(true);
        expect(t).toBeGreaterThanOrEqual(Math.floor(before));
        expect(t).toBeLessThanOrEqual(Math.ceil(after));
    });
});

describe('the grace period a held key gets', () => {
    const holds = (gamingMode, code) =>
        StreamView.prototype._holdsThroughStall.call({ _gamingMode: gamingMode }, code);

    // A game reads what is down: a key let go of then pressed again when the
    // link wakes is an "interact" or a reload fired twice (09/11/2026).
    it('is the long one for every key in gaming mode, not only the movement keys', () => {
        for (const code of ['KeyW', 'KeyE', 'KeyR', 'Space', 'Digit1', 'Tab', 'ArrowUp']) {
            expect(holds(true, code)).toBe(true);
        }
    });

    // At the desktop the host's typematic would type a held letter again and
    // again for the length of the stall.
    it('stays the short one at the desktop', () => {
        for (const code of ['KeyW', 'KeyE', 'Space']) {
            expect(holds(false, code)).toBe(false);
        }
    });
});

describe('a Windows AltGr', () => {
    // Chrome on Windows: AltGr is a left Ctrl and a right Alt going down
    // together, and from the right Alt on, getModifierState says AltGraph and
    // neither Control nor Alt. Read as up, the Ctrl was let go of as the Alt
    // went down, then the Alt at the next key with nothing between: Notepad
    // turned its KeyTips on at each @ (29/10/2026).
    const VK_RMENU = 0xa5; // the right Alt: AltGr on a European layout

    function pressAltGr(v) {
        v.handleKeyDown(evState('ControlLeft', 'Control', ['Control'], { ctrlKey: true }));
        v.handleKeyDown(
            evState('AltRight', 'AltGraph', ['AltGraph'], { ctrlKey: true, altKey: true }),
        );
    }

    it('keeps its Ctrl and Alt down for the character it types', () => {
        const v = keySink();
        pressAltGr(v);
        v.handleKeyDown(evState('Digit0', '@', ['AltGraph'], { ctrlKey: true, altKey: true }));

        expect(v.sent.filter((m) => m.type === 'keyup')).toEqual([]);
        expect(v.sent.slice(0, 2).map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', VK_LCTRL],
            ['keydown', VK_RMENU],
        ]);
        expect(v._heldPhysKeys.has('ControlLeft')).toBe(true);
        expect(v._heldPhysKeys.has('AltRight')).toBe(true);
    });

    it('still lets go of a Shift the event says is up', () => {
        const v = keySink();
        v.handleKeyDown(evState('ShiftLeft', 'Shift', ['Shift'], { shiftKey: true }));
        v.handleKeyDown(evState('ControlLeft', 'Control', ['Shift', 'Control'], { ctrlKey: true }));
        v.handleKeyDown(
            evState('AltRight', 'AltGraph', ['Shift', 'AltGraph'], { ctrlKey: true, altKey: true }),
        );
        v.sent.length = 0;
        v.handleKeyDown(evState('Digit0', '@', ['AltGraph'], { ctrlKey: true, altKey: true }));

        expect(v.sent[0]).toMatchObject({ type: 'keyup', code: 'ShiftLeft' });
    });

    it('lets go of its Ctrl and Alt once AltGraph is gone too', () => {
        const v = keySink();
        pressAltGr(v);
        v.sent.length = 0;
        // Both key-ups lost: the next plain key says nothing is held.
        v.handleKeyDown(evState('KeyA', 'a', []));

        expect(
            v.sent
                .filter((m) => m.type === 'keyup')
                .map((m) => m.keyCode)
                .sort(),
        ).toEqual([VK_LCTRL, VK_RMENU].sort());
    });
});

describe('the left Ctrl Windows puts under an AltGr', () => {
    // Windows sends it with the right Alt, in the same millisecond. Held, it
    // kept AltGr+E from typing € on a Linux host (issue #44, 29/10/2026): its
    // release goes out marked, for the server to pass to a positional host
    // only, and the key stays held here, marked, for the heartbeat.
    const VK_RMENU = 0xa5;

    function at(e, t) {
        e.timeStamp = t;
        return e;
    }

    function pressAltGrAt(v, ctrlAt, altAt) {
        v.handleKeyDown(
            at(evState('ControlLeft', 'Control', ['Control'], { ctrlKey: true }), ctrlAt),
        );
        v.handleKeyDown(
            at(
                evState('AltRight', 'AltGraph', ['AltGraph'], { ctrlKey: true, altKey: true }),
                altAt,
            ),
        );
    }

    it('goes up, marked, before the right Alt goes down', () => {
        const v = keySink();
        pressAltGrAt(v, 1000, 1000);

        expect(v.sent.map((m) => [m.type, m.keyCode, !!m.altGrCtrl])).toEqual([
            ['keydown', VK_LCTRL, false],
            ['keyup', VK_LCTRL, true],
            ['keydown', VK_RMENU, false],
        ]);
        // Still held here, marked: the heartbeat names it, and the native host
        // keeps it down.
        expect(v._heldPhysKeys.get('ControlLeft').altGrCtrl).toBe(true);
    });

    it('is told apart from a Ctrl a hand pressed first', () => {
        const v = keySink();
        pressAltGrAt(v, 1000, 1250);

        expect(v.sent.filter((m) => m.altGrCtrl)).toEqual([]);
        expect(v._heldPhysKeys.get('ControlLeft').altGrCtrl).toBeFalsy();
    });

    it('is not a Linux AltGr, which has no Ctrl', () => {
        const v = keySink();
        v.handleKeyDown(at(evState('AltRight', 'AltGraph', ['AltGraph']), 1000));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([['keydown', VK_RMENU]]);
    });

    it('is named as such in the heartbeat', () => {
        const v = keySink({
            _heldMouseButtons: new Set(),
            _gamepadManager: null,
            _lastBeatHadState: false,
            _sendToHost: (m) => v.sent.push(m),
            _sendInputState: StreamView.prototype._sendInputState,
        });
        pressAltGrAt(v, 1000, 1001);
        v.sent.length = 0;
        v._sendInputState();

        const keys = v.sent[0].keys;
        expect(keys.find((k) => k.code === 'ControlLeft').altGrCtrl).toBe(true);
        expect(keys.find((k) => k.code === 'AltRight').altGrCtrl).toBe(false);
    });
});
