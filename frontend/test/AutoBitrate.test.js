/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The recommended bitrate: 9.5 Mbps at 1080p, growing with the pixels to the
 * power 0.75 (docs/wiki/16-Auto-Bitrate-Benchmarks.md) — 20 Mbps and the power
 * 0.8 on the host's own network — rounded up to the
 * slider's whole Mbps, ×1.2 for HDR — and nothing else: neither the frame
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
        expect(computeAutoBitrate(2160, '16:9', false, true)).toBe(35); // 32.2
        expect(computeAutoBitrate(2160, '21:9', false, false)).toBe(35); // 32.9
    });

    it('adds a fifth for HDR, nothing for 4:4:4', () => {
        expect(computeAutoBitrate(1080, '16:9', false, true)).toBe(12); // 11.4
        expect(computeAutoBitrate(1080, '16:9', true, false)).toBe(10);
        expect(computeAutoBitrate(1080, '16:9', true, true)).toBe(12);
        expect(computeAutoBitrate(720, '16:9', false, true)).toBe(7); // 6.2
        expect(computeAutoBitrate(1440, '16:9', false, true)).toBe(18); // 17.6
        expect(computeAutoBitrate(2160, '16:9', true, false)).toBe(27);
    });

    it('takes 20 × P^0.8 on the LAN, HDR adding its fifth there too', () => {
        expect(computeAutoBitrate(1080, '16:9', false, false, true)).toBe(20);
        expect(computeAutoBitrate(720, '16:9', false, false, true)).toBe(11); // 10.45
        expect(computeAutoBitrate(1440, '16:9', false, false, true)).toBe(35); // 31.7
        expect(computeAutoBitrate(2160, '16:9', false, false, true)).toBe(65); // 60.6
        expect(computeAutoBitrate(1080, '16:9', false, true, true)).toBe(24);
        expect(computeAutoBitrate(1080, '16:9', true, false, true)).toBe(20);
        expect(computeAutoBitrate(1080, '16:9', false, false, false)).toBe(10);
    });

    it('never leaves [1, 150] Mbps', () => {
        expect(computeAutoBitrate(144, '16:9', false, false)).toBe(1);
        expect(computeAutoBitrate(4320, '32:9', true, true)).toBe(150);
        expect(computeAutoBitrate(2160, '16:9', false, true, true)).toBe(75); // 72.8
        expect(computeAutoBitrate(4320, '16:9', false, true, true)).toBe(150);
    });
});
