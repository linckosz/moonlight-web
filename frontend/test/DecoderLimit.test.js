/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */
import { describe, it, expect, afterEach, vi } from 'vitest';
import {
    capSizeToDecoder,
    decoderRefuses,
    hardwareDecodeLimit,
    knownHardwareDecodeLimit,
    resetHardwareDecodeLimits,
} from '../js/util/DecoderLimit.js';

const TV = { width: 1920, height: 1080 };

describe('DecoderLimit.capSizeToDecoder', () => {
    it('leaves a size the decoder takes alone', () => {
        const size = { height: 720, aspect: '1280:720', fitBox: true };
        expect(capSizeToDecoder(size, TV)).toEqual({ size, capped: false });
        expect(capSizeToDecoder(size, null).capped).toBe(false);
    });

    it("clamps a phone's Auto box side by side (the Mi TV case)", () => {
        // 4096:1440 is the handheld box; the host fits 16:9 inside it.
        const { size, capped } = capSizeToDecoder(
            { height: 1440, aspect: '4096:1440', fitBox: true, allowUpscale: false },
            TV,
        );
        expect(capped).toBe(true);
        expect(size.height).toBe(1080);
        expect(size.aspect).toBe('1920:1080');
        expect(size.fitBox).toBe(true);
    });

    it('scales an exact size as a whole, its shape kept, and its fallback box clamped', () => {
        const { size } = capSizeToDecoder(
            {
                height: 1080,
                aspect: '2560:1080',
                fitBox: true,
                matchDisplay: true,
                fallback: { width: 2560, height: 1080 },
            },
            TV,
        );
        expect(size.aspect).toBe('1920:810');
        expect(size.height).toBe(810);
        expect(size.matchDisplay).toBe(true);
        expect(size.fallback).toEqual({ width: 1920, height: 1080 });
    });

    it('brings a rung down to the height whose 16:9 width fits', () => {
        expect(capSizeToDecoder({ height: 1440, aspect: null }, TV).size.height).toBe(1080);
        expect(
            capSizeToDecoder({ height: 2160, aspect: null }, { width: 1280, height: 720 }).size
                .height,
        ).toBe(720);
        expect(capSizeToDecoder({ height: 1080, aspect: null }, TV).capped).toBe(false);
    });

    it("turns the host's own size into the limit as a box", () => {
        const { size } = capSizeToDecoder({ height: 0, aspect: null, fitBox: false }, TV);
        expect(size).toMatchObject({
            height: 1080,
            aspect: '1920:1080',
            fitBox: true,
            allowUpscale: false,
        });
    });
});

describe('DecoderLimit.hardwareDecodeLimit', () => {
    afterEach(() => {
        vi.unstubAllGlobals();
        resetHardwareDecodeLimits();
    });

    const decoderUpTo = (maxW, maxH, calls) => ({
        isConfigSupported: async (cfg) => {
            if (calls) calls.push(cfg);
            return {
                supported:
                    cfg.hardwareAcceleration === 'prefer-hardware' &&
                    cfg.codedWidth <= maxW &&
                    cfg.codedHeight <= maxH,
            };
        },
    });

    it('finds the largest size the hardware takes, asked with prefer-hardware', async () => {
        const calls = [];
        vi.stubGlobal('VideoDecoder', decoderUpTo(1920, 1088, calls));
        expect(await hardwareDecodeLimit('h264')).toEqual({ width: 1920, height: 1080 });
        expect(calls.every((c) => c.hardwareAcceleration === 'prefer-hardware')).toBe(true);
        expect(knownHardwareDecodeLimit('h264')).toEqual({ width: 1920, height: 1080 });
    });

    it('answers no limit when 4K decodes, and none when nothing does', async () => {
        vi.stubGlobal('VideoDecoder', decoderUpTo(4096, 2304));
        expect(await hardwareDecodeLimit('hevc')).toBeNull();
        resetHardwareDecodeLimits();
        vi.stubGlobal('VideoDecoder', decoderUpTo(0, 0));
        expect(await hardwareDecodeLimit('hevc')).toBeNull();
    });

    it('asks once per codec', async () => {
        const calls = [];
        vi.stubGlobal('VideoDecoder', decoderUpTo(1920, 1088, calls));
        await hardwareDecodeLimit('av1');
        const n = calls.length;
        await hardwareDecodeLimit('av1');
        expect(calls.length).toBe(n);
    });

    it('stays out of the way without WebCodecs', async () => {
        vi.stubGlobal('VideoDecoder', undefined);
        expect(await hardwareDecodeLimit('h264')).toBeNull();
    });
});

describe('DecoderLimit.decoderRefuses', () => {
    const stream = { width: 3840, height: 2160, fps: 120, bitrate: 150e6 };
    const answering = (info) => ({ decodingInfo: async () => info });

    it('hears an outright no', async () => {
        const no = [{ supported: false, smooth: false }, { supported: true, smooth: false }];
        for (const info of no)
            expect(await decoderRefuses('hevc', stream, 300, answering(info))).toBe(true);
    });

    it('takes a yes for no answer at all', async () => {
        const yes = answering({ supported: true, smooth: true, powerEfficient: false });
        expect(await decoderRefuses('hevc', stream, 300, yes)).toBe(false);
    });

    it('says what it asks about', async () => {
        const decodingInfo = vi.fn(async () => ({ supported: true, smooth: true }));
        await decoderRefuses('h264', stream, 300, { decodingInfo });
        expect(decodingInfo.mock.calls[0][0].type).toBe('media-source');
        expect(decodingInfo.mock.calls[0][0].video).toMatchObject({
            width: 3840,
            height: 2160,
            framerate: 120,
            bitrate: 150e6,
        });
    });

    it('asks about HEVC when no codec is stored', async () => {
        const decodingInfo = vi.fn(async () => ({ supported: false, smooth: false }));
        expect(await decoderRefuses(undefined, stream, 300, { decodingInfo })).toBe(true);
        expect(decodingInfo.mock.calls[0][0].video.contentType).toContain('hev1');
    });

    it('takes a missing API, an error or an unknown codec for no no', async () => {
        expect(await decoderRefuses('hevc', stream, 300, {})).toBe(false);
        const broken = {
            decodingInfo: async () => {
                throw new TypeError('bad config');
            },
        };
        expect(await decoderRefuses('hevc', stream, 300, broken)).toBe(false);
        const no = answering({ supported: false, smooth: false });
        expect(await decoderRefuses('pyrowave', stream, 300, no)).toBe(false);
    });

    it('does not wait past its time for an answer', async () => {
        const never = { decodingInfo: () => new Promise(() => {}) };
        expect(await decoderRefuses('hevc', stream, 5, never)).toBe(false);
    });
});
