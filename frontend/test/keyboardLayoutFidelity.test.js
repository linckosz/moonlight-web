/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * The protocol below us is positional: the key's PHYSICAL position is sent as a
 * US virtual key, and the host resolves it through its own layout. That types
 * the wrong characters the moment the two layouts disagree — an AZERTY viewer
 * on a QWERTY host presses "azerty" and reads "qwerty".
 *
 * The fix adds one optional field, `char`: the character the client's layout
 * actually produced. It is present ONLY when that character differs from what
 * the US layout puts at the same position, so a US client and every agreeing
 * key of any other client keep the exact message they have always sent — which
 * is the property these tests exist to hold.
 */

const VK_Q = 0x51;
const VK_E = 0x45;
const VK_1 = 0x31;
const VK_SPACE = 0x20;

/** Minimal stand-in exposing just what the keyboard path touches. */
function keySink(overrides = {}) {
    const sent = [];
    return {
        sent,
        webrtc: { send: (m) => sent.push(m) },
        _sendToHost: (m) => sent.push(m),
        _heldPhysKeys: new Map(),
        _heldMouseButtons: new Set(),
        _metaTapCodes: new Set(),
        _appleKeyboard: false,
        _quitting: false,
        _pendingClipboardWrite: null,
        _kbdCapture: null,
        _locksSynced: true,
        _cssFullscreen: false,
        _clipboardEnabled: false,
        _layoutMap: null,
        _gamingMode: false,
        _gamepadManager: null,
        _pendingPasteKey: null,
        _suppressPasteKeyUpCode: null,
        handleKeyDown: StreamView.prototype.handleKeyDown,
        handleKeyUp: StreamView.prototype.handleKeyUp,
        _sendKeyEvent: StreamView.prototype._sendKeyEvent,
        _forgetHeldKey: StreamView.prototype._forgetHeldKey,
        _holdsThroughStall: StreamView.prototype._holdsThroughStall,
        _sendInputState: StreamView.prototype._sendInputState,
        ...overrides,
    };
}

/** A KeyboardEvent-shaped plain object. */
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
        getModifierState: () => false,
        preventDefault() {},
        ...mods,
    };
}

describe('clientChar — which keystrokes need correcting at all', () => {
    it('says nothing for a US client, on every printable key', () => {
        // The anti-regression case: a viewer whose layout matches the one the
        // protocol assumes must produce the byte-identical message it always did.
        for (const [code, plain, shifted] of [
            ['KeyQ', 'q', 'Q'],
            ['KeyA', 'a', 'A'],
            ['Digit1', '1', '!'],
            ['Slash', '/', '?'],
            ['Backquote', '`', '~'],
        ]) {
            expect(StreamView.clientChar(ev(code, plain))).toBeNull();
            expect(StreamView.clientChar(ev(code, shifted, { shiftKey: true }))).toBeNull();
        }
    });

    it('says nothing for a key that has a name rather than a character', () => {
        for (const key of ['Enter', 'ArrowUp', 'F1', 'Shift', 'Escape', 'Dead'])
            expect(StreamView.clientChar(ev('Whatever', key))).toBeNull();
    });

    it('names the character on an AZERTY key that disagrees with its US position', () => {
        expect(StreamView.clientChar(ev('KeyQ', 'a'))).toBe('a');
        expect(StreamView.clientChar(ev('KeyW', 'z'))).toBe('z');
        expect(StreamView.clientChar(ev('KeyA', 'q'))).toBe('q');
        // The digit row is where AZERTY diverges the hardest: unshifted it
        // types symbols, and the shift state that reaches them differs per
        // layout, so the position alone can never get there.
        expect(StreamView.clientChar(ev('Digit1', '&'))).toBe('&');
        expect(StreamView.clientChar(ev('Digit1', '1', { shiftKey: true }))).toBe('1');
    });

    it('leaves the keys AZERTY shares with US alone', () => {
        // Most of the keyboard agrees, and every one of those keeps the
        // untouched path — including Space, which no layout moves.
        for (const [code, key] of [
            ['KeyE', 'e'],
            ['KeyR', 'r'],
            ['KeyT', 't'],
            ['Space', ' '],
        ])
            expect(StreamView.clientChar(ev(code, key))).toBeNull();
    });

    it('always names an AltGr character, which no US position carries', () => {
        // Windows reports AltGr as Ctrl+Alt, so this has to be decided before
        // anything treats the keystroke as a chord.
        const e = ev('KeyE', '€', {
            ctrlKey: true,
            altKey: true,
            getModifierState: (name) => name === 'AltGraph',
        });
        expect(StreamView.clientChar(e)).toBe('€');
    });

    it('names the letter of a Ctrl chord, which is the bug users actually hit', () => {
        // Ctrl+A on AZERTY sits at the US Q position: sent positionally it
        // reaches the host as Ctrl+Q.
        expect(StreamView.clientChar(ev('KeyQ', 'a', { ctrlKey: true }))).toBe('a');
    });
});

describe('the wire message', () => {
    it('carries the character on a divergent key and the position as before', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyQ', 'a'));

        expect(v.sent).toHaveLength(1);
        expect(v.sent[0].type).toBe('keydown');
        expect(v.sent[0].code).toBe('KeyQ');
        // The position is still there: the backend needs it for a host that can
        // only be addressed positionally.
        expect(v.sent[0].keyCode).toBe(VK_Q);
        expect(v.sent[0].char).toBe('a');
    });

    it('carries no character on an agreeing key', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyE', 'e'));
        expect(v.sent[0].keyCode).toBe(VK_E);
        expect(v.sent[0].char).toBeNull();
    });

    it('replays the press character on the release, never the release event', () => {
        // Releasing Shift before the letter changes what e.key reads. A release
        // resolved differently from its press is a different key to Sunshine —
        // which identifies one by (keyCode, flags) — so the press never comes
        // back up and the host holds it forever.
        const v = keySink();
        v.handleKeyDown(ev('Digit1', '1', { shiftKey: true }));
        expect(v.sent[0].char).toBe('1');

        v.sent.length = 0;
        v.handleKeyUp(ev('Digit1', '&')); // Shift already gone
        expect(v.sent).toHaveLength(1);
        expect(v.sent[0].type).toBe('keyup');
        expect(v.sent[0].keyCode).toBe(VK_1);
        expect(v.sent[0].char).toBe('1');
    });

    it('repeats the press the host received, character included', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyQ', 'a'));
        v.sent.length = 0;

        v.handleKeyDown(ev('KeyQ', 'a', { repeat: true }));
        expect(v.sent).toHaveLength(1);
        expect(v.sent[0]).toMatchObject({ type: 'keydown', keyCode: VK_Q, char: 'a' });
    });

    it('reports the character in the held-input heartbeat', () => {
        // Same reason as the release: a re-press the watchdog resolves down a
        // different path than the original press would strand the key.
        const v = keySink();
        v.handleKeyDown(ev('KeyQ', 'a'));
        v.handleKeyDown(ev('Space', ' '));
        v.sent.length = 0;

        v._sendInputState();
        expect(v.sent).toHaveLength(1);
        const state = v.sent[0];
        expect(state.type).toBe('inputstate');
        expect(state.keys).toEqual([
            expect.objectContaining({ keyCode: VK_Q, char: 'a' }),
            expect.objectContaining({ keyCode: VK_SPACE, char: null }),
        ]);
    });
});
