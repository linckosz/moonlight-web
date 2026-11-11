/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Settings' "MoonlightWeb Host" section: this browser's choices that only a
 * MoonlightWeb host acts on (Stream audio from, Turn off the host's other
 * screens), gathered out of Advanced under the admin's section name. Same rule
 * as the admin's: each choice only where a host can honour it, the section
 * only when it has one to offer.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
        saveStreamingSettings: vi.fn(async () => ({})),
        getHosts: vi.fn(async () => ({ hosts: [] })),
        getAudioFocused: vi.fn(async () => ({ app: '' })),
        getAudioApps: vi.fn(async () => ({ apps: [] })),
        getMetricsReporting: vi.fn(),
        setMetricsReporting: vi.fn(async () => ({ enabled: true })),
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

describe('SettingsView: the MoonlightWeb Host section', () => {
    let view;
    const $ = (sel) => view.container.querySelector(sel);
    const section = () => $('#settings-section-native-host');
    const titles = () =>
        Array.from(view.container.querySelectorAll('.settings-section-title')).map(
            (h) => h.textContent,
        );
    const mount = () => {
        view.render();
        view.bindEvents();
    };

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
        view._codecSupport = { h264: true, hevc: true, av1: true };
        view._autoSave = vi.fn();
    });

    afterEach(() => view.destroy());

    it('is not there at all where no host offers either choice', () => {
        mount();
        expect(section()).toBeNull();
        expect($('#settings-audio-source')).toBeNull();
        expect($('#settings-others-off')).toBeNull();
        expect(titles()).not.toContain('text:admin.nativeHost');
    });

    it("carries the admin's name, its own line, and comes right after Advanced", () => {
        view._audioSourceSupported = true;
        mount();
        expect(section().querySelector('.settings-section-title').textContent).toBe(
            'text:admin.nativeHost',
        );
        expect(section().querySelector('.settings-section-desc').textContent).toBe(
            'text:settings.nativeHostDesc',
        );
        const order = titles();
        expect(order.indexOf('text:admin.nativeHost')).toBe(
            order.indexOf('text:settings.advanced') + 1,
        );
        expect(order.indexOf('text:settings.language')).toBe(
            order.indexOf('text:admin.nativeHost') + 1,
        );
    });

    it('holds Stream audio from, open, where the host can follow one app', () => {
        view._audioSourceSupported = true;
        mount();
        const select = $('#settings-audio-source');
        expect(select.closest('.settings-section')).toBe(section());
        expect(select.disabled).toBe(false);
        // Not offered, so not shown: the other choice stays out.
        expect($('#settings-others-off')).toBeNull();

        select.value = 'focused';
        select.dispatchEvent(new Event('change'));
        expect(view._audioSource).toBe('focused');
        expect(view._autoSave).toHaveBeenCalled();
    });

    it("holds Turn off the host's other screens where a host allows it", () => {
        view._othersOffOffer = { inherited: false };
        mount();
        const box = $('#settings-others-off');
        expect(box.closest('.settings-section')).toBe(section());
        expect(box.checked).toBe(false);
        // The host cannot follow one app: that choice stays out.
        expect($('#settings-audio-source')).toBeNull();

        box.checked = true;
        box.dispatchEvent(new Event('change'));
        expect(view._othersOff).toBe(true);
        expect(view._autoSave).toHaveBeenCalled();
    });

    it('holds both, Stream audio from first, and neither is left in Advanced', () => {
        view._audioSourceSupported = true;
        view._othersOffOffer = { inherited: true };
        mount();
        const fields = Array.from(section().querySelectorAll('select, input')).map((el) => el.id);
        expect(fields).toEqual(['settings-audio-source', 'settings-others-off']);
        // A PC whose admin box was ticked before the choice moved to the viewer.
        expect($('#settings-others-off').checked).toBe(true);
        expect(view.container.querySelectorAll('#settings-audio-source')).toHaveLength(1);
        expect(view.container.querySelectorAll('#settings-others-off')).toHaveLength(1);
    });
});
