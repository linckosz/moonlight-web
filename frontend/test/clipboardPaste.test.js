import { describe, it, expect } from 'vitest';
import { StreamView } from '../js/ui/StreamView.js';

function sink(modifier = 'meta', injectCtrl = false) {
    const view = Object.create(StreamView.prototype);
    view.sent = [];
    view.webrtc = { send: (message) => view.sent.push(message) };
    view._heldPhysKeys = new Map();
    view._kbdCapture = null;
    view._clipboardPasteModifier = modifier;
    view._pendingPasteKey = {
        code: 'KeyV',
        injectCtrl,
        keydownMsg: { type: 'keydown', code: 'KeyV', keyCode: 0x56, ctrlKey: true },
        timer: setTimeout(() => {}, 10000),
    };
    return view;
}

function hold(view, code, keyCode) {
    view._heldPhysKeys.set(code, { type: 'keyup', code, keyCode });
}

function paste(view, text) {
    view.handlePaste({
        target: {},
        preventDefault() {},
        clipboardData: { getData: () => text },
    });
}

describe('native Mac clipboard paste', () => {
    it('releases both Control keys before Windows text reaches Command+V', () => {
        const view = sink();
        hold(view, 'ControlLeft', 0x11);
        hold(view, 'ControlRight', 0x11);
        paste(view, '中文\nsecond line');
        expect(view.sent.map((m) => m.type)).toEqual(['keyup', 'keyup', 'clipboardpaste']);
        expect(view.sent.slice(0, 2).every((m) => !m.ctrlKey && !m.metaKey)).toBe(true);
        expect(view.sent[2].text).toBe('中文\nsecond line');
        expect(view._heldPhysKeys.size).toBe(0);
        expect(view._suppressPasteKeyUpCode).toBe('KeyV');
    });

    it('also releases Meta before a Mac client paste', () => {
        const view = sink('meta', true);
        hold(view, 'MetaLeft', 0x5b);
        paste(view, 'text');
        expect(view.sent[0].code).toBe('MetaLeft');
        expect(view.sent[1].type).toBe('clipboardpaste');
        expect(view._heldPhysKeys.size).toBe(0);
    });

    it('empty or non-text local clipboard requests the Mac host paste chord', () => {
        const view = sink();
        hold(view, 'ControlLeft', 0x11);
        paste(view, '');
        expect(view.sent.at(-1)).toEqual({ type: 'clipboardpaste', text: '', injectCtrl: true });
        expect(view._suppressPasteKeyUpCode).toBe('KeyV');
        expect(view._heldPhysKeys.size).toBe(0);
    });

    it('timeout fallback also selects host-side Command+V', () => {
        const view = sink();
        hold(view, 'ControlLeft', 0x11);
        view._sendPendingPasteKey();
        expect(view.sent.at(-1).type).toBe('clipboardpaste');
        expect(view._pendingPasteKey).toBe(null);
    });

    it('keeps the legacy Windows Control+V fallback', () => {
        const view = sink('control');
        hold(view, 'ControlLeft', 0x11);
        view._sendPendingPasteKey();
        expect(view.sent.at(-1).type).toBe('keydown');
        expect(view._heldPhysKeys.has('ControlLeft')).toBe(true);
    });

    it('does not convert normal Windows paste into a Meta chord', () => {
        const view = sink('control');
        hold(view, 'ControlLeft', 0x11);
        paste(view, 'text');
        expect(view.sent).toEqual([{ type: 'clipboardpaste', text: 'text', injectCtrl: false }]);
        expect(view._heldPhysKeys.has('ControlLeft')).toBe(true);
    });
});
