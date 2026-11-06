/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The administrator's ceilings (issue #53): what Settings offers and what a
 * launch asks for, the same way the server holds it (AppSettings::
 * limitStreamSize, backend/tests/test_app_settings.cpp).
 */
import { afterEach, describe, it, expect } from 'vitest';
import {
    bitrateSliderMax,
    capSizeToLimits,
    customSizeMax,
    hasStreamLimits,
    limitBitrateKbps,
    limitBitrateMbps,
    limitFps,
    resetStreamLimits,
    rungAllowed,
    setStreamLimits,
} from '../js/util/StreamLimits.js';
import { autoFps } from '../js/util/RefreshRate.js';

const limit = (fps, mbps, width, height) =>
    setStreamLimits({
        max_allowed_frame_rate: fps,
        max_allowed_bitrate: mbps,
        max_allowed_stream_width: width,
        max_allowed_stream_height: height,
    });

afterEach(() => resetStreamLimits());

describe('StreamLimits', () => {
    it('holds nothing until the server names a ceiling', () => {
        expect(hasStreamLimits()).toBe(false);
        expect(limitFps(240)).toBe(240);
        expect(limitBitrateMbps(150)).toBe(150);
        expect(bitrateSliderMax()).toBe(150);
        expect(rungAllowed(2160)).toBe(true);
        const size = { height: 2160, aspect: '16:9' };
        expect(capSizeToLimits(size)).toEqual({ size, capped: false });
        // A reply from an older server, without the keys.
        setStreamLimits({ stream_fps: 0 });
        expect(hasStreamLimits()).toBe(false);
    });

    it('holds the frame rate and the bitrate, Auto included', () => {
        limit(60, 40, 0, 0);
        expect(limitFps(120)).toBe(60);
        expect(limitFps(30)).toBe(30);
        expect(limitFps(0)).toBe(0);
        expect(autoFps(144000, false)).toBe(60);
        expect(limitBitrateMbps(80)).toBe(40);
        expect(limitBitrateKbps(80000)).toBe(40000);
        expect(bitrateSliderMax()).toBe(40);
    });

    it('offers the rungs inside the box, a custom pair up to it', () => {
        limit(0, 0, 1920, 1080);
        expect([720, 1080, 1440, 2160].filter(rungAllowed)).toEqual([720, 1080]);
        expect(customSizeMax(4096)).toEqual({ width: 1920, height: 1080 });
        limit(0, 0, 0, 1440);
        expect([720, 1080, 1440, 2160].filter(rungAllowed)).toEqual([720, 1080, 1440]);
        expect(customSizeMax(4096)).toEqual({ width: 4096, height: 1440 });
    });

    it('keeps the shape of an explicit size, clips a box', () => {
        limit(0, 0, 1920, 1080);
        expect(capSizeToLimits({ height: 1440, aspect: '2560:1440' }).size).toEqual({
            height: 1080,
            aspect: '1920:1080',
            fallback: undefined,
        });
        expect(capSizeToLimits({ height: 1080, aspect: '2560:1080' }).size.aspect).toBe('1920:810');
        // A rung without its shape yet: its height, read at 16:9.
        const rung = capSizeToLimits({ height: 2160 });
        expect(rung.capped).toBe(true);
        expect(rung.size.height).toBe(1080);
        expect(rung.size.aspect).toBeUndefined();
        // The native host's box, clipped side by side; "Match my screen" keeps
        // its shape and its fallback is clipped.
        expect(
            capSizeToLimits({ height: 1600, aspect: '2560:1600', fitBox: true }).size.aspect,
        ).toBe('1920:1080');
        const match = capSizeToLimits({
            height: 1600,
            aspect: '2560:1600',
            fitBox: true,
            matchDisplay: true,
            fallback: { width: 2560, height: 1440 },
        }).size;
        expect(match.aspect).toBe('1728:1080');
        expect(match.fallback).toEqual({ width: 1920, height: 1080 });
        // The host's own size is the server's to bound.
        expect(capSizeToLimits({ height: 0 }).capped).toBe(false);
        // Under the box: untouched.
        expect(capSizeToLimits({ height: 720, aspect: '1280:720' }).capped).toBe(false);
    });
});
