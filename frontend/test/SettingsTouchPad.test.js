/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The on-screen controller's section of the Settings page: there only where a
 * touch screen is on, and saving what it says.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

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
});
