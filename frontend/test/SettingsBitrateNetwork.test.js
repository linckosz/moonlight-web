/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * Where the host sees this browser, on the bitrate's line (decision of
 * 11/11/2026): a house before the value on the host's own network, a globe
 * over the Internet, and "auto (local network)" or "auto (Internet)" while
 * the value is the estimate's — which is then the LAN rule's or the
 * Internet's, as a launch from here asks (util/AutoBitrate.js).
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
    t: (key) => 'text:' + key,
    getLanguage: () => 'en',
    setLanguage: vi.fn(),
    AVAILABLE_LANGUAGES: [{ code: 'en', label: 'English' }],
}));

import { BackendClient } from '../js/api/BackendClient.js';
import { SettingsView } from '../js/ui/SettingsView.js';

describe('SettingsView: the bitrate says where the host sees this browser', () => {
    let view;
    const $ = (sel) => view.container.querySelector(sel);

    async function open(server) {
        BackendClient.getStreamingSettings.mockResolvedValue(server);
        view = new SettingsView(document.getElementById('settings'), () => {});
        await view._loadState();
        view.render();
        view.bindEvents();
    }

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        vi.spyOn(console, 'warn').mockImplementation(() => {});
    });

    it('on the host network: the house, "auto (local network)", the LAN estimate', async () => {
        await open({ viewer_lan: true });
        expect($('#settings-bitrate-net').getAttribute('aria-label')).toBe(
            'text:settings.bitrateNetLan',
        );
        expect($('#settings-bitrate-auto').textContent).toBe('text:settings.bitrateAutoLan');
        const lan = view._estimateBitrate();
        expect($('#settings-stream-bitrate').value).toBe(String(lan));
        expect($('#settings-bitrate-value').textContent).toBe(String(lan));
        // The same choices over the Internet ask for less.
        view._viewerPath = 'internet';
        expect(view._estimateBitrate()).toBeLessThan(lan);
    });

    it('over the Internet: the globe and "auto (Internet)"', async () => {
        await open({ viewer_lan: false });
        expect($('#settings-bitrate-net').getAttribute('aria-label')).toBe(
            'text:settings.bitrateNetInternet',
        );
        expect($('#settings-bitrate-auto').textContent).toBe('text:settings.bitrateAutoInternet');
    });

    it('keeps the icon on a bitrate of the user’s own, without "auto"', async () => {
        await open({ viewer_lan: true });
        const slider = $('#settings-stream-bitrate');
        slider.value = '40';
        slider.dispatchEvent(new Event('change'));
        expect(view._bitrateAuto).toBe(false);
        expect($('#settings-bitrate-auto').textContent).toBe('');
        expect($('#settings-bitrate-net')).not.toBeNull();
        $('#settings-bitrate-back-to-auto').click();
        expect($('#settings-bitrate-auto').textContent).toBe('text:settings.bitrateAutoLan');
    });

    it('says neither when the server does not', async () => {
        await open({});
        expect($('#settings-bitrate-net')).toBeNull();
        expect($('#settings-bitrate-auto').textContent).toBe('text:settings.bitrateAuto');
    });
});
