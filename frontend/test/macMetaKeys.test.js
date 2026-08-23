/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * macOS never delivers the keyup of a key pressed while Cmd is held. The
 * keydown for KeyC goes out, its keyup never does — so _heldPhysKeys keeps the
 * key forever, the 100ms held-input heartbeat re-asserts it to the host, and
 * the guest typematic turns one Cmd+C into "ccccccccc…".
 *
 * The fix sends the chord as a tap (down immediately followed by up) instead
 * of waiting for a release that will not come. The Cmd→Windows-key mapping is
 * deliberately left alone: Cmd alone still opens the Start menu, and Ctrl+C
 * remains the way to copy on the host.
 */

const VK_C = 0x43;
const VK_V = 0x56;
const VK_LWIN = 0x5b;

/** Minimal stand-in exposing just what the keyboard path touches. */
function keySink(overrides = {}) {
    const sent = [];
    return {
        sent,
        webrtc: { send: (m) => sent.push(m) },
        _heldPhysKeys: new Map(),
        _metaTapCodes: new Set(),
        _appleKeyboard: true,
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
        _holdsThroughStall: StreamView.prototype._holdsThroughStall,
        _releaseKeysHeldUnderMeta: StreamView.prototype._releaseKeysHeldUnderMeta,
        _sendPendingPasteKey: StreamView.prototype._sendPendingPasteKey,
        ...overrides,
    };
}

/** A KeyboardEvent-shaped plain object. The target is a bare object: it is
 *  neither the soft-keyboard capture element nor (having no `closest`) one of
 *  the local inputs that keep their keystrokes client-side. */
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

describe('Cmd+key on an Apple keyboard', () => {
    it('releases the letter itself instead of waiting for a keyup that never comes', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyC', 'c', { metaKey: true }));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', VK_C],
            ['keyup', VK_C],
        ]);
        // The heartbeat reads this map: an entry here would re-press the key.
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('swallows the late keyup when the user lets go of Cmd first', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyC', 'c', { metaKey: true }));
        v.sent.length = 0;

        // Cmd released before C → macOS delivers C's keyup after all.
        v.handleKeyUp(ev('KeyC', 'c'));
        expect(v.sent).toEqual([]);
        expect(v._metaTapCodes.size).toBe(0);
    });

    it('leaves Cmd itself held, so Cmd alone still opens the Start menu', () => {
        const v = keySink();
        v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));

        expect(v.sent).toHaveLength(1);
        expect(v.sent[0]).toMatchObject({ type: 'keydown', keyCode: VK_LWIN });
        expect(v._heldPhysKeys.has('MetaLeft')).toBe(true);
    });

    it('releases a key that was already down when Cmd joined it', () => {
        const v = keySink();
        // C held first — no metaKey, so the tap branch never sees it.
        v.handleKeyDown(ev('KeyC', 'c'));
        v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));
        expect(v._heldPhysKeys.has('KeyC')).toBe(true);
        v.sent.length = 0;

        // Cmd comes back up: C's own keyup was eaten, sweep it out.
        v.handleKeyUp(ev('MetaLeft', 'Meta'));
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keyup', VK_C],
            ['keyup', VK_LWIN],
        ]);
        expect(v._heldPhysKeys.size).toBe(0);
    });
});

describe('Cmd+V falling back to a plain paste chord', () => {
    // The clipboard bridge swallows the Cmd+V keydown and waits for the native
    // 'paste' event. When none brings text, the chord is forwarded so the host
    // pastes its own clipboard — and that forward is a real keydown, which
    // needs the same treatment as any key held under Cmd.
    it('completes the chord instead of leaving V down', () => {
        const v = keySink({ _clipboardEnabled: true });
        v.handleKeyDown(ev('KeyV', 'v', { metaKey: true }));
        expect(v.sent).toEqual([]); // swallowed, waiting for 'paste'

        v._sendPendingPasteKey(); // what the 150ms timer does
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', VK_V],
            ['keyup', VK_V],
        ]);
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('does not double the release when V is let go before the paste event', () => {
        const v = keySink({ _clipboardEnabled: true });
        v.handleKeyDown(ev('KeyV', 'v', { metaKey: true }));

        v.handleKeyUp(ev('KeyV', 'v', { metaKey: true }));
        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', VK_V],
            ['keyup', VK_V],
        ]);
        expect(v._heldPhysKeys.size).toBe(0);
    });
});

describe('everything else keeps the plain press/release path', () => {
    it('keeps Ctrl+C a real hold on a Mac — the mapping users actually copy with', () => {
        const v = keySink();
        v.handleKeyDown(ev('KeyC', 'c', { ctrlKey: true }));

        expect(v.sent).toHaveLength(1);
        expect(v.sent[0]).toMatchObject({ type: 'keydown', keyCode: VK_C, ctrlKey: true });
        expect(v._heldPhysKeys.has('KeyC')).toBe(true);

        v.handleKeyUp(ev('KeyC', 'c', { ctrlKey: true }));
        expect(v.sent).toHaveLength(2);
        expect(v.sent[1]).toMatchObject({ type: 'keyup', keyCode: VK_C });
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('does not touch Win+key on a non-Apple client', () => {
        const v = keySink({ _appleKeyboard: false });
        v.handleKeyDown(ev('KeyC', 'c', { metaKey: true }));

        expect(v.sent).toHaveLength(1);
        expect(v.sent[0]).toMatchObject({ type: 'keydown', keyCode: VK_C });
        expect(v._heldPhysKeys.has('KeyC')).toBe(true);

        // And its Meta release sweeps nothing away.
        v.handleKeyUp(ev('MetaLeft', 'Meta'));
        expect(v.sent.map((m) => m.type)).toEqual(['keydown', 'keyup']);
        expect(v._heldPhysKeys.has('KeyC')).toBe(true);
    });
});
