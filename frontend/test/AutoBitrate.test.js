/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The recommended bitrate: 9.5 Mbps at 1080p, growing with the pixels to the
 * power 0.75 (docs/wiki/16-Auto-Bitrate-Benchmarks.md), rounded up to the
 * slider's whole Mbps, ×1.5 for HDR — and nothing else: neither the frame
 * rate, the device nor 4:4:4 moves it.
 */
import { describe, it, expect } from 'vitest';
import { aspectToNumber, computeAutoBitrate } from '../js/util/AutoBitrate.js';

describe('aspectToNumber', () => {
    it('reads "W:H" and falls back to 16:9', () => {
        expect(aspectToNumber('2560:1080')).toBeCloseTo(2560 / 1080);
        expect(aspectToNumber('auto')).toBeCloseTo(16 / 9);
        expect(aspectToNumber('16:0')).toBeCloseTo(16 / 9);
        expect(aspectToNumber(undefined)).toBeCloseTo(16 / 9);
    });
});

describe('computeAutoBitrate', () => {
    it('is 10 Mbps at 1080p (9.5 rounded up) and grows slower than the pixels', () => {
        expect(computeAutoBitrate(1080, '16:9', false, false)).toBe(10);
        expect(computeAutoBitrate(0, '16:9', false, false)).toBe(10);
        expect(computeAutoBitrate(720, '16:9', false, false)).toBe(6);
        expect(computeAutoBitrate(1440, '16:9', false, false)).toBe(15);
        expect(computeAutoBitrate(2160, '16:9', false, false)).toBe(27);
    });

    it('counts the pixels of a wider frame', () => {
        expect(computeAutoBitrate(1080, '2560:1080', false, false)).toBe(12);
        expect(computeAutoBitrate(1440, '3440:1440', false, false)).toBe(19);
    });

    it('rounds to 5 Mbps above 30', () => {
        expect(computeAutoBitrate(2160, '16:9', false, true)).toBe(45); // 40.3
        expect(computeAutoBitrate(2160, '21:9', false, false)).toBe(35); // 32.9
    });

    it('adds half for HDR, nothing for 4:4:4', () => {
        expect(computeAutoBitrate(1080, '16:9', false, true)).toBe(15);
        expect(computeAutoBitrate(1080, '16:9', true, false)).toBe(10);
        expect(computeAutoBitrate(1080, '16:9', true, true)).toBe(15);
        expect(computeAutoBitrate(2160, '16:9', true, false)).toBe(27);
    });

    it('never leaves [1, 150] Mbps', () => {
        expect(computeAutoBitrate(144, '16:9', false, false)).toBe(1);
        expect(computeAutoBitrate(4320, '32:9', true, true)).toBe(150);
    });
});
