/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * « Auto », the first codec of the Settings menu and its default (26/10/2026):
 * HEVC, or AV1 where HEVC cannot do the job. A stored HEVC, which cannot say
 * whether it was chosen or only left there, becomes Auto once per browser.
 */
import { describe, it, expect, beforeEach, vi } from 'vitest';

vi.mock('../js/stream/ultra/UltraPlayer.js', () => ({
    ultraPlayerSupported: () => false,
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
import {
    CODEC_AUTO,
    codecAfterAutoMigration,
    migrateStoredCodecToAuto,
    plainHttpLaunchCodec,
} from '../js/util/CodecAuto.js';

function memoryStore(init = {}) {
    const m = new Map(Object.entries(init));
    return {
        getItem: (k) => (m.has(k) ? m.get(k) : null),
        setItem: (k, v) => m.set(k, String(v)),
        map: m,
    };
}

describe('CodecAuto: the one move of HEVC to Auto', () => {
    it('turns a HEVC into Auto the first time, and keeps a HEVC chosen after', () => {
        const s = memoryStore();
        expect(codecAfterAutoMigration('hevc', s)).toBe(CODEC_AUTO);
        expect(codecAfterAutoMigration('hevc', s)).toBe('hevc');
    });

    it('leaves every other codec as it is, and counts it as the move', () => {
        for (const codec of ['h264', 'av1', 'pyrowave', 'auto']) {
            const s = memoryStore();
            expect(codecAfterAutoMigration(codec, s)).toBe(codec);
            expect(codecAfterAutoMigration('hevc', s)).toBe('hevc');
        }
    });

    it('waits for a codec: a browser with none stored moves the first one it gets', () => {
        const s = memoryStore();
        expect(codecAfterAutoMigration(undefined, s)).toBeUndefined();
        expect(codecAfterAutoMigration('', s)).toBe('');
        // the server's default, on a first visit to the Settings page
        expect(codecAfterAutoMigration('hevc', s)).toBe(CODEC_AUTO);
    });

    it('keeps the codec where the store can be neither read nor written', () => {
        const broken = {
            getItem() {
                throw new Error('denied');
            },
            setItem() {
                throw new Error('denied');
            },
        };
        expect(codecAfterAutoMigration('hevc', broken)).toBe('hevc');
        expect(codecAfterAutoMigration('hevc', null)).toBe('hevc');
    });

    it('rewrites the settings this browser stored, once', () => {
        const s = memoryStore({
            'mw-streaming-settings': JSON.stringify({ video_codec: 'hevc', hdr_enabled: true }),
        });
        migrateStoredCodecToAuto(s);
        expect(JSON.parse(s.getItem('mw-streaming-settings'))).toEqual({
            video_codec: 'auto',
            hdr_enabled: true,
        });
        // HEVC chosen again in the menu: it stays
        s.setItem('mw-streaming-settings', JSON.stringify({ video_codec: 'hevc' }));
        migrateStoredCodecToAuto(s);
        expect(JSON.parse(s.getItem('mw-streaming-settings')).video_codec).toBe('hevc');
    });

    it('does nothing without settings, and nothing to unreadable ones', () => {
        const none = memoryStore();
        migrateStoredCodecToAuto(none);
        expect(none.map.size).toBe(0);
        const bad = memoryStore({ 'mw-streaming-settings': 'not json' });
        expect(() => migrateStoredCodecToAuto(bad)).not.toThrow();
        expect(bad.getItem('mw-streaming-settings')).toBe('not json');
    });
});

describe('CodecAuto: a TV on plain HTTP asks H.264 (#42)', () => {
    it('asks H.264 for Auto', () => {
        expect(plainHttpLaunchCodec(CODEC_AUTO)).toBe('h264');
    });

    it('asks H.264 with no codec stored, the TV whose settings page is hidden', () => {
        expect(plainHttpLaunchCodec(undefined)).toBe('h264');
        expect(plainHttpLaunchCodec('')).toBe('h264');
    });

    it('keeps a codec stored by hand', () => {
        expect(plainHttpLaunchCodec('hevc')).toBe('hevc');
        expect(plainHttpLaunchCodec('av1')).toBe('av1');
        expect(plainHttpLaunchCodec('h264')).toBe('h264');
    });
});

describe('SettingsView: Auto in the codec menu', () => {
    let view;
    const $ = (sel) => view.container.querySelector(sel);

    beforeEach(() => {
        localStorage.clear();
        document.body.innerHTML = '<div id="settings"></div>';
        view = new SettingsView(document.getElementById('settings'), () => {});
        view._codecSupport = { h264: true, hevc: true, av1: true };
    });

    it('is the first codec and the default, under its own name', () => {
        view.render();
        const select = $('#settings-video-codec');
        expect(select.options[0].value).toBe('auto');
        expect(select.options[0].textContent).toBe('text:settings.codecAuto');
        expect(select.options[0].disabled).toBe(false);
        expect(select.value).toBe('auto');
    });

    it("is never greyed out by this browser's decoders", () => {
        view._codecSupport = { h264: true, hevc: false, av1: false };
        view.render();
        expect($('#settings-video-codec option[value="auto"]').disabled).toBe(false);
        expect($('#settings-video-codec').value).toBe('auto');
    });

    it('gives way to H.264 where the <video> transport is forced', () => {
        view._mediaTrackOnlyH264 = true;
        view.render();
        expect($('#settings-video-codec option[value="auto"]').disabled).toBe(true);
        expect($('#settings-video-codec').value).toBe('h264');
    });

    it("takes the server's HEVC as Auto on a first visit, and keeps a chosen codec", () => {
        view._applySettings({ video_codec: 'hevc' });
        expect(view._videoCodec).toBe('auto');
        view._applySettings({ video_codec: 'hevc' });
        expect(view._videoCodec).toBe('hevc');
        view._applySettings({ video_codec: 'av1' });
        expect(view._videoCodec).toBe('av1');
        view._applySettings({});
        expect(view._videoCodec).toBe('auto');
    });
});
