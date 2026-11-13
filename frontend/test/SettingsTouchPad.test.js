/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The on-screen controller's section of the Settings page: there only where a
 * touch screen is on, and saving what it says.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

const { editors } = vi.hoisted(() => ({ editors: [] }));
vi.mock('../js/ui/TouchPadLayoutEditor.js', () => ({
    TouchPadLayoutEditor: class {
        constructor(container, options) {
            this.container = container;
            this.options = options;
            this.opened = false;
            editors.push(this);
        }
        open() {
            this.opened = true;
        }
    },
}));

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key) => 'text:' + key,
    getLanguage: () => 'en',
    setLanguage: vi.fn(),
    AVAILABLE_LANGUAGES: [{ code: 'en', label: 'English' }],
}));

import { SettingsView } from '../js/ui/SettingsView.js';
import { touchPadSettings } from '../js/stream/TouchPadState.js';

describe('SettingsView on-screen controller section', () => {
    let view;
    let media;

    const setTouch = (on) => {
        Object.defineProperty(navigator, 'maxTouchPoints', {
            value: on ? 5 : 0,
            configurable: true,
        });
        media['(any-pointer: coarse)'] = on;
    };

    /** Just the section, in the page's container, with its handlers. */
    const paint = () => {
        view.container.innerHTML =
            view._renderTouchPadSection() + '<div class="settings-section-shortcuts"></div>';
        view._bindTouchPadSection();
    };
    const field = (name) => view.container.querySelector('#settings-touch-pad-' + name);

    beforeEach(() => {
        media = {};
        window.matchMedia = (q) => ({
            matches: media[q] === true,
            addEventListener: () => {},
            removeEventListener: () => {},
        });
        const container = document.createElement('div');
        document.body.appendChild(container);
        view = Object.create(SettingsView.prototype);
        view.container = container;
        view._touchPad = touchPadSettings();
        view._autoSave = vi.fn();
        view._refreshShortcutsSection = vi.fn();
    });
    afterEach(() => {
        document.body.innerHTML = '';
        delete window.matchMedia;
        delete navigator.maxTouchPoints;
    });

    it('is not there without a touch screen the OS has on', () => {
        setTouch(false);
        expect(view._renderTouchPadSection()).toBe('');
    });

    it('shows the defaults, the opacity up to 100%', () => {
        setTouch(true);
        paint();
        expect(field('mode').value).toBe('auto');
        expect(field('scale').value).toBe('100');
        expect(field('scale').min).toBe('80');
        expect(field('scale').max).toBe('140');
        expect(field('opacity').value).toBe('100');
        expect(field('opacity').min).toBe('20');
        expect(field('opacity').max).toBe('100');
        expect(field('vibrate').checked).toBe(true);
        expect(field('outside').value).toBe('mouse');
    });

    it('reads back what the viewer set', () => {
        setTouch(true);
        paint();
        field('mode').value = 'always';
        field('scale').value = '120';
        field('opacity').value = '45';
        field('vibrate').checked = false;
        field('outside').value = 'ignore';
        expect(view._readTouchPadSection()).toEqual({
            mode: 'always',
            scale: 1.2,
            opacity: 0.45,
            vibrate: false,
            outside: 'ignore',
            layout: { preset: 'default', landscape: {}, portrait: {} },
        });
    });

    it('keeps what was saved when the section is not on the page', () => {
        setTouch(false);
        view._touchPad = touchPadSettings({ touch_pad_mode: 'never' });
        view.container.innerHTML = '';
        expect(view._readTouchPadSection().mode).toBe('never');
    });

    it('saves on every change, and shows a slider as it moves', () => {
        setTouch(true);
        paint();
        field('opacity').value = '60';
        field('opacity').dispatchEvent(new Event('input'));
        expect(field('opacity-value').textContent).toBe('60');
        expect(view._autoSave).not.toHaveBeenCalled();
        for (const name of ['mode', 'scale', 'opacity', 'vibrate', 'outside']) {
            field(name).dispatchEvent(new Event('change'));
        }
        expect(view._autoSave).toHaveBeenCalledTimes(5);
    });

    it('takes the gesture off the help as soon as the mode says never', () => {
        setTouch(true);
        paint();
        expect(view._helpOpts().touchPad).toBe(true);
        field('mode').value = 'never';
        field('mode').dispatchEvent(new Event('change'));
        expect(view._refreshShortcutsSection).toHaveBeenCalled();
        expect(view._helpOpts().touchPad).toBe(false);
    });

    it('offers the presets, and starts from the one saved', () => {
        setTouch(true);
        view._touchPad = touchPadSettings({ touch_pad_layout: { preset: 'left' } });
        paint();
        expect(field('layout').value).toBe('left');
        const values = Array.from(field('layout').options).map((o) => o.value);
        expect(values).toEqual(['default', 'left', 'compact']);
    });

    it('takes a preset as a starting point, dropping the places it replaces', () => {
        setTouch(true);
        view._touchPad = touchPadSettings({
            touch_pad_layout: { landscape: { a: { x: 0.5, y: 0.5, s: 1 } } },
        });
        paint();
        field('layout').value = 'compact';
        field('layout').dispatchEvent(new Event('change'));
        expect(view._touchPad.layout).toEqual({
            preset: 'compact',
            landscape: {},
            portrait: {},
        });
        expect(view._autoSave).toHaveBeenCalledTimes(1);
    });

    it('opens the editor with what is saved, and keeps what it gives back', () => {
        setTouch(true);
        view._touchPad = touchPadSettings({
            touch_pad_layout: { landscape: { a: { x: 0.5, y: 0.5, s: 1 } } },
        });
        paint();
        view.container.querySelector('#settings-touch-pad-edit').click();
        expect(editors.length).toBe(1);
        const editor = editors[0];
        expect(editor.opened).toBe(true);
        expect(editor.container).toBe(document.body);
        expect(editor.options.layout).toEqual(view._touchPad.layout);

        const layout = { preset: 'left', landscape: { a: { x: 0.2, y: 0.8, s: 1 } }, portrait: {} };
        editor.options.onChange(layout);
        expect(view._touchPad.layout).toEqual(layout);
        expect(view._autoSave).toHaveBeenCalled();
        expect(view._readTouchPadSection().layout).toEqual(layout);

        editor.options.onClosed();
        expect(view._touchPadEditor).toBeNull();
    });
});
