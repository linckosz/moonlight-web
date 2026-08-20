/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

/**
 * AspectProbe / AspectRatio — observable behaviour: given the edge pixels of a
 * frame, which aspect does the client ask the host for next?
 *
 * Sunshine pads its capture with black to fill the frame we requested, and that
 * padding is the only trace of the host's real screen format that reaches the
 * browser. These tests pin both halves of the verdict: the bar measurement, and
 * the white list that decides whether a measurement is a screen at all — the
 * guard that keeps a pillarboxed video from reshaping the stream.
 */
import { describe, it, expect } from 'vitest';
import { scanBars, decideAspect } from '../js/stream/AspectProbe.js';
import { resolveMeasuredAspect } from '../js/util/AspectRatio.js';

const N = 3; // probe columns / rows, as sampled by the real code

/**
 * Build the two edge strips the probe reads back: N columns (n×h) and N rows
 * (w×N) of a frame whose real picture sits inside the given bars.
 * `contentBlack` models a host screen that is genuinely black.
 */
function frame(w, h, { top = 0, bottom = 0, left = 0, right = 0, contentBlack = false } = {}) {
    const cols = new Uint8ClampedArray(N * h * 4);
    const rows = new Uint8ClampedArray(w * N * 4);
    const lit = contentBlack ? 0 : 128;
    for (let y = 0; y < h; y++) {
        const inPicture = y >= top && y < h - bottom;
        for (let i = 0; i < N; i++) {
            const p = (y * N + i) * 4;
            cols[p] = cols[p + 1] = cols[p + 2] = inPicture ? lit : 0;
            cols[p + 3] = 255;
        }
    }
    for (let x = 0; x < w; x++) {
        const inPicture = x >= left && x < w - right;
        for (let i = 0; i < N; i++) {
            const p = (i * w + x) * 4;
            rows[p] = rows[p + 1] = rows[p + 2] = inPicture ? lit : 0;
            rows[p + 3] = 255;
        }
    }
    return { cols, rows };
}

/** Full verdict path: synthetic frame → bars → aspect to request. */
function verdict(w, h, bars) {
    const { cols, rows } = frame(w, h, bars);
    return decideAspect(scanBars(cols, rows, w, h, N), w, h);
}

describe('AspectProbe — reading the host format off the padding', () => {
    it('reads a 16:10 host out of the 4:3 frame it was padded into', () => {
        // 1440×1080 requested, 1440×900 of picture → 90px top and bottom.
        expect(verdict(1440, 1080, { top: 90, bottom: 90 })).toEqual({
            aspect: '16:10',
            reason: 'measured',
        });
    });

    it('reads a 4:3 host out of a 16:9 frame (bars on the sides)', () => {
        expect(verdict(1920, 1080, { left: 240, right: 240 })).toEqual({
            aspect: '4:3',
            reason: 'measured',
        });
    });

    it('asks for nothing when the frame already matches the host', () => {
        expect(verdict(1920, 1080, {})).toEqual({ aspect: null, reason: 'no-bars' });
    });

    it('treats a black host screen as undecidable, never as all-padding', () => {
        expect(verdict(1920, 1080, { contentBlack: true })).toEqual({
            aspect: null,
            reason: 'uniform',
        });
    });

    it('rejects bars on both axes — a scaler only ever pads one', () => {
        expect(verdict(1920, 1080, { top: 60, bottom: 60, left: 100, right: 100 })).toEqual({
            aspect: null,
            reason: 'both-axes',
        });
    });

    it('rejects off-centre bars, which belong to the picture', () => {
        expect(verdict(1920, 1080, { top: 200, bottom: 40 })).toEqual({
            aspect: null,
            reason: 'asymmetric',
        });
    });

    it('ignores hairline edges (encoder ringing, under 2%)', () => {
        expect(verdict(1920, 1080, { top: 6, bottom: 6 })).toEqual({
            aspect: null,
            reason: 'no-bars',
        });
    });

    it('rejects a measurement that matches no real screen format', () => {
        // A square picture: 1:1 is not a monitor.
        expect(verdict(1920, 1080, { left: 420, right: 420 })).toEqual({
            aspect: null,
            reason: 'implausible',
        });
    });
});

describe('AspectRatio — the white list of real screen formats', () => {
    it('names the common formats', () => {
        expect(resolveMeasuredAspect(1920, 1080)).toBe('16:9');
        expect(resolveMeasuredAspect(1600, 1000)).toBe('16:10');
        expect(resolveMeasuredAspect(1440, 1080)).toBe('4:3');
        expect(resolveMeasuredAspect(1620, 1080)).toBe('3:2');
        expect(resolveMeasuredAspect(1350, 1080)).toBe('5:4');
        expect(resolveMeasuredAspect(3840, 1080)).toBe('32:9');
    });

    it('keeps the exact ratio when the nominal label would leave a bar', () => {
        // 3440×1440 is 2.389, the "21:9" label is 2.333 — rounding to the label
        // would put ~60px of black back on the sides, so the measurement wins.
        expect(resolveMeasuredAspect(3440, 1440)).toBe('3440:1440');
        expect(resolveMeasuredAspect(2560, 1080)).toBe('2560:1080');
    });

    it('refuses ratios that are no screen at all', () => {
        expect(resolveMeasuredAspect(1080, 1080)).toBeNull(); // square
        expect(resolveMeasuredAspect(1500, 1080)).toBeNull(); // between 4:3 and 3:2
        expect(resolveMeasuredAspect(0, 1080)).toBeNull();
        expect(resolveMeasuredAspect(1920, 0)).toBeNull();
    });
});
