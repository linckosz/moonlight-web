/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The resolution choice on a TV's Settings page. Its Auto stops at 720 lines
 * (util/StreamResolution.js, TV_AUTO_MAX_HEIGHT): the line under the choice
 * must say so, and the bitrate it recommends must be the one for the 720p the
 * stream carries — not for the 1080p screen it lands on.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/util/BrowserDetect.js', async (importOriginal) => ({
    ...(await importOriginal()),
    IS_TV: true,
}));
vi.mock('../js/api/BackendClient.js', () => ({
    BackendClient: {
        getAuthStatus: vi.fn(async () => ({ has_session: false })),
        getStreamingSettings: vi.fn(async () => ({})),
        saveStreamingSettings: vi.fn(async () => ({})),
        getMetricsReporting: vi.fn(),
        setMetricsReporting: vi.fn(async () => ({ enabled: true })),
    },
}));
vi.mock('../js/ui/Toast.js', () => ({
    Toast: { success: vi.fn(), error: vi.fn(), warning: vi.fn(), info: vi.fn() },
}));
vi.mock('../js/i18n/i18n.js', () => ({
    t: (key, params) => 'text:' + key + (params ? ':' + JSON.stringify(params) : ''),
    getLanguage: () => 'en',
    setLanguage: vi.fn(),
    AVAILABLE_LANGUAGES: [{ code: 'en', label: 'English' }],
}));

import { SettingsView } from '../js/ui/SettingsView.js';
import { computeAutoBitrate } from '../js/util/AutoBitrate.js';

describe('SettingsView resolution on a TV', () => {
    let view;
    let savedScreen;
    let savedRatio;

    beforeEach(() => {
        localStorage.clear();
        // A Freebox Player POP in TV Bro: 960×540 at a device pixel ratio of 2.
        savedScreen = Object.getOwnPropertyDescriptor(window, 'screen');
        savedRatio = window.devicePixelRatio;
        Object.defineProperty(window, 'screen', {
            value: { width: 960, height: 540 },
            configurable: true,
        });
        window.devicePixelRatio = 2;
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
    });

    afterEach(() => {
        if (savedScreen) Object.defineProperty(window, 'screen', savedScreen);
        else delete window.screen;
        window.devicePixelRatio = savedRatio;
    });

    it('says that Auto stops at 720p on a TV, and what "Match my screen" gives', () => {
        view.render();
        const text = view.container.textContent;
        expect(text).toContain('text:settings.resolutionAutoDescTv:{"size":"1920×1080"}');
        expect(text).not.toContain('text:settings.resolutionAutoDesc:');
    });

    it('recommends the bitrate of the 720p it streams', () => {
        view.render();
        expect(view._estimateBitrate()).toBe(computeAutoBitrate(720, '1280:720', false, false));
        expect(view._estimateBitrate()).toBeLessThan(
            computeAutoBitrate(1080, '1920:1080', false, false),
        );
    });
});
