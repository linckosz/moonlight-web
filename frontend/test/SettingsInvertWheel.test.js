/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * « Invert mouse wheel scrolling » (issue #52, v0.4.4): off by default, kept
 * by this browser alone, and offered only where a page can tell a notched
 * wheel from a trackpad (Chrome, Edge and Safari on a Mac). Elsewhere it shows
 * "(unavailable)", greyed out and unticked, like « Allow tearing ».
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
        saveStreamingSettings: vi.fn(async () => ({})),
        getHosts: vi.fn(async () => ({ hosts: [] })),
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
// The mouse settings live in the block a desktop's mouse gets; jsdom, which
// has touch events, reads like a touch device.
vi.mock('../js/util/BrowserDetect.js', async (importOriginal) => ({
    ...(await importOriginal()),
    supportsGamingMode: () => true,
}));

import { SettingsView } from '../js/ui/SettingsView.js';

const UA = {
    chromeMac:
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/126.0.0.0 Safari/537.36',
    safariMac:
        'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/27.0 Safari/605.1.15',
    chromeWindows:
        'Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/157.0.0.0 Safari/537.36',
};

describe('SettingsView: invert the mouse wheel', () => {
    let view;
    const box = () => view.container.querySelector('#settings-invert-wheel');
    const field = () => box().closest('.settings-field');
    const stored = () => JSON.parse(localStorage.getItem('mw-streaming-settings') || '{}');
    const mount = () => {
        view.render();
        view.bindEvents();
    };
    const setUa = (ua) =>
        Object.defineProperty(navigator, 'userAgent', { value: ua, configurable: true });

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
        view._codecSupport = { h264: true, hevc: true, av1: true };
    });
    afterEach(() => {
        delete navigator.userAgent;
    });

    it('is offered unticked under the mouse sensitivity, in Chrome on a Mac', () => {
        setUa(UA.chromeMac);
        mount();
        expect(box().checked).toBe(false);
        expect(box().disabled).toBe(false);
        expect(field().classList.contains('settings-field-locked')).toBe(false);
        expect(field().textContent).toContain('text:settings.invertWheelDesc');
        expect(field().textContent).not.toContain('text:settings.unavailableSuffix');
        const sensitivity = view.container.querySelector('#settings-mouse-sensitivity');
        const fields = Array.from(view.container.querySelectorAll('.settings-field'));
        expect(fields.indexOf(field())).toBe(
            fields.indexOf(sensitivity.closest('.settings-field')) + 1,
        );
    });

    it('is offered in Safari on a Mac', () => {
        setUa(UA.safariMac);
        mount();
        expect(box().disabled).toBe(false);
    });

    it('is greyed out and unavailable where a wheel cannot be told from a touchpad', () => {
        setUa(UA.chromeWindows);
        view._invertWheel = true;
        mount();
        expect(box().disabled).toBe(true);
        expect(box().checked).toBe(false);
        expect(field().classList.contains('settings-field-locked')).toBe(true);
        expect(field().textContent).toContain('text:settings.unavailableSuffix');
    });

    it('ticked, is kept by this browser', async () => {
        setUa(UA.chromeMac);
        mount();
        box().checked = true;
        box().dispatchEvent(new Event('change'));
        await vi.waitFor(() => expect(stored().invert_wheel).toBe(true));
        expect(view._invertWheel).toBe(true);
    });

    it('comes back ticked from what this browser kept, and off when nothing was', () => {
        setUa(UA.chromeMac);
        view._applySettings({});
        expect(view._invertWheel).toBe(false);
        view._applySettings({ invert_wheel: true });
        mount();
        expect(box().checked).toBe(true);
    });
});
