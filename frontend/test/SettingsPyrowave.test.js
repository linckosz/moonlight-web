/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * « PyroWave (Wired LAN only) », a codec of this browser's Settings since it
 * left the admin page (13/10/2026). Offered where a host says it codes it (a
 * native Windows host on the D3D12 route) and this browser decodes it, never
 * with HDR or the <video> transport; a choice that cannot run streams Auto
 * (HEVC) without a word, and stays for the next host that can.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

const ultra = vi.hoisted(() => ({ supported: true }));

vi.mock('../js/stream/ultra/UltraPlayer.js', () => ({
    ultraPlayerSupported: () => ultra.supported,
    presentMode: () => null,
    UltraPlayer: class {},
}));
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

import { SettingsView } from '../js/ui/SettingsView.js';
import { BackendClient } from '../js/api/BackendClient.js';
import {
    anyHostCodesPyrowave,
    applyPyrowaveChoice,
    hostCodesPyrowave,
    pyrowaveBlocker,
} from '../js/util/PyrowaveCodec.js';

const windowsHost = { backendType: 'native', nativeDisplay: { state: 'ok', pyrowave: true } };
const linuxHost = { backendType: 'native', nativeDisplay: { state: 'ok', pyrowave: false } };
const sunshineHost = { backendType: '', nativeDisplay: null };

describe('PyrowaveCodec: which hosts code it', () => {
    it("is the host's card that says so, and only a native one's", () => {
        expect(hostCodesPyrowave(windowsHost)).toBe(true);
        expect(hostCodesPyrowave(linuxHost)).toBe(false);
        expect(hostCodesPyrowave(sunshineHost)).toBe(false);
        // A Sunshine-family card that would carry the flag still is not one.
        expect(hostCodesPyrowave({ backendType: 'wolf', nativeDisplay: { pyrowave: true } })).toBe(
            false,
        );
        expect(hostCodesPyrowave(undefined)).toBe(false);
        expect(anyHostCodesPyrowave([sunshineHost, windowsHost])).toBe(true);
        expect(anyHostCodesPyrowave([sunshineHost, linuxHost])).toBe(false);
        expect(anyHostCodesPyrowave(undefined)).toBe(false);
    });

    it('says why the menu cannot offer it, the transport first', () => {
        const ok = { hostCodes: true, browserDecodes: true };
        expect(pyrowaveBlocker(ok)).toBe(null);
        // The host list not answered yet blocks nothing: the launch decides.
        expect(pyrowaveBlocker({ ...ok, hostCodes: null })).toBe(null);
        expect(pyrowaveBlocker({ ...ok, hostCodes: false })).toBe('host');
        expect(pyrowaveBlocker({ ...ok, hdr: true })).toBe('hdr');
        expect(pyrowaveBlocker({ ...ok, browserDecodes: false })).toBe('browser');
        expect(pyrowaveBlocker({ ...ok, mediaTrackOnly: true, browserDecodes: false })).toBe(
            'transport',
        );
    });
});

describe('PyrowaveCodec: the launch', () => {
    beforeEach(() => {
        ultra.supported = true;
    });

    it('asks PyroWave of a host that codes it, HEVC beside it, 4:4:4 dropped', () => {
        const s = applyPyrowaveChoice(
            { video_codec: 'pyrowave', chroma_444_enabled: true },
            windowsHost,
        );
        expect(s.ultra_codec).toBe('pyrowave');
        expect(s.video_codec).toBe('hevc');
        expect(s.chroma_444_enabled).toBe(false);
    });

    it('is an Auto launch, without PyroWave, wherever it cannot run', () => {
        const cases = [
            [{ video_codec: 'pyrowave' }, sunshineHost, {}],
            [{ video_codec: 'pyrowave' }, linuxHost, {}],
            [{ video_codec: 'pyrowave', hdr_enabled: true }, windowsHost, {}],
            [{ video_codec: 'pyrowave', power_save: true }, windowsHost, {}],
            [{ video_codec: 'pyrowave' }, windowsHost, { hdrOverride: true }],
        ];
        for (const [settings, host, opts] of cases) {
            const s = applyPyrowaveChoice({ ...settings }, host, opts);
            expect(s.ultra_codec).toBeUndefined();
            expect(s.video_codec).toBe('hevc');
        }
        ultra.supported = false;
        const s = applyPyrowaveChoice({ video_codec: 'pyrowave' }, windowsHost);
        expect(s.ultra_codec).toBeUndefined();
        expect(s.video_codec).toBe('hevc');
    });

    it('gives way to a codec a fallback relaunch forced', () => {
        // app.js writes the forced codec before asking: H.264 after a failed decode.
        const s = applyPyrowaveChoice({ video_codec: 'h264' }, windowsHost, {
            codecOverride: 'h264',
        });
        expect(s.ultra_codec).toBeUndefined();
        expect(s.video_codec).toBe('h264');
    });

    it('leaves any other codec alone, and never keeps an older ask', () => {
        const s = applyPyrowaveChoice({ video_codec: 'av1', ultra_codec: 'pyrowave' }, windowsHost);
        expect(s.video_codec).toBe('av1');
        expect(s.ultra_codec).toBeUndefined();
    });
});

describe('SettingsView: PyroWave in the codec menu', () => {
    let view;
    const $ = (sel) => view.container.querySelector(sel);
    const option = () => $('#settings-video-codec option[value="pyrowave"]');
    const mount = () => {
        view.render();
        view.bindEvents();
    };

    beforeEach(() => {
        localStorage.clear();
        ultra.supported = true;
        document.body.innerHTML = '<div id="settings"></div>';
        vi.clearAllMocks();
        view = new SettingsView(document.getElementById('settings'), () => {});
        view._codecSupport = { h264: true, hevc: true, av1: true };
        view._pyrowaveHost = true;
    });

    it('is the last codec, under its own name, where a host codes it', () => {
        mount();
        const values = Array.from($('#settings-video-codec').options).map((o) => o.value);
        expect(values).toEqual(['auto', 'h264', 'hevc', 'av1', 'pyrowave']);
        expect(option().disabled).toBe(false);
        expect(option().textContent).toBe('text:settings.codecPyrowave');
    });

    it('learns from the host list whether a host codes it', async () => {
        BackendClient.getHosts.mockResolvedValueOnce({ hosts: [sunshineHost, windowsHost] });
        await view._loadPyrowaveHost();
        expect(view._pyrowaveHost).toBe(true);
        BackendClient.getHosts.mockResolvedValueOnce({ hosts: [sunshineHost] });
        await view._loadPyrowaveHost();
        expect(view._pyrowaveHost).toBe(false);
        // No answer: left open, the launch decides.
        BackendClient.getHosts.mockRejectedValueOnce(new Error('offline'));
        await view._loadPyrowaveHost();
        expect(view._pyrowaveHost).toBe(null);
    });

    it('is greyed out, with the reason, where no host codes it', () => {
        view._pyrowaveHost = false;
        mount();
        expect(option().disabled).toBe(true);
        expect(option().textContent).toBe('text:settings.codecUnavailable');
        expect(option().title).toBe('text:settings.pyrowaveNoHost');
    });

    it('is greyed out where this browser cannot decode it', () => {
        ultra.supported = false;
        mount();
        expect(option().disabled).toBe(true);
        expect(option().title).toBe('text:settings.pyrowaveNoBrowser');
    });

    it('is greyed out with HDR ticked, as H.264 is', () => {
        // A browser that can show HDR: elsewhere render() unticks the box.
        view._hdrCapability = { webgpu: true, decode: true };
        view._hdrEnabled = true;
        mount();
        expect(option().disabled).toBe(true);
        expect(option().textContent).toBe('text:settings.codecHdrUnavailable');
        expect(option().title).toBe('text:settings.pyrowaveNoHdr');
    });

    it('is greyed out in Power Saving (the <video> transport)', () => {
        view._powerSave = true;
        mount();
        expect(option().disabled).toBe(true);
        expect(option().title).toBe('text:settings.pyrowaveNoTransport');
    });

    it('chosen, is kept and says what it asks of the network', async () => {
        mount();
        const select = $('#settings-video-codec');
        select.value = 'pyrowave';
        select.dispatchEvent(new Event('change'));
        expect(view._videoCodec).toBe('pyrowave');
        expect($('#settings-video-codec').value).toBe('pyrowave');
        expect(view.container.innerHTML).toContain('text:settings.codecPyrowaveNote');
        await vi.waitFor(() =>
            expect(JSON.parse(localStorage.getItem('mw-streaming-settings')).video_codec).toBe(
                'pyrowave',
            ),
        );
    });

    it('never becomes the default of every other browser', async () => {
        // jsdom runs on localhost, and the host says this browser administers
        // it: the server's defaults are written too.
        BackendClient.getAuthStatus.mockResolvedValueOnce({ is_localhost: true });
        view._videoCodec = 'pyrowave';
        await view._saveToStorage();
        expect(JSON.parse(localStorage.getItem('mw-streaming-settings')).video_codec).toBe(
            'pyrowave',
        );
        expect(BackendClient.saveStreamingSettings).toHaveBeenCalledTimes(1);
        const sent = BackendClient.saveStreamingSettings.mock.calls[0][0];
        expect('video_codec' in sent).toBe(false);
    });

    it('chosen where it cannot run, shows the fallback and keeps the choice', async () => {
        view._videoCodec = 'pyrowave';
        view._pyrowaveHost = false;
        mount();
        expect($('#settings-video-codec').value).toBe('hevc');
        expect(view.container.innerHTML).toContain('text:settings.codecPyrowaveFallback');
        // Another setting saved meanwhile does not put the choice away.
        const fps = $('#settings-stream-fps');
        fps.dispatchEvent(new Event('change'));
        await vi.waitFor(() =>
            expect(
                JSON.parse(localStorage.getItem('mw-streaming-settings') || '{}').video_codec,
            ).toBe('pyrowave'),
        );
    });
});
