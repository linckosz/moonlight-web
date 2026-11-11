/* MoonlightWeb — input regression tests. GPLv3. */
import { afterEach, beforeEach, describe, expect, it, vi } from 'vitest';
import {
    SessionKeyMapper,
    normalizeInput,
    inputStorageKey,
    loadInput,
    saveInput,
} from '../js/util/SessionInput.js';
import { t } from '../js/i18n/i18n.js';
import { StreamView } from '../js/ui/StreamView.js';
import { openSessionInputPanel, openKeyMappingDialog } from '../js/ui/SessionInputPanel.js';

const ALT_META_SWAP = { AltLeft: 'MetaLeft', MetaLeft: 'AltLeft' };

const event = (type, code, flags = {}) => ({
    type,
    code,
    keyCode: StreamView.codeToWindowsVk(code),
    ctrlKey: false,
    altKey: false,
    shiftKey: false,
    metaKey: false,
    ...flags,
});
const mapper = (map = ALT_META_SWAP) => new SessionKeyMapper(map, StreamView.codeToWindowsVk);
const sink = () => {
    const v = Object.create(StreamView.prototype);
    v.sent = [];
    v.webrtc = { send: (m) => v.sent.push(m) };
    v._heldPhysKeys = new Map();
    v._heldMouseButtons = new Set();
    v._inputPreferences = normalizeInput({ mappings: ALT_META_SWAP });
    v._keyMapper = mapper();
    v._clipboardEnabled = true;
    return v;
};

afterEach(() => {
    localStorage.clear();
    vi.restoreAllMocks();
    document.body.replaceChildren();
});

describe('session key mappings', () => {
    it('swaps only left Alt/Meta, including flags on following letters', () => {
        const m = mapper();
        expect(m.send(event('keydown', 'AltLeft', { altKey: true }))).toMatchObject({
            code: 'MetaLeft',
            metaKey: true,
            altKey: false,
        });
        expect(m.send(event('keydown', 'KeyC', { altKey: true }))).toMatchObject({
            code: 'KeyC',
            metaKey: true,
            altKey: false,
        });
        expect(m.send(event('keyup', 'AltLeft'))).toMatchObject({
            code: 'MetaLeft',
            metaKey: false,
        });
        expect(m.send(event('keydown', 'MetaLeft', { metaKey: true }))).toMatchObject({
            code: 'AltLeft',
            altKey: true,
            metaKey: false,
        });
        expect(m.send(event('keydown', 'AltRight', { altKey: true, metaKey: true }))).toMatchObject(
            { code: 'AltRight', altKey: true, metaKey: false },
        );
    });
    it('clears upstream character and AltGr metadata on a remapped dead key', () => {
        const m = mapper({ Quote: 'KeyB' });
        const down = m.send(
            event('keydown', 'Quote', {
                char: '~',
                dead: true,
                chord: true,
                altGraph: true,
                altGrCtrl: true,
            }),
        );
        expect(down).toMatchObject({
            code: 'KeyB',
            keyCode: 0x42,
            char: null,
            dead: false,
            chord: false,
            altGraph: false,
            altGrCtrl: false,
        });
        expect(m.send(event('keyup', 'Quote'))).toMatchObject({ code: 'KeyB', dead: false });
    });
    it('preserves upstream AltGr and layout metadata when no mapping applies', () => {
        const m = mapper({});
        const original = event('keydown', 'AltRight', { altKey: true, altGraph: true });
        expect(m.send(original)).toEqual(original);
        expect(StreamView.codeToWindowsVk('AltRight')).toBe(0xa5);
    });
    it('does not inject the upstream AltGr Ctrl release for a remapped Ctrl', () => {
        const view = sink();
        view._inputPreferences = normalizeInput({ mappings: { ControlLeft: 'CapsLock' } });
        view._keyMapper = mapper(view._inputPreferences.mappings);
        view._sendKeyEvent(event('keydown', 'ControlLeft', { ctrlKey: true }));
        view._ctrlLeftAt = 10;
        view._markAltGrCtrl({
            code: 'AltRight',
            timeStamp: 10,
            getModifierState: () => true,
        });
        expect(view.sent).toHaveLength(1);
        expect(view.sent[0].code).toBe('CapsLock');
    });
    it('forgets mapped press bookkeeping when upstream swallows a late release', () => {
        const view = sink();
        view._sendKeyEvent(event('keydown', 'AltLeft', { altKey: true }));
        view._forgetHeldKey('AltLeft');
        expect(view._keyMapper.pressed.size).toBe(0);
        expect(view._heldPhysKeys.size).toBe(0);
    });
    it('uses the target physical key, not the original layout character', () => {
        const m = mapper({ KeyA: 'KeyB' });
        const down = m.send(event('keydown', 'KeyA', { char: 'a', nonUs: true }));
        expect(down).toMatchObject({ code: 'KeyB', keyCode: 0x42, char: null, nonUs: false });
        expect(m.send(event('keydown', 'KeyA', { char: 'A', shiftKey: true }))).toMatchObject({
            code: 'KeyB',
            char: null,
        });
        expect(m.send(event('keyup', 'KeyA'))).toMatchObject({ code: 'KeyB', keyCode: 0x42 });
    });
    it('does not release a target still held by another physical key', () => {
        const m = mapper({ KeyA: 'KeyB' });
        m.send(event('keydown', 'KeyA'));
        m.send(event('keydown', 'KeyB'));
        expect(m.send(event('keyup', 'KeyA'))).toBe(null);
        expect(m.send(event('keyup', 'KeyB')).type).toBe('keyup');
    });
    it('maps held-input heartbeats without reapplying the swap', () => {
        const m = mapper();
        const raw = event('keydown', 'AltLeft', { altKey: true });
        m.send(raw);
        expect(m.held([raw])[0]).toMatchObject({ code: 'MetaLeft', metaKey: true, altKey: false });
    });
    it('releases the mapped key on focus loss and before preferences change', () => {
        const v = sink();
        v._sendKeyEvent(event('keydown', 'AltLeft', { altKey: true }));
        v._setInputPreferences({ mappings: {} });
        expect(v.sent[1]).toMatchObject({ type: 'keyup', code: 'MetaLeft', metaKey: false });
        expect(v._heldPhysKeys.size).toBe(0);
        expect(v._keyMapper.pressed.size).toBe(0);
    });
    it('keeps synthesized toolbar shortcuts unchanged', () => {
        const m = mapper();
        const msg = { type: 'keydown', keyCode: 0x11, code: '', ctrlKey: true };
        expect(m.send(msg)).toBe(msg);
    });
});

describe('key mapping persistence', () => {
    it('persists applied host mappings and isolates guests', () => {
        const key = inputStorageKey({ uuid: 'host-1' });
        const settings = normalizeInput({ mappings: ALT_META_SWAP });
        saveInput(key, settings);
        expect(loadInput(key).mappings).toEqual(ALT_META_SWAP);
        expect(loadInput(inputStorageKey({ uuid: 'host-1' }, true)).mappings).toEqual({});
        saveInput(key, normalizeInput());
        expect(loadInput(key).mappings).toEqual({});
    });
    it('ignores corrupt storage and invalid key codes', () => {
        localStorage.setItem('bad', 'not-json');
        expect(loadInput('bad')).toEqual({ mappings: {} });
        expect(
            normalizeInput(
                {
                    mappings: {
                        Bad: 'KeyA',
                        KeyA: 'toString',
                        constructor: 'KeyB',
                        KeyB: ['KeyC'],
                        AltLeft: 'MetaLeft',
                    },
                },
                (c) => !!StreamView.codeToWindowsVk(c),
            ).mappings,
        ).toEqual({ AltLeft: 'MetaLeft' });
    });
    it('carries key mappings across view replacement', () => {
        const v = sink();
        const state = v.captureViewState();
        expect(state.inputPreferences.mappings).toEqual(ALT_META_SWAP);
        const next = sink();
        next._adoptOverlayDismissals = () => {};
        next.restoreViewState(state);
        expect(next._inputPreferences).toEqual(v._inputPreferences);
    });
    it('offers native controls without forwarding panel keyboard events', () => {
        const v = sink();
        v.streamEl = document.createElement('div');
        document.body.append(v.streamEl);
        HTMLDialogElement.prototype.showModal = vi.fn();
        openSessionInputPanel(v);
        expect(v._inputDialog.querySelector('[role=switch]')).toBeNull();
        expect(v._inputDialog.querySelector('textarea')).toBeNull();
        const touch = vi.fn();
        v.streamEl.addEventListener('touchstart', touch);
        v._inputDialog.dispatchEvent(new Event('touchstart', { bubbles: true }));
        expect(touch).not.toHaveBeenCalled();
        expect(v._inputDialog.querySelector('details').open).toBe(false);
        expect(StreamView.isLocalKeyboardTarget(v._inputDialog.querySelector('button'))).toBe(true);
    });
});

describe('host shortcuts after mapping', () => {
    const key = (code, flags = {}) => ({
        target: {},
        code,
        key: code === 'KeyV' ? 'v' : code,
        repeat: false,
        ctrlKey: false,
        altKey: false,
        metaKey: false,
        shiftKey: false,
        preventDefault: vi.fn(),
        ...flags,
    });
    const view = () => {
        const v = sink();
        v._clipboardPasteModifier = 'meta';
        v._appleKeyboard = false;
        v._locksSynced = true;
        v._metaTapCodes = new Set();
        v._swallowKeyUpCodes = new Set();
        return v;
    };
    it.each(['KeyC', 'KeyZ'])('sends Windows Alt+%s as a physical Command chord', (code) => {
        const v = view();
        v.handleKeyDown(key('AltLeft', { altKey: true }));
        v.handleKeyDown(key(code, { key: code.slice(-1).toLowerCase(), altKey: true }));
        expect(v.sent.at(-1)).toMatchObject({
            type: 'keydown',
            code,
            char: null,
            metaKey: true,
            altKey: false,
        });
        v._sendInputState(true);
        expect(v.sent.at(-1).keys.find((k) => k.code === code)).toMatchObject({
            char: null,
            metaKey: true,
            altKey: false,
        });
        v.handleKeyUp(key('AltLeft'));
        v.handleKeyUp(key(code));
        expect(v.sent.at(-1)).toMatchObject({ type: 'keyup', code, char: null, metaKey: false });
    });
    const appleView = (mappings = {}, host = 'control') => {
        const v = view();
        v._appleKeyboard = true;
        v._clipboardPasteModifier = host;
        v._inputPreferences = normalizeInput({ mappings });
        v._keyMapper = mapper(mappings);
        return v;
    };
    it.each(['text', 'timeout'])(
        'keeps upstream Cmd+V and Start suppression with key mapping settings (%s)',
        (mode) => {
            const v = appleView();
            v.handleKeyDown(key('MetaLeft', { metaKey: true }));
            v.handleKeyDown(key('KeyV', { metaKey: true }));
            expect(v._pendingPasteKey).toBeTruthy();
            v.sent.length = 0;
            if (mode === 'text')
                v.handlePaste({
                    target: {},
                    preventDefault() {},
                    clipboardData: { getData: () => 'paste text' },
                });
            else v._sendPendingPasteKey();
            expect(v.sent.map((msg) => msg.type)).toEqual([
                'keydown',
                'keyup',
                'keyup',
                'clipboardpaste',
            ]);
            expect(v.sent[0]).toMatchObject({ keyCode: 0x11, ctrlKey: true, metaKey: true });
            expect(v.sent[2]).toMatchObject({ code: 'MetaLeft', metaKey: false });
            expect(v.sent[3]).toMatchObject({
                text: mode === 'text' ? 'paste text' : '',
                injectCtrl: true,
            });
            expect(v._heldPhysKeys.size).toBe(0);
        },
    );
    it.each(['control', 'meta'])(
        'keeps native paste by layout character on a %s host without mappings',
        (host) => {
            const v = appleView({}, host);
            v.handleKeyDown(key('MetaLeft', { metaKey: true }));
            v.handleKeyDown(key('Period', { key: 'v', metaKey: true }));
            expect(v._pendingPasteKey?.code).toBe('Period');
            v._releaseAllPhysKeys();
        },
    );
    it('does not intercept the physical V position when the layout types another character', () => {
        const v = appleView();
        v.handleKeyDown(key('MetaLeft', { metaKey: true }));
        v.handleKeyDown(key('KeyV', { key: '.', metaKey: true }));
        expect(v._pendingPasteKey).toBeUndefined();
        expect(v.sent.at(-2)).toMatchObject({ type: 'keydown', code: 'KeyV' });
        v._releaseAllPhysKeys();
    });
    it('keeps native copy by layout character when a mapping does not affect the chord', () => {
        const v = appleView({ ControlLeft: 'AltLeft' });
        v.handleKeyDown(key('MetaLeft', { metaKey: true }));
        v.handleKeyDown(key('KeyI', { key: 'c', metaKey: true }));
        expect(v.sent.slice(-4).map((msg) => [msg.type, msg.keyCode])).toEqual([
            ['keydown', 0x11],
            ['keydown', 0x43],
            ['keyup', 0x43],
            ['keyup', 0x11],
        ]);
        v._releaseAllPhysKeys();
    });
    it('keeps upstream Cmd+C on Windows with mapping enabled', () => {
        const v = appleView({ ControlLeft: 'AltLeft' });
        v.handleKeyDown(key('MetaLeft', { metaKey: true }));
        v.handleKeyDown(key('KeyC', { key: 'c', metaKey: true }));
        expect(v.sent.slice(-4).map((msg) => [msg.type, msg.keyCode])).toEqual([
            ['keydown', 0x11],
            ['keydown', 0x43],
            ['keyup', 0x43],
            ['keyup', 0x11],
        ]);
        expect(v.sent.at(-3)).toMatchObject({ ctrlKey: true, metaKey: false });
        expect(v._heldPhysKeys.size).toBe(0);
    });
    it('leaves a mapped Option+C chord out of the upstream Ctrl+C conversion', () => {
        const v = appleView({ MetaLeft: 'AltLeft' });
        v.handleKeyDown(key('MetaLeft', { metaKey: true }));
        v.handleKeyDown(key('KeyC', { key: 'c', metaKey: true }));
        expect(v.sent.at(-2)).toMatchObject({
            code: 'KeyC',
            altKey: true,
            ctrlKey: false,
            metaKey: false,
        });
        expect(v.sent.some((msg) => msg.keyCode === 0x11)).toBe(false);
        v._releaseAllPhysKeys();
    });
    it('copies on a Mac host with Command after the upstream Windows shortcut change', () => {
        const v = appleView({}, 'meta');
        v.handleKeyDown(key('MetaLeft', { metaKey: true }));
        v.handleKeyDown(key('KeyC', { key: 'c', metaKey: true }));
        expect(v.sent.at(-2)).toMatchObject({ code: 'KeyC', metaKey: true, ctrlKey: false });
        expect(v.sent.some((msg) => msg.keyCode === 0x11)).toBe(false);
        v._releaseAllPhysKeys();
    });

    it('preserves mapped Option and Shift instead of reinterpreting the client character', () => {
        const v = view();
        v.handleKeyDown(key('MetaLeft', { metaKey: true }));
        v.handleKeyDown(key('KeyC', { key: 'C', metaKey: true, shiftKey: true }));
        expect(v.sent.at(-1)).toMatchObject({
            code: 'KeyC',
            char: null,
            altKey: true,
            shiftKey: true,
            metaKey: false,
        });
    });
    it.each(['KeyC', 'KeyZ'])(
        'keeps the held left Alt mapping for %s after paste',
        async (code) => {
            const v = view();
            Object.defineProperty(navigator, 'clipboard', {
                configurable: true,
                value: { readText: vi.fn().mockResolvedValue('local text') },
            });
            v.handleKeyDown(key('AltLeft', { altKey: true }));
            v.handleKeyDown(key('KeyV', { altKey: true }));
            await Promise.resolve();
            v.handleKeyUp(key('KeyV', { altKey: true }));
            v.handleKeyDown(key(code, { key: code.slice(-1).toLowerCase(), altKey: true }));
            expect(v.sent.at(-1)).toMatchObject({ code, char: null, metaKey: true, altKey: false });
            v._sendInputState(true);
            expect(v.sent.at(-1).keys.find((k) => k.code === code)).toMatchObject({
                char: null,
                metaKey: true,
                altKey: false,
            });
            v.handleKeyUp(key(code, { altKey: true }));
            v.handleKeyUp(key('AltLeft'));
            v.handleKeyDown(key('AltRight', { altKey: true }));
            v.handleKeyDown(key(code, { key: code.slice(-1).toLowerCase(), altKey: true }));
            expect(v.sent.at(-1)).toMatchObject({ code, metaKey: false, altKey: true });
            v._releaseAllPhysKeys();
            expect(v._keyMapper.physicalModifiers.size).toBe(0);
        },
    );
    it('does not replace Ctrl+V with Command+V on a Mac host', () => {
        const v = view();
        v.handleKeyDown(key('ControlLeft', { ctrlKey: true }));
        v.handleKeyDown(key('KeyV', { ctrlKey: true }));
        expect(v.sent.at(-1)).toMatchObject({
            type: 'keydown',
            code: 'KeyV',
            ctrlKey: true,
            metaKey: false,
        });
        expect(v._pendingPasteKey).toBeUndefined();
    });
    it('reads local clipboard for Alt+V mapped to host Command+V', async () => {
        const v = view();
        Object.defineProperty(navigator, 'clipboard', {
            configurable: true,
            value: { readText: vi.fn().mockResolvedValue('local text') },
        });
        v.handleKeyDown(key('AltLeft', { altKey: true }));
        v.handleKeyDown(key('KeyV', { altKey: true }));
        await Promise.resolve();
        expect(v.sent.at(-1)).toEqual({
            type: 'clipboardpaste',
            text: 'local text',
            injectCtrl: true,
        });
        expect(v._heldPhysKeys.size).toBe(0);
        expect(v.sent.some((m) => m.type === 'keydown' && m.code === 'KeyV')).toBe(false);
    });
    it('drops an in-flight browser read when key mappings change', async () => {
        const v = view();
        let resolve;
        Object.defineProperty(navigator, 'clipboard', {
            configurable: true,
            value: {
                readText: () =>
                    new Promise((r) => {
                        resolve = r;
                    }),
            },
        });
        v._readClipboardForHost('KeyV');
        v._setInputPreferences({ mappings: {} });
        resolve('secret');
        await Promise.resolve();
        expect(v.sent.some((m) => m.type === 'clipboardpaste')).toBe(false);
    });
    it('ignores keys mapped to an empty target, including heartbeats', () => {
        const m = mapper({ AltLeft: '' });
        const e = event('keydown', 'AltLeft', { altKey: true });
        expect(m.send(e)).toBe(null);
        expect(m.held([e])).toEqual([]);
        expect(m.send(event('keydown', 'KeyV', { altKey: true })).altKey).toBe(false);
        expect(m.send(event('keyup', 'AltLeft'))).toBe(null);
    });
    it('provides independent from/to rows and no preset button', () => {
        const v = sink();
        v.streamEl = document.createElement('div');
        document.body.append(v.streamEl);
        HTMLDialogElement.prototype.showModal = vi.fn();
        openKeyMappingDialog(v, structuredClone(v._inputPreferences));
        const dialog = document.querySelector('dialog');
        expect(dialog.querySelectorAll('tbody tr').length).toBe(2);
        expect(dialog.querySelectorAll('tbody input').length).toBe(4);
        expect(dialog.querySelectorAll('tbody select').length).toBe(0);
        expect(dialog.querySelector('.session-input-info').getAttribute('title')).toBeTruthy();
        expect(dialog.querySelector('details').open).toBe(false);
        expect(dialog.querySelector('details button')).not.toBeNull();
    });
});

it('holds mapped movement keys through a gaming stall using the target key', () => {
    const m = new SessionKeyMapper(
        { KeyI: 'KeyW', KeyW: 'KeyB' },
        StreamView.codeToWindowsVk,
        (code) => StreamView.HOLD_THROUGH_STALL_CODES.has(code),
    );
    expect(m.send(event('keydown', 'KeyI')).hold).toBe(true);
    expect(m.send(event('keydown', 'KeyW', { hold: true })).hold).toBe(false);
});

describe('key mapping editor', () => {
    let v;
    const buttons = (dialog, label) =>
        [...dialog.querySelectorAll('button')].find((b) => b.textContent === label);
    beforeEach(() => {
        HTMLDialogElement.prototype.showModal = function () {
            this.open = true;
        };
        HTMLDialogElement.prototype.close = function () {
            this.open = false;
            this.dispatchEvent(new Event('close'));
        };
        v = sink();
        v._inputStorageKey = inputStorageKey({ uuid: 'editor-host' });
        v._inputPreferences = normalizeInput({ mappings: { AltLeft: 'MetaLeft' } });
        v._keyMapper = mapper(v._inputPreferences.mappings);
        v.streamEl = document.createElement('div');
        document.body.append(v.streamEl);
        openSessionInputPanel(v);
    });
    it('applies and persists mappings without a separate save option', () => {
        const panel = v._inputDialog;
        expect(panel.querySelector('input[type=checkbox]')).toBeNull();
        panel.querySelectorAll('tbody input')[1].value = 'ControlLeft';
        buttons(panel, t('sessionInput.confirm')).click();
        expect(v._inputPreferences).toEqual({
            mappings: { AltLeft: 'ControlLeft' },
        });
        expect(JSON.parse(localStorage.getItem(v._inputStorageKey))).toEqual(v._inputPreferences);
    });
    it('removes a persistent mapping when its row is deleted', () => {
        buttons(v._inputDialog, t('sessionInput.confirm')).click();
        expect(loadInput(v._inputStorageKey).mappings).toEqual({ AltLeft: 'MetaLeft' });
        openSessionInputPanel(v);
        const panel = v._inputDialog;
        panel.querySelector('tbody button').click();
        buttons(panel, t('sessionInput.confirm')).click();
        expect(loadInput(v._inputStorageKey).mappings).toEqual({});
        expect(v._inputPreferences.mappings).toEqual({});
    });
    it.each(['close', 'escape'])('discards edits on %s', (action) => {
        const panel = v._inputDialog;
        panel.querySelectorAll('tbody input')[1].value = 'ShiftLeft';
        if (action === 'close') buttons(panel, t('sessionInput.close')).click();
        else {
            panel.dispatchEvent(new Event('cancel'));
            panel.close();
        }
        expect(v._inputPreferences.mappings.AltLeft).toBe('MetaLeft');
        expect(localStorage.getItem(v._inputStorageKey)).toBeNull();
    });
    it('rejects identical source/target codes and permits an empty target', () => {
        const panel = v._inputDialog;
        panel.querySelectorAll('tbody input')[1].value = 'AltLeft';
        buttons(panel, t('sessionInput.confirm')).click();
        expect(panel.open).toBe(true);
        expect(panel.querySelector('[role=status]').textContent).toBe(t('sessionInput.sameCode'));
        panel.querySelectorAll('tbody input')[1].value = '';
        buttons(panel, t('sessionInput.confirm')).click();
        expect(v._inputPreferences.mappings.AltLeft).toBe('');
    });
});
