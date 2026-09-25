/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';
import { chroma444Codec } from '../js/util/BrowserDetect.js';
import { isChroma444Profile } from '../js/util/Mp4Muxer.js';
import { StreamView } from '../js/ui/StreamView.js';

/**
 * 4:4:4 asked only in a codec the browser decodes it in.
 *
 * The bench of 25/09/2026 (Chrome, AMD iGPU): the probe answered "yes" because
 * ONE 4:4:4 profile passed, an HEVC 4:4:4 stream was asked, the decoder took a
 * Main string for it, failed every frame, and three errors later the viewer was
 * in H.264 4:2:0 — without a word.
 */

/** A VideoDecoder whose isConfigSupported says yes to the strings given. */
function stubDecoder(supported) {
    const asked = [];
    vi.stubGlobal('VideoDecoder', {
        isConfigSupported: async (cfg) => {
            asked.push(cfg);
            return { supported: supported.includes(cfg.codec), config: cfg };
        },
    });
    return asked;
}

describe('chroma444ClientCapability — per codec', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.resetModules();
    });

    it('tells HEVC 4:4:4 and H.264 4:4:4 apart', async () => {
        const asked = stubDecoder(['avc1.F4002A']);
        const m = await import('../js/util/BrowserDetect.js');
        expect(await m.chroma444ClientCapability()).toEqual({
            decode: true,
            hevc: false,
            h264: true,
        });
        // At the size the decoder is configured at: a level is a size.
        expect(asked.every((c) => c.codedWidth === 1920 && c.codedHeight === 1080)).toBe(true);
    });

    it('says no when neither decodes', async () => {
        stubDecoder([]);
        const m = await import('../js/util/BrowserDetect.js');
        expect(await m.chroma444ClientCapability()).toEqual({
            decode: false,
            hevc: false,
            h264: false,
        });
    });

    it('trusts a browser that cannot be asked', async () => {
        vi.stubGlobal('VideoDecoder', undefined);
        const m = await import('../js/util/BrowserDetect.js');
        expect((await m.chroma444ClientCapability()).hevc).toBe(true);
    });
});

describe('chroma444Codec — the codec a 4:4:4 launch asks for', () => {
    const only264 = { decode: true, hevc: false, h264: true };
    const onlyHevc = { decode: true, hevc: true, h264: false };
    const none = { decode: false, hevc: false, h264: false };

    it('keeps the chosen codec when it decodes 4:4:4', () => {
        expect(chroma444Codec('hevc', onlyHevc)).toBe('hevc');
        expect(chroma444Codec(undefined, onlyHevc)).toBe('hevc');
        expect(chroma444Codec('h264', only264)).toBe('h264');
    });

    it('asks H.264 4:4:4 rather than lose 4:4:4 to an HEVC it cannot decode', () => {
        expect(chroma444Codec('hevc', only264)).toBe('h264');
        expect(chroma444Codec(undefined, only264)).toBe('h264');
    });

    it('declines when the chosen codec cannot carry it here', () => {
        expect(chroma444Codec('h264', onlyHevc)).toBe(null);
        expect(chroma444Codec('hevc', none)).toBe(null);
        expect(chroma444Codec('av1', none)).toBe(null);
    });

    it('leaves AV1 to the host on a browser that decodes 4:4:4', () => {
        expect(chroma444Codec('av1', only264)).toBe('av1');
    });
});

describe('isChroma444Profile', () => {
    it('names HEVC RExt and H.264 High 4:4:4, nothing 4:2:0', () => {
        expect(isChroma444Profile('hvc1.4.10.L153.B0')).toBe(true);
        expect(isChroma444Profile('hev1.4.158.L123.B0')).toBe(true);
        expect(isChroma444Profile('avc1.F4002A')).toBe(true);
        expect(isChroma444Profile('avc1.f40028')).toBe(true);
        expect(isChroma444Profile('hvc1.1.6.L153.B0')).toBe(false);
        expect(isChroma444Profile('hvc1.2.4.L153.B0')).toBe(false);
        expect(isChroma444Profile('avc1.64002A')).toBe(false);
        expect(isChroma444Profile('')).toBe(false);
    });
});

describe('codec fallback from H.264 4:4:4', () => {
    const target = (self) => StreamView.prototype._computeCodecFallbackTarget.call(self);

    it('goes to H.264 4:2:0 instead of giving up', () => {
        expect(target({ videoCodec: 'h264', _hdrEnabled: false, _yuv444: true })).toEqual({
            codec: 'h264',
            hdr: false,
        });
    });

    it('still gives up on H.264 4:2:0', () => {
        expect(target({ videoCodec: 'h264', _hdrEnabled: false, _yuv444: false })).toBe(null);
    });
});
