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

    it('shows the defaults, and one entry for everything the editor sets', () => {
        setTouch(true);
        paint();
        expect(field('mode').value).toBe('auto');
        expect(field('vibrate').checked).toBe(true);
        expect(field('outside').value).toBe('mouse');
        // Size, opacity and layout are the editor's now, behind one button.
        for (const gone of ['scale', 'opacity', 'layout']) expect(field(gone)).toBeNull();
        expect(field('edit').textContent).toContain('text:settings.touchPadCustomizeButton');
    });

    it('says in one line how the controller is set', () => {
        setTouch(true);
        view._touchPad = touchPadSettings({
            touch_pad_scale: 1.2,
            touch_pad_opacity: 0.45,
            touch_pad_layout: { preset: 'left' },
        });
        paint();
        expect(field('summary').textContent).toBe(
            [
                'text:settings.touchPadLayoutLeft',
                'text:settings.touchPadSummarySize',
                'text:settings.touchPadSummaryOpacity',
            ].join(' · '),
        );
        view._touchPad = touchPadSettings({
            touch_pad_layout: { portrait: { a: { x: 0.5, y: 0.5, s: 1 } } },
        });
        expect(view._touchPadSummary()).toContain('text:settings.touchPadSummaryMoved');
    });

    it('reads back what the viewer set, and what the editor last gave', () => {
        setTouch(true);
        view._touchPad = touchPadSettings({ touch_pad_scale: 1.2, touch_pad_opacity: 0.45 });
        paint();
        field('mode').value = 'always';
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

    it('saves on every change', () => {
        setTouch(true);
        paint();
        for (const name of ['mode', 'vibrate', 'outside']) {
            field(name).dispatchEvent(new Event('change'));
        }
        expect(view._autoSave).toHaveBeenCalledTimes(3);
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

    it('opens the editor with what is saved, and keeps what it gives back', () => {
        setTouch(true);
        view._touchPad = touchPadSettings({
            touch_pad_scale: 0.9,
            touch_pad_opacity: 0.6,
            touch_pad_layout: { landscape: { a: { x: 0.5, y: 0.5, s: 1 } } },
        });
        paint();
        field('edit').click();
        expect(editors.length).toBe(1);
        const editor = editors[0];
        expect(editor.opened).toBe(true);
        expect(editor.container).toBe(document.body);
        expect(editor.options.layout).toEqual(view._touchPad.layout);
        expect(editor.options.scale).toBe(0.9);
        expect(editor.options.opacity).toBe(0.6);

        const layout = { preset: 'left', landscape: { a: { x: 0.2, y: 0.8, s: 1 } }, portrait: {} };
        editor.options.onChange(layout, { scale: 1.3, opacity: 0.4 });
        expect(view._touchPad.layout).toEqual(layout);
        expect(view._autoSave).toHaveBeenCalled();
        const read = view._readTouchPadSection();
        expect(read.layout).toEqual(layout);
        expect(read.scale).toBe(1.3);
        expect(read.opacity).toBe(0.4);
        // The line under the button follows at once.
        expect(field('summary').textContent).toContain('text:settings.touchPadLayoutLeft');
        expect(field('summary').textContent).toContain('text:settings.touchPadSummaryMoved');

        editor.options.onClosed();
        expect(view._touchPadEditor).toBeNull();
    });
});
