/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The frame rate Auto shows on the Settings page, under the pixel budget
 * (util/StreamResolution.js, fitPixelBudget). On a 4K screen at 120 Hz, the
 * Auto line must name the rate the stream will really be sent at for the
 * resolution shown — 60 for a 2160p or a 1440p rung, 90 for an ultrawide
 * 2560×1080 — and the bitrate it
 * recommends must be the one for that pair, a rate the viewer named kept.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/util/RefreshRate.js', async (importOriginal) => ({
    ...(await importOriginal()),
    measuredFps: () => 120,
    autoFps: () => 120,
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

describe('SettingsView frame rate under the pixel budget', () => {
    let view;
    let savedScreen;
    let savedRatio;

    const autoLine = () =>
        view.container.querySelector('#settings-stream-fps option[value="0"]').textContent;
    const choose = async (id, value) => {
        const el = view.container.querySelector(id);
        el.value = value;
        el.dispatchEvent(new Event('change'));
        // _autoSave debounces 300 ms.
        await new Promise((r) => setTimeout(r, 350));
    };

    beforeEach(() => {
        localStorage.clear();
        // A 4K monitor at a device pixel ratio of 1, refreshing at 120 Hz.
        savedScreen = Object.getOwnPropertyDescriptor(window, 'screen');
        savedRatio = window.devicePixelRatio;
        Object.defineProperty(window, 'screen', {
            value: { width: 3840, height: 2160 },
            configurable: true,
        });
        window.devicePixelRatio = 1;
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
        view.render();
        view.bindEvents();
    });

    afterEach(() => {
        if (savedScreen) Object.defineProperty(window, 'screen', savedScreen);
        else delete window.screen;
        window.devicePixelRatio = savedRatio;
    });

    it("says Auto/Auto streams at the screen's own 120", () => {
        // Auto's 1440 box comes down to 1080 lines, and the 120 stands.
        expect(autoLine()).toBe('text:settings.fpsAutoMeasured:{"fps":120}');
    });

    it('names the 60 a 2160p or a 1440p rung is really sent at', async () => {
        await choose('#settings-stream-height', '2160');
        expect(autoLine()).toBe('text:settings.fpsAutoBudget:{"fps":60}');
        expect(view._estimateBitrate()).toBe(computeAutoBitrate(2160, '3840:2160', false, false));
        // A rung to a rung re-renders nothing: the line follows on its own.
        await choose('#settings-stream-height', '1440');
        expect(autoLine()).toBe('text:settings.fpsAutoBudget:{"fps":60}');
        await choose('#settings-stream-height', '1080');
        expect(autoLine()).toBe('text:settings.fpsAutoMeasured:{"fps":120}');
    });

    it('names the 90 an ultrawide frame keeps, off the grid as it is', async () => {
        // 2560×1080 fits at 90; the 60 under it on the 120 Hz grid would
        // cost a third, so 90 is what is sent, and what the line says.
        await choose('#settings-stream-height', 'custom');
        await choose('#settings-custom-width', '2560');
        expect(autoLine()).toBe('text:settings.fpsAutoBudget:{"fps":90}');
    });

    it('recommends the bitrate of a named rate at the lines Auto leaves it', async () => {
        await choose('#settings-stream-fps', '144');
        expect(view._estimateBitrate()).toBe(computeAutoBitrate(1080, '1920:1080', false, false));
    });
});
