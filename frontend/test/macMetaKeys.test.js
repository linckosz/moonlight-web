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
 * deliberately left alone: Cmd alone still opens the Start menu. With the
 * shared clipboard, Cmd+V and Cmd+C alone become the host's Ctrl+V and Ctrl+C.
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
        handlePaste: StreamView.prototype.handlePaste,
        _sendKeyEvent: StreamView.prototype._sendKeyEvent,
        _reconcileModifiers: StreamView.prototype._reconcileModifiers,
        _markAltGrCtrl: StreamView.prototype._markAltGrCtrl,
        _forgetHeldKey: StreamView.prototype._forgetHeldKey,
        _holdsThroughStall: StreamView.prototype._holdsThroughStall,
        _releaseKeysHeldUnderMeta: StreamView.prototype._releaseKeysHeldUnderMeta,
        _releaseHeldMeta: StreamView.prototype._releaseHeldMeta,
        _sendPendingPasteKey: StreamView.prototype._sendPendingPasteKey,
        _sendCopyChord: StreamView.prototype._sendCopyChord,
        ...overrides,
    };
}

/** A 'paste' event carrying local clipboard text. */
function pasteEv(text) {
    return {
        target: {},
        clipboardData: { getData: () => text },
        preventDefault() {},
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

/** Cmd is down on the host as the Windows key by the time the paste chord is
 *  injected — the whole point of these two blocks. */
function macPasting() {
    const v = keySink({ _clipboardEnabled: true });
    v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));
    v.handleKeyDown(ev('KeyV', 'v', { metaKey: true }));
    v.sent.length = 0;
    return v;
}

describe('Cmd+V reaching the host as a paste', () => {
    // Cmd streams as the host's Windows key and it is still held while the
    // backend injects Ctrl+V: the host reads Win+Ctrl+V, which pastes nothing.
    it('lets go of the Windows key before the injected chord', () => {
        const v = macPasting();
        v.handlePaste(pasteEv('x'));

        expect(v.sent[2]).toMatchObject({ type: 'keyup', keyCode: VK_LWIN });
        expect(v.sent[3]).toMatchObject({ type: 'clipboardpaste', text: 'x', injectCtrl: true });
        expect(v._heldPhysKeys.has('MetaLeft')).toBe(false);
    });

    // Win down then up with nothing between is what opens the Start menu, and
    // the injected chord would paste into its search box.
    it('taps Ctrl under the Windows key so its release opens no Start menu', () => {
        const v = macPasting();
        v.handlePaste(pasteEv('x'));

        expect(v.sent[0]).toMatchObject({ type: 'keydown', code: 'ControlLeft', metaKey: true });
        expect(v.sent[1]).toMatchObject({ type: 'keyup', code: 'ControlLeft', metaKey: true });
        expect(v._heldPhysKeys.has('ControlLeft')).toBe(false);
    });

    it('leaves a Cmd pressed alone a plain Windows key', () => {
        const v = keySink({ _clipboardEnabled: true });
        v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));
        v.handleKeyUp(ev('MetaLeft', 'Meta'));

        expect(v.sent.map((m) => m.code)).toEqual(['MetaLeft', 'MetaLeft']);
    });

    // No text to ship (empty or non-text local clipboard): the host pastes its
    // own clipboard. Forwarding the chord itself would send Win+V, so ask the
    // backend for the same injected Ctrl+V with nothing to commit.
    it('asks for a bare chord instead of forwarding Win+V', () => {
        const v = macPasting();
        v._sendPendingPasteKey(); // what the 150ms timer does

        expect(v.sent.map((m) => m.type)).toEqual(['keydown', 'keyup', 'keyup', 'clipboardpaste']);
        expect(v.sent[3]).toMatchObject({ text: '', injectCtrl: true });
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('does not release V twice when it is let go before the paste event', () => {
        const v = macPasting();
        v.handleKeyUp(ev('KeyV', 'v', { metaKey: true }));

        expect(v.sent.map((m) => m.type)).toEqual(['keydown', 'keyup', 'keyup', 'clipboardpaste']);
        expect(v._heldPhysKeys.has('KeyV')).toBe(false);
    });
});

describe('Ctrl+V on a Mac', () => {
    // macOS pastes with Cmd, so Ctrl+V never fires a native 'paste' event.
    // Routing it through the clipboard bridge meant waiting 150ms for an event
    // that was not coming, then swallowing V's release — which left V held on
    // the host, where typematic repeated the paste until another key was hit.
    it('goes straight to the host as a plain chord', () => {
        const v = keySink({ _clipboardEnabled: true });
        v.handleKeyDown(ev('ControlLeft', 'Control', { ctrlKey: true }));
        v.handleKeyDown(ev('KeyV', 'v', { ctrlKey: true }));

        expect(v._pendingPasteKey).toBe(null);
        expect(v.sent[1]).toMatchObject({ type: 'keydown', keyCode: VK_V, ctrlKey: true });

        v.handleKeyUp(ev('KeyV', 'v', { ctrlKey: true }));
        expect(v.sent[2]).toMatchObject({ type: 'keyup', keyCode: VK_V });
        expect(v._heldPhysKeys.has('KeyV')).toBe(false);
    });

    // Cmd+V arms a key-up token for a release macOS then eats, so the token is
    // still armed at the next press of that key. Swallowing that release is
    // fine; letting the key stay held is what repeated the paste forever.
    it('never stays held when a stale suppression token eats its release', () => {
        const v = keySink({ _clipboardEnabled: true, _suppressPasteKeyUpCode: 'KeyV' });
        v.handleKeyDown(ev('ControlLeft', 'Control', { ctrlKey: true }));
        v.handleKeyDown(ev('KeyV', 'v', { ctrlKey: true }));
        expect(v._heldPhysKeys.has('KeyV')).toBe(true);

        v.handleKeyUp(ev('KeyV', 'v', { ctrlKey: true }));
        expect(v._heldPhysKeys.has('KeyV')).toBe(false);
        expect(v._suppressPasteKeyUpCode).toBe(null);
    });
});

describe('Ctrl+V away from Apple keyboards', () => {
    it('still goes through the clipboard bridge', () => {
        const v = keySink({ _clipboardEnabled: true, _appleKeyboard: false });
        v.handleKeyDown(ev('KeyV', 'v', { ctrlKey: true }));
        expect(v.sent).toEqual([]); // swallowed, waiting for 'paste'

        v.handlePaste(pasteEv('x'));
        expect(v.sent).toHaveLength(1);
        expect(v.sent[0]).toMatchObject({ type: 'clipboardpaste', text: 'x', injectCtrl: false });
    });

    it('forwards the chord itself when no text comes back', () => {
        const v = keySink({ _clipboardEnabled: true, _appleKeyboard: false });
        v.handleKeyDown(ev('KeyV', 'v', { ctrlKey: true }));

        v._sendPendingPasteKey();
        expect(v.sent).toHaveLength(1);
        expect(v.sent[0]).toMatchObject({ type: 'keydown', keyCode: VK_V, ctrlKey: true });
        expect(v._heldPhysKeys.has('KeyV')).toBe(true);
    });
});

/** Cmd held, then C, with the shared clipboard on. */
function macCopying(mods = {}, key = 'c', code = 'KeyC') {
    const v = keySink({ _clipboardEnabled: true });
    v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));
    v.sent.length = 0;
    v.handleKeyDown(ev(code, key, { metaKey: true, ...mods }));
    return v;
}

describe('Cmd+C reaching the host as a copy', () => {
    // Sent as is, Cmd+C is Win+C on the host: the window loses its selection
    // and nothing is copied (Bruno on the native card, 08/11).
    it('sends Ctrl+C, after letting go of the Windows key', () => {
        const v = macCopying();

        expect(v.sent.map((m) => [m.type, m.code])).toEqual([
            ['keydown', 'ControlLeft'], // the tap that keeps the Start menu shut
            ['keyup', 'ControlLeft'],
            ['keyup', 'MetaLeft'],
            ['keydown', 'ControlLeft'],
            ['keydown', 'KeyC'],
            ['keyup', 'KeyC'],
            ['keyup', 'ControlLeft'],
        ]);
        expect(v.sent[4]).toMatchObject({ keyCode: VK_C, ctrlKey: true, metaKey: false });
        expect(v._heldPhysKeys.size).toBe(0);
    });

    it('swallows the late keyup of C', () => {
        const v = macCopying();
        v.sent.length = 0;

        v.handleKeyUp(ev('KeyC', 'c'));
        expect(v.sent).toEqual([]);
        expect(v._metaTapCodes.size).toBe(0);
    });

    // A layout where another key types "c": Windows still copies with VK_C.
    it('copies with VK_C whichever key types "c"', () => {
        const v = macCopying({}, 'c', 'KeyI');

        expect(
            v.sent.find((m) => m.type === 'keydown' && m.ctrlKey && m.code !== 'ControlLeft'),
        ).toMatchObject({
            code: 'KeyC',
            keyCode: VK_C,
        });
        v.sent.length = 0;
        v.handleKeyUp(ev('KeyI', 'c'));
        expect(v.sent).toEqual([]);
    });

    it('leaves Cmd+Shift+C and Cmd+Option+C to the plain tap', () => {
        for (const mods of [{ shiftKey: true }, { altKey: true }]) {
            const v = macCopying(mods, 'C');
            expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
                ['keydown', VK_C],
                ['keyup', VK_C],
            ]);
            expect(v._heldPhysKeys.has('MetaLeft')).toBe(true);
        }
    });

    it('stays a Win+C tap when the clipboard is not shared', () => {
        const v = keySink();
        v.handleKeyDown(ev('MetaLeft', 'Meta', { metaKey: true }));
        v.sent.length = 0;
        v.handleKeyDown(ev('KeyC', 'c', { metaKey: true }));

        expect(v.sent.map((m) => [m.type, m.keyCode])).toEqual([
            ['keydown', VK_C],
            ['keyup', VK_C],
        ]);
        expect(v._heldPhysKeys.has('MetaLeft')).toBe(true);
    });

    it('does not turn Win+C into a copy on a non-Apple client', () => {
        const v = keySink({ _clipboardEnabled: true, _appleKeyboard: false });
        v.handleKeyDown(ev('KeyC', 'c', { metaKey: true }));

        expect(v.sent).toHaveLength(1);
        expect(v.sent[0]).toMatchObject({ type: 'keydown', keyCode: VK_C, ctrlKey: false });
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
