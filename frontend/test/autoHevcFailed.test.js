/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { MoonlightApp } from '../js/app.js';
import { StreamView } from '../js/ui/StreamView.js';
import { Toast } from '../js/ui/Toast.js';
import {
    HEVC_FAILED_DAYS,
    SCM_MASK_AV1,
    autoLaunchCodec,
    av1FailedHere,
    hevcFailedHere,
    noteAv1Failed,
    noteHevcFailed,
} from '../js/util/CodecAuto.js';

/**
 * Firefox on the UM790Pro (28/10/2026): « Auto » asked the host for HEVC, the
 * browser could not decode it, and 0.8 s later the stream came back in H.264,
 * at every launch, each time with an « HDR unavailable » toast although no HDR
 * had been asked. Now the browser keeps the verdict and Auto asks AV1 there
 * from the next launch on (Bruno, 07/11/2026), H.264 where AV1 failed too or
 * the host has none; and HDR is said lost only where it was asked. AV1 only
 * where the hardware decodes it (autoAv1Hardware.test.js): these are run with
 * a browser that says it does.
 */

/** What the browser answered about its AV1 decoder. */
const HW = { hardware: true };

/** Sunshine on an RTX: H.264, HEVC Main/Main10, AV1 Main8/Main10. */
const HOST_AV1 = 0x1 | 0x100 | 0x200 | 0x10000 | 0x20000;
const HOST_NO_AV1 = 0x1 | 0x100 | 0x200;

const DAY = 24 * 3600 * 1000;

function memoryStore(init = {}) {
    const m = new Map(Object.entries(init));
    return {
        getItem: (k) => (m.has(k) ? m.get(k) : null),
        setItem: (k, v) => m.set(k, String(v)),
        map: m,
    };
}

describe('CodecAuto: the verdict on a browser whose HEVC failed', () => {
    const FX = 'Mozilla/5.0 (Windows NT 10.0; Win64; x64; rv:144.0) Gecko/20100101 Firefox/144.0';

    it('asks AV1 once HEVC failed here, HEVC before', () => {
        const s = memoryStore();
        const now = 1000 * DAY;
        expect(autoLaunchCodec(HOST_AV1, FX, s, now)).toBe('hevc');
        noteHevcFailed(FX, s, now);
        expect(hevcFailedHere(FX, s, now + DAY)).toBe(true);
        expect(autoLaunchCodec(HOST_AV1, FX, s, now + DAY, HW)).toBe('av1');
    });

    it('asks H.264 of a host without AV1, or once AV1 failed here too', () => {
        const s = memoryStore();
        const now = 1000 * DAY;
        noteHevcFailed(FX, s, now);
        expect(autoLaunchCodec(HOST_NO_AV1, FX, s, now, HW)).toBe('h264');
        expect(autoLaunchCodec(undefined, FX, s, now, HW)).toBe('h264');
        expect(autoLaunchCodec(0x20000, FX, s, now, HW)).toBe('av1');
        noteAv1Failed(FX, s, now);
        expect(av1FailedHere(FX, s, now)).toBe(true);
        expect(autoLaunchCodec(HOST_AV1, FX, s, now, HW)).toBe('h264');
        // The AV1 verdict alone changes nothing: HEVC first, as long as it works.
        const t = memoryStore();
        noteAv1Failed(FX, t, now);
        expect(autoLaunchCodec(HOST_AV1, FX, t, now)).toBe('hevc');
        expect(SCM_MASK_AV1 & HOST_NO_AV1).toBe(0);
    });

    it('lets the AV1 verdict lapse like the HEVC one', () => {
        const s = memoryStore();
        const now = 1000 * DAY;
        noteAv1Failed(FX, s, now);
        expect(av1FailedHere(FX, s, now + HEVC_FAILED_DAYS * DAY)).toBe(false);
        expect(av1FailedHere(FX, s, now - DAY)).toBe(false);
        expect(hevcFailedHere(FX, s, now)).toBe(false);
    });

    it('asks a new version of the browser again', () => {
        const s = memoryStore();
        noteHevcFailed(FX, s, 0);
        expect(hevcFailedHere(FX.replace(/144\.0/g, '145.0'), s, DAY)).toBe(false);
    });

    it('holds for a while only, and not for a clock set back', () => {
        const s = memoryStore();
        const now = 1000 * DAY;
        noteHevcFailed(FX, s, now);
        expect(hevcFailedHere(FX, s, now + (HEVC_FAILED_DAYS - 1) * DAY)).toBe(true);
        expect(hevcFailedHere(FX, s, now + HEVC_FAILED_DAYS * DAY)).toBe(false);
        expect(hevcFailedHere(FX, s, now - DAY)).toBe(false);
    });

    it('drops the verdicts past their time when it writes a new one', () => {
        const s = memoryStore();
        noteHevcFailed('old', s, 0);
        noteHevcFailed(FX, s, (HEVC_FAILED_DAYS + 1) * DAY);
        expect(Object.keys(JSON.parse(s.getItem('mw_hevc_failed')))).toEqual([FX]);
    });

    it('asks HEVC where the store can be neither read nor written', () => {
        const broken = {
            getItem() {
                throw new Error('denied');
            },
            setItem() {
                throw new Error('denied');
            },
        };
        expect(() => noteHevcFailed(FX, broken, 0)).not.toThrow();
        expect(() => noteAv1Failed(FX, broken, 0)).not.toThrow();
        expect(autoLaunchCodec(HOST_AV1, FX, broken, 0)).toBe('hevc');
        expect(autoLaunchCodec(HOST_AV1, FX, null, 0)).toBe('hevc');
        expect(hevcFailedHere(FX, memoryStore({ mw_hevc_failed: 'not json' }), 0)).toBe(false);
    });
});

describe('StreamView: the fallback keeps the verdict', () => {
    const P = StreamView.prototype;
    function view(over = {}) {
        return {
            videoCodec: 'hevc',
            _hdrEnabled: false,
            _yuv444: false,
            _quitting: false,
            _manualQuitting: false,
            _codecFallbackRequested: false,
            _codecFallback: null,
            webrtc: null,
            quit: vi.fn(),
            _computeCodecFallbackTarget: P._computeCodecFallbackTarget,
            _requestCodecFallback: P._requestCodecFallback,
            ...over,
        };
    }
    const clear = () => {
        localStorage.removeItem('mw_hevc_failed');
        localStorage.removeItem('mw_av1_failed');
    };
    beforeEach(clear);
    afterEach(clear);

    it('an 8-bit HEVC that fails, Auto or chosen: H.264, and the verdict', () => {
        for (const videoCodec of ['hevc', 'auto']) {
            localStorage.removeItem('mw_hevc_failed');
            const v = view({ videoCodec });
            v._requestCodecFallback();
            expect(v._codecFallback).toEqual({ codec: 'h264', hdr: false });
            expect(v.quit).toHaveBeenCalledOnce();
            expect(hevcFailedHere()).toBe(true);
        }
    });

    it('no verdict for an HDR or a 4:4:4 HEVC, nor for another codec', () => {
        for (const over of [
            { _hdrEnabled: true },
            { _yuv444: true },
            { videoCodec: 'av1' },
            { videoCodec: 'h264', _yuv444: true },
        ]) {
            const v = view(over);
            v._requestCodecFallback();
            expect(v._codecFallbackRequested, JSON.stringify(over)).toBe(true);
            expect(hevcFailedHere(), JSON.stringify(over)).toBe(false);
        }
    });

    it('an 8-bit AV1 that fails: H.264, and the AV1 verdict, not the HEVC one', () => {
        const v = view({ videoCodec: 'av1' });
        v._requestCodecFallback();
        expect(v._codecFallback).toEqual({ codec: 'h264', hdr: false });
        expect(av1FailedHere()).toBe(true);
        expect(hevcFailedHere()).toBe(false);
    });

    it('no AV1 verdict for an HDR AV1, nor for HEVC', () => {
        for (const over of [{ videoCodec: 'av1', _hdrEnabled: true }, { videoCodec: 'hevc' }]) {
            clear();
            const v = view(over);
            v._requestCodecFallback();
            expect(av1FailedHere(), JSON.stringify(over)).toBe(false);
        }
    });
});

describe('MoonlightApp: an Auto launch, and the fallback it may end in', () => {
    beforeEach(() => {
        localStorage.removeItem('mw_hevc_failed');
        localStorage.removeItem('mw-streaming-settings');
    });
    afterEach(() => {
        vi.restoreAllMocks();
        localStorage.removeItem('mw_hevc_failed');
        localStorage.removeItem('mw-streaming-settings');
    });

    function quitWithFallback(target, hdr) {
        const launch = vi.spyOn(MoonlightApp, 'launchApp').mockResolvedValue(undefined);
        vi.spyOn(MoonlightApp, 'transition').mockImplementation(() => {});
        vi.spyOn(MoonlightApp, '_showRelaunchLoader').mockImplementation(() => {});
        const persist = vi.spyOn(MoonlightApp, '_persistHdrDisabled').mockImplementation(() => {});
        const warn = vi.spyOn(Toast, 'warning').mockImplementation(() => {});
        MoonlightApp._nav.overlay = 'streaming';
        MoonlightApp._fallbackAttemptCount = 0;
        MoonlightApp._lastStreamHost = { backendType: 'sunshine' };
        MoonlightApp._lastStreamApp = { id: 1 };
        MoonlightApp.streamView = {
            _codecFallbackRequested: true,
            _codecFallback: target,
            _hdrEnabled: hdr,
        };
        MoonlightApp._onStreamingQuit();
        return { launch, persist, warn };
    }

    it('says nothing of HDR when an SDR HEVC falls back to H.264', () => {
        const { launch, persist, warn } = quitWithFallback({ codec: 'h264', hdr: false }, false);
        expect(launch).toHaveBeenCalledWith(expect.anything(), expect.anything(), 'h264', false);
        expect(persist).not.toHaveBeenCalled();
        expect(warn).not.toHaveBeenCalledWith(expect.stringContaining('hdrFallback'));
        expect(warn.mock.calls.some((c) => /HDR/i.test(String(c[0])))).toBe(false);
    });

    it('still says it, and puts the box away, when an HDR stream loses HDR', () => {
        const { persist, warn } = quitWithFallback({ codec: 'hevc', hdr: false }, true);
        expect(persist).toHaveBeenCalledOnce();
        expect(warn).toHaveBeenCalledOnce();
    });
});
