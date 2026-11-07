/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The resolution choice on the Settings page, and the ratio it retired. The
 * ratio dropdown left the product in 0.3.1: a user must not find it, a value
 * they forced before must not survive a save, and picking any resolution
 * puts the ratio back to Auto — while a debug build keeps all of it for tests.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

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

const STORAGE_KEY = 'mw-streaming-settings';

describe('SettingsView resolution', () => {
    let view;

    const stored = () => JSON.parse(localStorage.getItem(STORAGE_KEY));
    const select = () => view.container.querySelector('#settings-stream-height');
    const pick = async (value) => {
        select().value = value;
        select().dispatchEvent(new Event('change'));
        // _autoSave debounces 300 ms.
        await new Promise((r) => setTimeout(r, 350));
    };

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
    });

    it('offers Auto, this screen and custom, then the rungs largest first', () => {
        view.render();
        const values = [...select().options].map((o) => o.value);
        expect(values).toEqual(['auto', 'device', 'custom', '2160', '1440', '1080', '720']);
        expect(select().value).toBe('auto');
        expect(view.container.querySelector('#settings-custom-width')).toBeNull();
    });

    // The bitrate follows the estimate until the slider leaves it, and is the
    // user's own from then on — whatever the resolution becomes.
    it('keeps a bitrate the user set, and follows the estimate otherwise', async () => {
        view.render();
        view.bindEvents();
        expect(view._bitrateAuto).toBe(true);
        const slider = view.container.querySelector('#settings-stream-bitrate');
        slider.value = '7';
        slider.dispatchEvent(new Event('change'));
        await new Promise((r) => setTimeout(r, 350));
        expect(view._bitrateAuto).toBe(false);
        expect(stored().stream_bitrate_auto).toBe(false);
        expect(stored().stream_bitrate).toBe(7000);
        await pick('720');
        expect(stored().stream_bitrate).toBe(7000);
        // Back on the estimate: auto again, and it moves with the rung.
        slider.value = String(view._estimateBitrate());
        slider.dispatchEvent(new Event('change'));
        await new Promise((r) => setTimeout(r, 350));
        expect(stored().stream_bitrate_auto).toBe(true);
        await pick('2160');
        expect(stored().stream_bitrate).toBe(view._estimateBitrate() * 1000);
    });

    // The pixel budget leaves a named rung its lines (issue #38): with the
    // rate on Auto, 2160p at 60 is over the budget, and the estimate is
    // still the one for the 2160 lines the stream carries.
    it('recommends the bitrate of a named rung at its full size', async () => {
        view.render();
        view.bindEvents();
        await pick('2160');
        expect(view._estimateBitrate()).toBe(computeAutoBitrate(2160, '16:9', false, false));
    });

    it('hides the ratio from a user, and shows it to a debug build', () => {
        view.render();
        expect(view.container.querySelector('#settings-stream-aspect')).toBeNull();
        view._debugBuild = true;
        view.render();
        expect(view.container.querySelector('#settings-stream-aspect')).not.toBeNull();
    });

    // The 0.3.1 migration: a "4:3" forced under 0.3.0 is Auto from the next
    // save on, and never marked as a deliberate override.
    it('saves the ratio as Auto whatever was stored, outside a debug build', async () => {
        view._applySettings({ stream_aspect: '4:3', stream_height: 1440 });
        expect(view._streamAspect).toBe('4:3');
        view.render();
        view.bindEvents();
        expect(view._streamAspect).toBe('auto');
        await view._saveToStorage();
        expect(stored().stream_aspect).toBe('auto');
        expect(stored().stream_aspect_forced).toBeUndefined();
        expect(stored().stream_height).toBe(1440);
        // A store from before the choice existed is Auto, the rung kept.
        expect(stored().stream_resolution).toBe('auto');
    });

    it('keeps a forced ratio in a debug build, and says so', async () => {
        view._debugBuild = true;
        view._applySettings({ stream_aspect: '4:3' });
        view.render();
        view.bindEvents();
        await view._saveToStorage();
        expect(stored().stream_aspect).toBe('4:3');
        expect(stored().stream_aspect_forced).toBe(true);
    });

    it('puts the ratio back to Auto when a resolution is picked', async () => {
        view._debugBuild = true;
        view._applySettings({ stream_aspect: '4:3' });
        view.render();
        view.bindEvents();
        await pick('device');
        expect(view._streamAspect).toBe('auto');
        expect(stored().stream_aspect).toBe('auto');
        expect(stored().stream_resolution).toBe('device');
        // The rung is kept for the day the user comes back to it.
        expect(stored().stream_height).toBe(1080);
    });

    it('shows the custom pair with its choice, pinned into bounds and even', async () => {
        view.render();
        view.bindEvents();
        await pick('custom');
        const w = view.container.querySelector('#settings-custom-width');
        const h = view.container.querySelector('#settings-custom-height');
        expect(w).not.toBeNull();
        expect(w.value).toBe('1920');
        expect(h.value).toBe('1080');
        w.value = '9999';
        w.dispatchEvent(new Event('change'));
        h.value = '767';
        h.dispatchEvent(new Event('change'));
        await new Promise((r) => setTimeout(r, 350));
        expect(w.value).toBe('4096');
        expect(h.value).toBe('766');
        expect(stored().stream_custom_width).toBe(4096);
        expect(stored().stream_custom_height).toBe(766);
        expect(stored().stream_resolution).toBe('custom');
        // Back to a rung: the pair goes, the value it held stays stored.
        await pick('720');
        expect(view.container.querySelector('#settings-custom-width')).toBeNull();
        expect(stored().stream_resolution).toBe('fixed');
        expect(stored().stream_height).toBe(720);
        expect(stored().stream_custom_width).toBe(4096);
    });

    it('reads an unknown stored choice as Auto, the rung kept for later', () => {
        view._applySettings({ stream_resolution: 'native', stream_height: 2160 });
        view.render();
        expect(select().value).toBe('auto');
        expect(view._streamHeight).toBe(2160);
    });
});
