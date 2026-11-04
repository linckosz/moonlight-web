/*
 * MWSR — MoonlightWeb Super Resolution, the upscaler of MoonlightWeb.
 * Copyright (c) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * SPDX-License-Identifier: MIT
 *
 * This file is under the MIT License, unlike the rest of MoonlightWeb (GNU
 * GPL v3 or later), so that MWSR may be taken into any program; its shaders,
 * generated from it, and their guide are in the repository's mwsr/ folder.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */

/**
 * MwsrShader — the texts of `mwsr`, MoonlightWeb's own upscaler
 * (docs/design/upscaler-mwsr.md, §3 and §4): one WGSL source per mode,
 * specialised by `override` constants, the input feed substituted in its
 * preamble; and the GLSL ES 3.00 twin of each (mwsrGlsl, plan §3.6), written
 * from the same tables of constants so that the two texts cannot drift
 * apart, which the laboratory's conformity flow holds to 2/255 (C11). An
 * HLSL twin (mwsrHlsl) serves the native renderers of other programs,
 * Direct3D 11 and 12, from the same tables (the mwsr/ folder).
 *
 * Two modes (plan §4.3), chosen per frame by mwsrRegime():
 *
 * `up` — the kernel, per output pixel:
 *   1. one bilinear read at the position that gives a cubic's middle pair (the
 *      texels around the pixel, by weights that sharpen with kc) and its luma;
 *   2. the vote: the screen derivatives of that luma, scaled to luma per input
 *      texel, decide per 2×2 quad whether the pixel sits on an edge; a flat
 *      quad keeps that one read;
 *   3. on an edge, four more reads, the cubic's outer lobes, complete the
 *      cubic K; two more, laid across the edge the derivatives measure, give
 *      the detail D = K - their mean, which sharpens by kg, and its colour by
 *      kwc: seven reads in all;
 *   4. the result stays within the range of the middle read and the pair
 *      across the edge, widened by kl of that range: a little overshoot, on
 *      purpose (Bruno's eye chose it over a hard bound), and no colour below
 *      zero on a float target. On an HDR feed kl is 0 (mwsrUniforms' `hdr`):
 *      the margin would make highlights.
 *   With CHROMA444 (4:4:4 streams, plan §4.2) the vote and the edge's direction
 *   are taken from the channel with the steepest ramp, so that a border between
 *   two colours of one luma is seen and sharpened too.
 *
 * `down` — the surface filter: a k×k grid of bilinear samples spread over the
 *   output pixel's footprint in the input (k by the scale, the grid's span
 *   R_DOWN with it), averaged in linear light: the decisive term, +2.5 to
 *   +12 dB against the host's Lanczos in the lab (P3). The light is best made
 *   by the hardware: read through an `-srgb` view and drawn into one, the
 *   sampler decodes before it filters and the target encodes on write
 *   (LINEAR_LIGHT 0: free, exact). Then one bilinear sample is the whole
 *   filter between ×0.5 and 1:1 (k = 1: the exact box at ×0.5; at ×0.75, +4 dB
 *   and half the FLIP of the gamma-space bilinear, at its cost, J15), and the
 *   3×3 and 4×4 grids serve below ×0.5 with five times less aliasing energy
 *   than in software light (J15). Without the views (an rgba16float feed)
 *   LINEAR_LIGHT 1 decodes each sample by a square (2: exact sRGB, three times
 *   the cost on the AMD iGPU) and the 2×2 grid serves between ×0.5 and 1:1
 *   (J12).
 *   From ×0.5 to 1:1 a light sharpening follows, which Bruno's eye took on 11
 *   pictures of 11 (MWSR_DOWN_LAW): coarse derivatives of the read's luma vote,
 *   a flat quad keeps the read; elsewhere a cross of 4 reads, one output pixel
 *   away, gives the detail (the read against their mean) and the bound (their
 *   range, widened by a margin), in linear light, the gain and the margin
 *   falling on a strong edge by that range. Below ×0.5, and on an HDR feed
 *   (the gain is 0: mwsrUniforms' `hdr`), no sharpening.
 *
 * HDR (plan §4.1): the renderer's `linear` feed is sRGB-encoded past 1.0
 * (1.0 = reference white); both modes are per channel, bounded by their own
 * samples and clamp nothing to 1.0, so they run on it, the kernel without its
 * margin (with it, 1000-nit text reached about 1400 nits and the colour error
 * on text passed the plain copy's; without it every check of the lab's
 * hdr_check.py holds, 03/11/2026; not yet seen by Bruno's eye). For a
 * reduction, though, the renderer's HDR pass 0 writes the linear light itself
 * (LINEAR_LIGHT 3): the sampler then mixes linear light, as through the sRGB
 * views of SDR, and the grids are those views' (Bruno, 26/10/2026: a filter
 * mixing the encoded values took fine contrast down by 8 to 28 %, journal J45).
 *
 * Written from scratch (plan §10.2). Ideas taken, and credited: bilinear taps
 * instead of gathers (Atyuwen's mobile EASU, Jimenez, MJP); a cubic read as
 * bilinear taps (Sigg and Hadwiger, GPU Gems 2); the early out on an edge vote
 * (SGSR1, NEDI); the local min/max bound against ringing (EASU, SGSR1);
 * sharpening held by the local range (CAS, RCAS). The vote by screen
 * derivatives and the taps laid across the measured edge are this project's.
 * No line, polynomial or constant of FSR1, SGSR1, NIS or the gist is
 * reproduced; the constants come from the laboratory
 * (scripts/bench/scaler-lab/journal.md), the test file checks the forbidden
 * ones never creep in.
 */

/**
 * The pipeline-overridable constants, with their type and the default the
 * WGSL text declares. A renderer sets them at pipeline creation
 * (`fragment.constants`), the laboratory per cell (`mwsr:dering=0;...`).
 * Every text declares them all; each mode reads its own.
 */
export const MWSR_OVERRIDE_SPEC = Object.freeze({
    /** 1: paint the vote (white = edge quad, black = flat) instead of the picture. */
    SHOW: ['u32', 0],
    /** 1: the vote and the edge's direction from the channel with the steepest ramp (4:4:4 streams); 0: from the luma. */
    CHROMA444: ['u32', 0],
    /** 1: the gaze zone (plan §15.2): the vote threshold and the sharpness slide from the law's values to MWSR_ZONE's at the centre of the picture, by the pixel's place and the stream's motion (the uniform `gaze`); 0: the text has no zone. */
    GAZE: ['u32', 0],
    /** Reduction: the span of the tap grid, as a fraction of the pixel's footprint in the input (mwsrRegime() sets it by the grid). */
    R_DOWN: ['f32', 1.0],
    /** Reduction: 0 average the encoded values; 1 in gamma-2 light (square, square root); 2 in sRGB linear light (≤ 0.35 dB better, 15 pow per pixel); 3 the feed is linear light already (the HDR pass 0 wrote it), averaged as it comes and encoded by the sRGB transfer once, at the output. */
    LINEAR_LIGHT: ['u32', 1],
});

/** The defaults alone, name → value. */
export const MWSR_OVERRIDES = Object.freeze(
    Object.fromEntries(Object.entries(MWSR_OVERRIDE_SPEC).map(([k, [, v]]) => [k, v])),
);

/**
 * The constants the product lays over the defaults by device class, under
 * the laboratory's own overrides. None: one kernel on every device (Bruno,
 * 03/11/2026). The renderers call it once, with BrowserDetect's IS_HANDHELD.
 * @param {{handheld: boolean}} device a phone or a tablet, not a TV
 * @returns {Object<string, number>} name → value
 */
export function mwsrClassOverrides(device) {
    return {};
}

/** Constants shared by the WGSL text and, later, its GLSL twin. */
export const MWSR_CONSTANTS = Object.freeze({
    /** Luma weights: Rec. 709 rounded to powers of two, two multiply-adds. */
    LUMA: [0.25, 0.625, 0.125],
    /** The scale (output / input) from which the kernel's sharpening is whole; none at 1:1. The law's first measured scale. */
    S_FULL: 1.07,
});

// mwsr-law: the coefficients, written by the lab's fit (begin)
const MWSR_LAW_CURVES = {
    kc: [0.6353583202388096, 0.021845681633595574, 0.12781268100437676],
    kg: [-0.1288413863403059, 2.5844683855070816, -1.260092021482419],
    kr: [1.0071546009267338, -1.0209066704914191, 0.5699605831985769],
    kl: [0.33416994543071277, -0.2962022126856069, 0.08628272130287401],
};
// mwsr-law: the coefficients, written by the lab's fit (end)

// What the native host's zones count their drops against: the sharpness they
// were tuned under, a curve in ln(s) from ×1.15 to ×3 (the `map` text's zref).
const MWSR_ZONE_REF = [-0.2384, 10.3176, -26.8143, 28.681, -10.6546];

/**
 * The law of the kernel's per-draw values (Bruno, 03/11/2026), a curve over
 * the size of the stream on the size of the surface it is drawn on,
 *
 *     value = c0 + c1 x + c2 x²     x = ln(s)     s: the scale, output / input, the smaller axis
 *
 * fitted on the lab's searches, from ×1.07 to ×4, and measured back: LPIPS first,
 * then VMAF NEG, on a cubic's kc, the sharpness kg, the across reads' distance
 * kr and the bound's margin kl. Bruno's eye then set the sharpness (kg and kl)
 * above that: the force, 1 up to ×1.33, 1.35 from ×2 (two comparisons of six
 * pictures each), a straight line in ln(s) between them, which his eye took at
 * ×1.5 on six pictures of six. Beyond ×2 it stays there: nothing higher was seen.
 * Outside the measured range, s is held at its ends.
 */
export const MWSR_LAW = Object.freeze({
    /** The scales measured: the law holds its ends beyond. */
    scale: Object.freeze([1.07, 4]),
    /** The force of the sharpness, from the scale `from` (force 1) to `to` (force `top`). */
    force: Object.freeze({ from: 4 / 3, to: 2, top: 1.35 }),
    /** The vote's threshold, in luma per input texel, and the colour's sharpness, at every scale. */
    ft: 0.005,
    kwc: 0.1,
    curves: Object.freeze(MWSR_LAW_CURVES),
});

/**
 * The law's values for an input of inW×inH drawn at outW×outH.
 * @param {number} inW
 * @param {number} inH
 * @param {number} outW
 * @param {number} outH
 * @returns {{kc: number, ft: number, kg: number, kr: number, kwc: number, kl: number}}
 */
export function mwsrLaw(inW, inH, outW, outH) {
    const [lo, hi] = MWSR_LAW.scale;
    const s = Math.min(Math.max(Math.min(outW / inW, outH / inH), lo), hi);
    const x = Math.log(s);
    const value = ([c0, c1, c2]) => c0 + x * (c1 + x * c2);
    const { from, to, top } = MWSR_LAW.force;
    const force =
        1 + (top - 1) * Math.min(Math.max(Math.log(s / from) / Math.log(to / from), 0), 1);
    const { kc, kg, kr, kl } = MWSR_LAW.curves;
    return {
        kc: value(kc),
        ft: MWSR_LAW.ft,
        kg: value(kg) * force,
        kr: value(kr),
        kwc: MWSR_LAW.kwc,
        kl: value(kl) * force,
    };
}

/**
 * The reduction's sharpening (Bruno, 03/11/2026): the setting his eye took on
 * 11 pictures of 11, at ×0.75, ×2/3 and ×0.5, over a sharper one, a stronger
 * one and FSR1. A straight line in s joins ×0.5 to ×2/3, held above; the gain
 * falls to none between `fade` and 1:1, which the renderer copies. His eye saw
 * those three scales through the sRGB views, not the values in between.
 *
 *     t = smoothstep(range[0], range[1], the cross's range of square-root luma)
 *     R = c + mix(gain, edgeGain, t) (c - the cross's mean)
 *     R is held within the range of the read and the cross, widened by mix(margin, edgeMargin, t) of it
 *
 * Each pair: at ×0.5, then at ×2/3 and above.
 */
export const MWSR_DOWN_LAW = Object.freeze({
    /** The scales of each pair; below the first, no sharpening. */
    scale: Object.freeze([0.5, 2 / 3]),
    gain: Object.freeze([0.25, 0.45]),
    edgeGain: Object.freeze([0.05, 0.2]),
    margin: Object.freeze([0.1, 0.25]),
    edgeMargin: Object.freeze([0, 0.1]),
    /** From this scale to 1:1, both gains fall to none. */
    fade: 0.93,
    /** The vote's threshold, in square-root luma per output pixel. */
    ft: 0.003,
    /** Where a local range of square-root luma is a strong edge: from none to whole. */
    range: Object.freeze([0.2, 0.5]),
});

/**
 * The reduction's values for an input of inW×inH drawn at outW×outH, under the
 * names of the uniform block's places that hold them: ft, kg the gain, kwc the
 * gain at a strong edge, kl the margin, zref the margin at a strong edge.
 * @param {number} inW
 * @param {number} inH
 * @param {number} outW
 * @param {number} outH
 * @returns {{ft: number, kg: number, kwc: number, kl: number, zref: number}}
 */
export function mwsrDownLaw(inW, inH, outW, outH) {
    const { scale, gain, edgeGain, margin, edgeMargin, fade, ft } = MWSR_DOWN_LAW;
    const s = Math.min(outW / inW, outH / inH);
    if (s < scale[0]) return { ft, kg: 0, kwc: 0, kl: 0, zref: 0 };
    const t = Math.min((s - scale[0]) / (scale[1] - scale[0]), 1);
    const at = ([a, b]) => a + (b - a) * t;
    const f = Math.min(Math.max((1 - s) / (1 - fade), 0), 1);
    return { ft, kg: at(gain) * f, kwc: at(edgeGain) * f, kl: at(margin), zref: at(edgeMargin) };
}

/**
 * The gaze zone's values (plan §15.2, Bruno's picture of 23/10/2026: a green
 * ring at the centre, about 40 % of the width and most of the height, an
 * orange ring around it at about 70 % of the width, the rest left readable):
 * the centre and the half-axes of the green ellipse as fractions of the
 * picture, the orange ring's width as a fraction of those half-axes (the
 * weight falls by a smoothstep from the ellipse to 1 + ring times it), the
 * vote threshold at the centre and its sharpness as a multiple of the law's,
 * whose values stay the border's. Read by the `up` text when its GAZE constant
 * is 1, weighted per draw by the stream's motion (the uniform `gaze`, 0 on a
 * still picture: one quality everywhere). Off by default, a lab key: these
 * values were never seen on this kernel, by an eye or by a score.
 */
export const MWSR_ZONE = Object.freeze({
    cx: 0.5,
    cy: 0.5,
    rx: 0.2,
    ry: 0.45,
    ring: 0.75,
    ft: 0.005,
    boost: 1.5,
});

/** Number of f32 in the uniform block: texel.xy, scale.xy, dims.xy (the input's
 *  size), across.xy (the across reads' distance, as a share of the input), kc,
 *  ft, kg, kwc, what the gaze zone reads (centre.xy, half-axes.xy, ring, its
 *  threshold, its kg, its weight), kl, what the host's zones count against, and
 *  a pad to the block's size. */
export const MWSR_UNIFORM_FLOATS = 24;

/**
 * The reduction's tap grids when the light is software's (LINEAR_LIGHT 1 or
 * 2), by the smallest scale (output / input) they serve: [minimum scale, taps,
 * R_DOWN]. A 2×2 grid of bilinear samples spread over the footprint is the
 * exact box average at ×0.5 (plan §4.3); below ×0.5 the 3×3 grid, spread a
 * quarter wider than the footprint, took +2.7 dB on text and 40 % off the
 * moiré of the zone plate in the lab (P3, journal J12); the 4×4 grid below
 * ×0.25 is the plan's, unmeasured.
 */
export const MWSR_DOWN_TAPS = Object.freeze([
    [0.5, 4, 1.0],
    [0.25, 9, 1.25],
    [0, 16, 1.25],
]);

/**
 * The grids through sRGB views (the sampler decodes, the target encodes,
 * LINEAR_LIGHT 0): one bilinear sample is the filter down to ×0.5, where it is
 * the exact box average, at the cost of the renderer's plain copy (J15: at
 * ×0.75, 30.2 dB against the host's Lanczos-2 on text and half the FLIP of
 * the gamma-space bilinear, where the 2×2 grids reach 27 to 29 dB); below
 * ×0.5 the same 3×3 and 4×4 grids as above.
 */
export const MWSR_DOWN_TAPS_SRGB = Object.freeze([
    [0.5, 1, 1.0],
    [0.25, 9, 1.25],
    [0, 16, 1.25],
]);

/**
 * Which text a frame of inW×inH drawn at outW×outH takes: 'copy' at exactly
 * 1:1 (the renderer copies, plan §5), 'down' with its grid, its span and its
 * light below 1 on either axis, 'up' otherwise. `srgb`: the renderer reads the
 * input through an `-srgb` view and draws into one (SDR: `viewFormats` on the
 * pass-0 texture and on the canvas), so the hardware makes the linear light
 * (linearLight 0, the LINEAR_LIGHT override) and one sample is the filter
 * down to ×0.5; `linear`: the feed holds linear light (the renderer's HDR pass
 * 0 writes it for a reduction), so the sampler mixes linear light too, on the
 * same grids, and the text encodes at the output (linearLight 3); otherwise
 * the light is software's (linearLight 1) and the 2×2 grid serves there.
 * @param {number} inW
 * @param {number} inH
 * @param {number} outW
 * @param {number} outH
 * @param {{srgb?: boolean, linear?: boolean}} [opts]
 * @returns {{mode: 'copy'|'up'|'down', taps: number, rDown: number, linearLight: number}}
 */
export function mwsrRegime(inW, inH, outW, outH, { srgb = false, linear = false } = {}) {
    if (inW === outW && inH === outH) return { mode: 'copy', taps: 0, rDown: 0, linearLight: 0 };
    const s = Math.min(outW / inW, outH / inH);
    if (s >= 1) return { mode: 'up', taps: 0, rDown: 0, linearLight: 0 };
    const table = srgb || linear ? MWSR_DOWN_TAPS_SRGB : MWSR_DOWN_TAPS;
    const linearLight = srgb ? 0 : linear ? 3 : 1;
    for (const [min, taps, rDown] of table)
        if (s >= min) return { mode: 'down', taps, rDown, linearLight };
    const [, taps, rDown] = table[table.length - 1];
    return { mode: 'down', taps, rDown, linearLight };
}

/**
 * The pipeline constants of a reduction's regime (mwsrRegime), for
 * `fragment.constants`. The one-read text has no grid, so it reads no R_DOWN,
 * and it must not be given one: WebKit refuses a pipeline a constant of which
 * its text does not read ("Fragment library could not be created", iOS 26.5,
 * 04/11/2026), and the frame then never reaches the canvas.
 * @param {{taps: number, rDown: number, linearLight: number}} reg
 * @returns {{R_DOWN?: number, LINEAR_LIGHT: number}}
 */
export function mwsrDownConstants(reg) {
    return reg.taps === 1
        ? { LINEAR_LIGHT: reg.linearLight }
        : { R_DOWN: reg.rDown, LINEAR_LIGHT: reg.linearLight };
}

/**
 * Every regime mwsrRegime() can answer a renderer with — the `up` kernel and
 * each grid of the reduction's table, through sRGB views or not — so that the
 * renderer builds all of its pipelines at creation and compiles nothing while
 * the stream runs (plan §4.3: the pinch zoom and the quality ladder change the
 * scale from one frame to the next). `linear`: the grids on a linear feed as
 * well, beside the software light's, which a frame the HDR pass 0 did not
 * take keeps.
 * @param {{srgb?: boolean, linear?: boolean}} [opts]
 * @returns {Array<{mode: 'up'|'down', taps: number, rDown: number, linearLight: number}>}
 */
export function mwsrRegimes({ srgb = false, linear = false } = {}) {
    const table = srgb ? MWSR_DOWN_TAPS_SRGB : MWSR_DOWN_TAPS;
    const linearLight = srgb ? 0 : 1;
    return [
        { mode: 'up', taps: 0, rDown: 0, linearLight: 0 },
        ...table.map(([, taps, rDown]) => ({ mode: 'down', taps, rDown, linearLight })),
        ...(linear && !srgb
            ? MWSR_DOWN_TAPS_SRGB.map(([, taps, rDown]) => ({
                  mode: 'down',
                  taps,
                  rDown,
                  linearLight: 3,
              }))
            : []),
    ];
}

/**
 * The name a renderer keeps a regime's pipeline or program under: 'up', or
 * 'down' and the grid's taps, 'lin' after them on a linear feed (otherwise
 * the span and the light follow from the taps and the renderer's views).
 * @param {{mode: 'up'|'down', taps?: number, linearLight?: number}} reg
 * @returns {string}
 */
export function mwsrRegimeKey(reg) {
    return reg.mode === 'up' ? 'up' : 'down' + reg.taps + (reg.linearLight === 3 ? 'lin' : '');
}

function zoneRef(sx, sy) {
    const x = Math.log(Math.min(Math.max(Math.min(sx, sy), 1.15), 3));
    const [c0, c1, c2, c3, c4] = MWSR_ZONE_REF;
    return Math.max(c0 + x * (c1 + x * (c2 + x * (c3 + x * c4))), 1e-3);
}

/**
 * The uniform block for an input of inW×inH drawn at outW×outH.
 * @param {number} inW
 * @param {number} inH
 * @param {number} outW
 * @param {number} outH
 * @param {{kc?: number, ft?: number, kg?: number, kr?: number, kwc?: number, kl?: number,
 *           sfull?: number, gaze?: number, zone?: Object, zones?: boolean,
 *           hdr?: boolean}} [u]
 *   a value not named comes from mwsrLaw at these sizes, from mwsrDownLaw for
 *   a reduction, whose values take the same places; `sfull`: over
 *   MWSR_CONSTANTS.S_FULL; `gaze`: the gaze zone's weight, 0 (none, the default)
 *   to 1, read by a text built with GAZE 1 only; `zone`: its values over
 *   MWSR_ZONE's; `zones`: the frame is read with the host's zones; `hdr`: the
 *   feed holds values past 1.0, the bound takes no margin and a reduction does
 *   not sharpen.
 * @returns {Float32Array}
 */
export function mwsrUniforms(inW, inH, outW, outH, u = {}) {
    const law = mwsrLaw(inW, inH, outW, outH);
    const gaze = Math.min(Math.max(u.gaze ?? 0, 0), 1);
    const z = { ...MWSR_ZONE, ...(u.zone || {}) };
    // a ring of no width would put the smoothstep's two edges together
    z.ring = Math.max(z.ring, 1e-3);
    const kr = u.kr ?? law.kr;
    // the sharpening's strength: none at 1:1 (the renderer copies the frame there), whole from S_FULL on
    const sfull = u.sfull ?? MWSR_CONSTANTS.S_FULL;
    const s = Math.min(outW / inW, outH / inH);
    const strength = Math.min(Math.max((s - 1) / (sfull - 1), 0), 1);
    // a reduction's own values, in the same places: its text reads them under these names
    const down = s < 1 ? mwsrDownLaw(inW, inH, outW, outH) : null;
    const kg = down ? (u.kg ?? (u.hdr ? 0 : down.kg)) : (u.kg ?? law.kg) * strength;
    return new Float32Array([
        1 / inW,
        1 / inH,
        outW / inW,
        outH / inH,
        inW,
        inH,
        kr / inW,
        kr / inH,
        u.kc ?? law.kc,
        // the edge's direction divides by the slope's length: a vote of 0 would let a slope of 0 through
        Math.max(u.ft ?? (down ? down.ft : law.ft), 1e-6),
        kg,
        down ? (u.kwc ?? (u.hdr ? 0 : down.kwc)) : (u.kwc ?? law.kwc) * strength,
        z.cx,
        z.cy,
        z.rx,
        z.ry,
        z.ring,
        Math.max(z.ft, 1e-6),
        kg * z.boost,
        gaze,
        u.kl ?? (down ? down.kl : u.hdr ? 0 : law.kl),
        u.zones ? zoneRef(outW / inW, outH / inH) : down ? down.zref : 1,
        0,
        0,
    ]);
}

function wgslNumber(type, v) {
    if (type === 'u32') return `${v >>> 0}u`;
    const s = String(v);
    return /[.e]/.test(s) ? s : s + '.0';
}

const FEEDS = {
    tex: {
        decl: '@group(0) @binding(1) var tex : texture_2d<f32>;',
        sample: `fn S(uv : vec2f) -> vec3f {
    return textureSampleLevel(tex, samp, uv, 0.0).rgb;
}`,
    },
    ext: {
        decl: '@group(0) @binding(1) var tex : texture_external;',
        sample: `fn S(uv : vec2f) -> vec3f {
    return textureSampleBaseClampToEdge(tex, samp, uv).rgb;
}`,
    },
};

// The native host's zones (plan B, upscaler-mwsr-b.md §12 c), the `up` text's
// second compilation: binding 3, an r16float texture of one texel per zone of
// the capture, the drop that zone takes off the across weight, which the
// sampler reads bilinearly between the zones' centres, as the bench did.
const MAP_DECL = '@group(0) @binding(3) var zoneMap : texture_2d<f32>;';

// The part every text shares: the vertex stage, the uniform block, the
// bindings, the feed, the overrides and the luma (and the zones' texture,
// `map`).
function preamble(feed, half, map = false) {
    const f = FEEDS[feed];
    if (!f) throw new Error('mwsr: feed ' + feed + '?');
    const prelude = half ? 'enable f16;\n\n' : '';
    const aliases = half
        ? 'alias hf = f16;\nalias h3 = vec3<f16>;'
        : 'alias hf = f32;\nalias h3 = vec3<f32>;';
    const overrides = Object.entries(MWSR_OVERRIDE_SPEC)
        .map(([k, [t, v]]) => `override ${k} : ${t} = ${wgslNumber(t, v)};`)
        .join('\n');
    const [lr, lg, lb] = MWSR_CONSTANTS.LUMA;
    return `${prelude}struct VSOut {
    @builtin(position) pos : vec4f,
    @location(0) uv : vec2f,
};

@vertex
fn vs(@builtin(vertex_index) vid : u32) -> VSOut {
    var out : VSOut;
    let uv = vec2f(f32((vid << 1u) & 2u), f32(vid & 2u));
    out.uv = uv;
    out.pos = vec4f(uv * vec2f(2.0, -2.0) + vec2f(-1.0, 1.0), 0.0, 1.0);
    return out;
}

struct U {
    texel : vec2f,
    scale : vec2f,
    dims : vec2f,
    across : vec2f,
    kc : f32,
    ft : f32,
    kg : f32,
    kwc : f32,
    zc : vec2f,
    zr : vec2f,
    zring : f32,
    zft : f32,
    zkg : f32,
    gaze : f32,
    kl : f32,
    zref : f32,
    pad1 : f32,
    pad2 : f32,
};

@group(0) @binding(0) var samp : sampler;
${f.decl}
@group(0) @binding(2) var<uniform> u : U;${map ? '\n' + MAP_DECL : ''}

${f.sample}

${aliases}

${overrides}

const LUMA : vec3f = vec3f(${wgslNumber('f32', lr)}, ${wgslNumber('f32', lg)}, ${wgslNumber('f32', lb)});
`;
}

// The `up` fragment stage: the kernel of the file's head.
const UP_FS = `
// The gaze zone's weight at a pixel (GAZE 1, plan §15.2): 1 inside the
// ellipse of centre u.zc and half-axes u.zr, falling by a smoothstep over the
// ring to 0 outside, times the stream's motion u.gaze — 0 on a still
// picture, where the law rules the whole screen.
fn gazeWeight(uv : vec2f) -> f32 {
    if (GAZE != 1u) {
        return 0.0;
    }
    let d = length((uv - u.zc) / u.zr);
    return (1.0 - smoothstep(1.0, 1.0 + u.zring, d)) * u.gaze;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let uv = in.uv;
    // The cubic of kc around the pixel, its middle pair on each axis read as one
    // bilinear sample: the four texels around, by one read.
    let sp = uv * u.dims;
    let f0 = floor(sp - 0.5);
    let t = sp - 0.5 - f0;
    let s = 1.0 - t;
    let ku = u.kc * t * s;
    let w12 = 1.0 + ku;
    let p12 = (f0 + 0.5 + (t * t * (3.0 - 2.0 * t) + ku * s) / w12) * u.texel;
    let rM = S(p12);
    // The gaze zone's weight, 0 without one: the vote threshold and, on an
    // edge, the sharpness slide from the law's values to the centre's.
    let zg = gazeWeight(uv);
    var ft = u.ft;
    if (GAZE == 1u) {
        ft = mix(u.ft, u.zft, zg);
    }
    // The vote: the screen derivatives of that read, scaled to a change per
    // input texel; coarse, so the whole quad takes the same branch. From the
    // luma, or (CHROMA444) from the channel with the steepest ramp, so that a
    // border between two colours of one luma is an edge too.
    var gM : vec2f;
    var flat : bool;
    if (CHROMA444 == 1u) {
        let dx = dpdxCoarse(rM) * u.scale.x;
        let dy = dpdyCoarse(rM) * u.scale.y;
        let m = abs(dx) + abs(dy);
        let rg = m.g > m.r;
        let b = m.b > max(m.r, m.g);
        gM = vec2f(select(select(dx.r, dx.g, rg), dx.b, b), select(select(dy.r, dy.g, rg), dy.b, b));
        flat = max(m.r, max(m.g, m.b)) < ft;
    } else {
        let lM = dot(rM, LUMA);
        gM = vec2f(dpdxCoarse(lM), dpdyCoarse(lM)) * u.scale;
        flat = abs(gM.x) + abs(gM.y) < ft;
    }
    if (SHOW == 1u) {
        return vec4f(vec3f(select(1.0, 0.0, flat)), 1.0);
    }
    if (SHOW == 2u) {
        return vec4f(vec3f(zg), 1.0);
    }
    if (flat) {
        return vec4f(rM, 1.0);
    }
    // On an edge: the cubic's outer lobes, one read per side.
    let p0 = (f0 - 0.5) * u.texel;
    let p3 = (f0 + 2.5) * u.texel;
    let rT = S(vec2f(p12.x, p0.y));
    let rL = S(vec2f(p0.x, p12.y));
    let rR = S(vec2f(p3.x, p12.y));
    let rB = S(vec2f(p12.x, p3.y));
    // The two reads across the edge, along the normal the derivatives measure.
    let dn = gM * inverseSqrt(dot(gM, gM)) * u.across;
    let ra = S(uv - dn);
    let rb = S(uv + dn);
    var kg = u.kg;
    if (GAZE == 1u) {
        kg = mix(u.kg, u.zkg, zg);
    }
    var kw = u.kwc;
    // From here the arithmetic is hf (f32, or f16 when the module says so).
    let hM = h3(rM);
    let hA = h3(ra);
    let hB = h3(rb);
    let hu = vec2<hf>(ku);
    let hs = vec2<hf>(s);
    let ht = vec2<hf>(t);
    let h12 = vec2<hf>(w12);
    let K = (h12.x * (h12.y * hM - hu.y * (hs.y * h3(rT) + ht.y * h3(rB))) - h12.y * hu.x * (hs.x * h3(rL) + ht.x * h3(rR))) / (hf(1.0) - hu.x * hu.y);
    let D = K - hf(0.5) * (hA + hB);
    let R = K + hf(kg) * D + hf(kw) * (D - h3(dot(D, h3(LUMA))));
    let mn = min(hM, min(hA, hB));
    let mx = max(hM, max(hA, hB));
    let mg = (mx - mn) * hf(u.kl);
    // A float target keeps what falls below zero: the feed holds none.
    return vec4f(vec3f(clamp(R, max(mn - mg, h3(hf(0.0))), mx + mg)), 1.0);
}
`;

// The `up` text that reads the zones (`map`): the sharpness on an edge is the
// law's times what the zone's drop leaves of it, which never exceeds the law
// (plan B §12 c). The renderer reads the zones from ×1.15 up only.
const ZONE_LAW = '    // From here the arithmetic is hf (f32, or f16 when the module says so).\n';
const ZONE_MAP = `    let zf = saturate(1.0 - textureSampleLevel(zoneMap, samp, uv, 0.0).r / u.zref);
    kg *= zf;
    kw *= zf;
${ZONE_LAW}`;
const UP_FS_MAP = UP_FS.replace(ZONE_LAW, ZONE_MAP);

// The k×k grid of the reduction, as offsets in the pixel's footprint (the
// span): the centres of k×k equal cells, row by row. Both twins unroll it.
function downOffsets(taps) {
    const k = Math.round(Math.sqrt(taps));
    if (![1, 2, 3, 4].includes(k) || k * k !== taps)
        throw new Error('mwsr: down taps ' + taps + '?');
    const offs = [];
    for (let i = 0; i < k; i++) offs.push((i + 0.5) / k - 0.5);
    const grid = [];
    for (const oy of offs) for (const ox of offs) grid.push([ox, oy]);
    return grid;
}

// The reduction's sharpening, in the block's places (MWSR_DOWN_LAW): after the
// read `c`, in linear light; the grids below ×0.5 (9 and 16 samples) have none.
// The derivatives come before the branch, in uniform control flow, as WGSL
// requires; the 4 more reads are made past the vote only.
function downSharpenWgsl() {
    const [r0, r1] = MWSR_DOWN_LAW.range.map((v) => wgslNumber('f32', v));
    return `    let l = dot(sqrt(max(c, vec3f(0.0))), LUMA);
    let g = vec2f(dpdxCoarse(l), dpdyCoarse(l));
    if (u.kg == 0.0 || abs(g.x) + abs(g.y) < u.ft) {
        return vec4f(enc(c), 1.0);
    }
    let px = u.texel / u.scale;
    let a = dec(S(uv - vec2f(px.x, 0.0)));
    let b = dec(S(uv + vec2f(px.x, 0.0)));
    let d = dec(S(uv - vec2f(0.0, px.y)));
    let e = dec(S(uv + vec2f(0.0, px.y)));
    let mean = 0.25 * (a + b + d + e);
    let mn = min(c, min(a, min(b, min(d, e))));
    let mx = max(c, max(a, max(b, max(d, e))));
    // kwc and zref: the gain and the margin at a strong edge
    let t = smoothstep(${r0}, ${r1}, dot(sqrt(max(mx, vec3f(0.0))) - sqrt(max(mn, vec3f(0.0))), LUMA));
    let R = c + mix(u.kg, u.kwc, t) * (c - mean);
    let mg = (mx - mn) * mix(u.kl, u.zref, t);
    return vec4f(enc(clamp(R, max(mn - mg, vec3f(0.0)), mx + mg)), 1.0);`;
}

// The `down` fragment stage: a k×k grid of bilinear samples over the pixel's
// footprint, averaged (plan §4.3), unrolled here so that the text holds no loop.
function downFs(taps) {
    const k = Math.round(Math.sqrt(taps));
    const lines = downOffsets(taps).map(
        ([ox, oy]) =>
            `    acc += dec(S(uv + vec2f(${wgslNumber('f32', ox)}, ${wgslNumber('f32', oy)}) * span));`,
    );
    return `
// The sRGB transfer, both ways, for LINEAR_LIGHT 2 (the encode alone for 3);
// past 1.0 (HDR) the same formula continues, as the renderer's HDR pass 0
// encodes it.
fn srgbToLin(c : vec3f) -> vec3f {
    let lo = c / 12.92;
    let hi = pow((max(c, vec3f(0.0)) + 0.055) / 1.055, vec3f(2.4));
    return select(hi, lo, c <= vec3f(0.04045));
}

fn linToSrgb(c : vec3f) -> vec3f {
    let lo = c * 12.92;
    let hi = 1.055 * pow(max(c, vec3f(0.0)), vec3f(1.0 / 2.4)) - 0.055;
    return select(hi, lo, c <= vec3f(0.0031308));
}

fn dec(c : vec3f) -> vec3f {
    if (LINEAR_LIGHT == 1u) {
        return c * c;
    }
    if (LINEAR_LIGHT == 2u) {
        return srgbToLin(c);
    }
    return c;
}

fn enc(c : vec3f) -> vec3f {
    if (LINEAR_LIGHT == 1u) {
        return sqrt(max(c, vec3f(0.0)));
    }
    if (LINEAR_LIGHT >= 2u) {
        return linToSrgb(c);
    }
    return c;
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let uv = in.uv;
${
    taps === 1
        ? `    // One sample at the pixel's centre is the whole filter.
    let c = dec(S(uv));
${downSharpenWgsl()}`
        : `    // The pixel's footprint in the input, 1 / scale texels per axis, spanned
    // by the grid (R_DOWN 1); ${k}×${k} samples at its ${k}×${k} centres.
    let span = u.texel / u.scale * R_DOWN;
    var acc = vec3f(0.0);
${lines.join('\n')}
${
    taps === 4
        ? `    let c = acc * ${wgslNumber('f32', 1 / taps)};
${downSharpenWgsl()}`
        : `    return vec4f(enc(acc * ${wgslNumber('f32', 1 / taps)}), 1.0);`
}`
}
}
`;
}

/**
 * The WGSL text of one mode: a full-screen triangle (`vs`) and the fragment
 * stage (`fs`); bindings 0 sampler (linear, clamp to edge), 1 the feed, 2 the
 * uniform block of mwsrUniforms(). Explicit LOD and clamp-to-edge sampling
 * only, so the samples may sit in the non-uniform branch; the derivatives are
 * taken before it, in uniform control flow, as WGSL requires.
 * @param {{feed?: 'tex'|'ext', half?: boolean, mode?: 'up'|'down', taps?: number, fine?: boolean,
 *          map?: boolean}} [opts]
 *   'tex' reads a texture_2d<f32>: the product's path, after its pass 0 (the
 *   external texture's two planes per sample made 'ext' lose to pass 0 +
 *   kernel on the RTX, the AMD iGPU and the M1 alike, P2.3; 'ext' stays for
 *   the lab). `half` runs the up mode's colour arithmetic in f16 (`enable
 *   f16`: the device must have 'shader-f16'); the coordinates, the vote and the
 *   down mode stay in f32.
 *   `mode` 'down' is the reduction with `taps` samples (1, 4, 9 or 16; see
 *   mwsrRegime()), which sharpens with 1 or 4 (MWSR_DOWN_LAW). `map`, the up mode only: the second text, which reads the
 *   native host's zones from binding 3 (a texture_2d<f32>, see UP_FS_MAP);
 *   without it the text is the same as before the zones, to the byte.
 * @returns {string}
 */
export function mwsrWgsl({
    feed = 'tex',
    half = false,
    mode = 'up',
    taps = 4,
    fine = false,
    map = false,
} = {}) {
    if (mode === 'up') {
        const fs = map ? UP_FS_MAP : UP_FS;
        return (
            preamble(feed, half, map) + (fine ? fs.replace(/dpd([xy])Coarse/g, 'dpd$1Fine') : fs)
        );
    }
    if (map) throw new Error('mwsr: the zones are read by the up mode only');
    if (mode === 'down') return preamble(feed, false) + downFs(taps);
    throw new Error('mwsr: mode ' + mode + '?');
}

// The `up` fragment stage in GLSL: the kernel of the WGSL text above, line for
// line. The colour's arithmetic is mediump — f16 where the GPU has it, the twin
// of the `half` module — the coordinates, the derivatives and the result highp.
const UP_GLSL = `
// The gaze zone's weight, the WGSL text's gazeWeight line for line.
float gazeWeight(vec2 uv) {
    if (GAZE != 1u) {
        return 0.0;
    }
    float d = length((uv - uZone.xy) / uZone.zw);
    return (1.0 - smoothstep(1.0, 1.0 + uZoneBonus.x, d)) * uZoneBonus.w;
}

void main() {
    vec2 uv = vUv;
    vec2 texel = uTexelScale.xy;
    vec2 sp = uv * uDimsAcross.xy;
    vec2 f0 = floor(sp - 0.5);
    vec2 t = sp - 0.5 - f0;
    vec2 s = 1.0 - t;
    vec2 ku = uKernel.x * t * s;
    vec2 w12 = 1.0 + ku;
    vec2 p12 = (f0 + 0.5 + (t * t * (3.0 - 2.0 * t) + ku * s) / w12) * texel;
    vec3 rM = S(p12);
    float zg = gazeWeight(uv);
    float ft = uKernel.y;
    if (GAZE == 1u) {
        ft = mix(uKernel.y, uZoneBonus.y, zg);
    }
    // GL's window y runs up, WebGPU's framebuffer y runs down: dFdy is the
    // WGSL text's dpdy negated, or the edge normal comes out mirrored and the
    // across taps fall along the edge on every diagonal (the first conformity run).
    vec2 gM;
    bool isFlat;
    if (CHROMA444 == 1u) {
        vec3 dx = dFdx(rM) * uTexelScale.z;
        vec3 dy = -dFdy(rM) * uTexelScale.w;
        vec3 m = abs(dx) + abs(dy);
        bool rg = m.g > m.r;
        bool b = m.b > max(m.r, m.g);
        gM = vec2(b ? dx.b : (rg ? dx.g : dx.r), b ? dy.b : (rg ? dy.g : dy.r));
        isFlat = max(m.r, max(m.g, m.b)) < ft;
    } else {
        float lM = dot(rM, LUMA);
        gM = vec2(dFdx(lM) * uTexelScale.z, -dFdy(lM) * uTexelScale.w);
        isFlat = abs(gM.x) + abs(gM.y) < ft;
    }
    if (SHOW == 1u) {
        oColor = vec4(vec3(isFlat ? 0.0 : 1.0), 1.0);
        return;
    }
    if (SHOW == 2u) {
        oColor = vec4(vec3(zg), 1.0);
        return;
    }
    if (isFlat) {
        oColor = vec4(rM, 1.0);
        return;
    }
    vec2 p0 = (f0 - 0.5) * texel;
    vec2 p3 = (f0 + 2.5) * texel;
    vec3 rT = S(vec2(p12.x, p0.y));
    vec3 rL = S(vec2(p0.x, p12.y));
    vec3 rR = S(vec2(p3.x, p12.y));
    vec3 rB = S(vec2(p12.x, p3.y));
    vec2 dn = gM * inversesqrt(dot(gM, gM)) * uDimsAcross.zw;
    vec3 ra = S(uv - dn);
    vec3 rb = S(uv + dn);
    float kg = uKernel.z;
    if (GAZE == 1u) {
        kg = mix(uKernel.z, uZoneBonus.z, zg);
    }
    mediump vec3 hM = rM;
    mediump vec3 hA = ra;
    mediump vec3 hB = rb;
    mediump vec3 hTop = rT;
    mediump vec3 hLeft = rL;
    mediump vec3 hRight = rR;
    mediump vec3 hBottom = rB;
    mediump vec2 hu = ku;
    mediump vec2 hs = s;
    mediump vec2 ht = t;
    mediump vec2 h12 = w12;
    mediump vec3 K = (h12.x * (h12.y * hM - hu.y * (hs.y * hTop + ht.y * hBottom)) - h12.y * hu.x * (hs.x * hLeft + ht.x * hRight)) / (1.0 - hu.x * hu.y);
    mediump vec3 D = K - 0.5 * (hA + hB);
    mediump float hg = kg;
    mediump float hw = uKernel.w;
    mediump vec3 R = K + hg * D + hw * (D - vec3(dot(D, LUMA)));
    mediump vec3 mn = min(hM, min(hA, hB));
    mediump vec3 mx = max(hM, max(hA, hB));
    mediump float hl = uBound.x;
    mediump vec3 mg = (mx - mn) * hl;
    oColor = vec4(clamp(R, max(mn - mg, vec3(0.0)), mx + mg), 1.0);
}
`;

// The reduction's sharpening in GLSL, the WGSL text's line for line: kg in
// uKernel.z, its value at a strong edge in .w; the margin and its own in uBound.
function downSharpenGlsl() {
    const [r0, r1] = MWSR_DOWN_LAW.range.map((v) => wgslNumber('f32', v));
    return `    float l = dot(sqrt(max(c, vec3(0.0))), LUMA);
    vec2 g = vec2(dFdx(l), dFdy(l));
    if (uKernel.z == 0.0 || abs(g.x) + abs(g.y) < uKernel.y) {
        oColor = vec4(enc(c), 1.0);
        return;
    }
    vec2 px = uTexelScale.xy / uTexelScale.zw;
    vec3 a = dec(S(uv - vec2(px.x, 0.0)));
    vec3 b = dec(S(uv + vec2(px.x, 0.0)));
    vec3 d = dec(S(uv - vec2(0.0, px.y)));
    vec3 e = dec(S(uv + vec2(0.0, px.y)));
    vec3 mean = 0.25 * (a + b + d + e);
    vec3 mn = min(c, min(a, min(b, min(d, e))));
    vec3 mx = max(c, max(a, max(b, max(d, e))));
    float t = smoothstep(${r0}, ${r1}, dot(sqrt(max(mx, vec3(0.0))) - sqrt(max(mn, vec3(0.0))), LUMA));
    vec3 R = c + mix(uKernel.z, uKernel.w, t) * (c - mean);
    vec3 mg = (mx - mn) * mix(uBound.x, uBound.y, t);
    oColor = vec4(enc(clamp(R, max(mn - mg, vec3(0.0)), mx + mg)), 1.0);`;
}

// The `down` fragment stage in GLSL: the same grid, from the same offsets.
function downGlsl(taps) {
    const lines = downOffsets(taps).map(
        ([ox, oy]) =>
            `    acc += dec(S(uv + vec2(${wgslNumber('f32', ox)}, ${wgslNumber('f32', oy)}) * span));`,
    );
    return `
vec3 srgbToLin(vec3 c) {
    vec3 lo = c / 12.92;
    vec3 hi = pow((max(c, vec3(0.0)) + 0.055) / 1.055, vec3(2.4));
    return mix(hi, lo, lessThanEqual(c, vec3(0.04045)));
}

vec3 linToSrgb(vec3 c) {
    vec3 lo = c * 12.92;
    vec3 hi = 1.055 * pow(max(c, vec3(0.0)), vec3(1.0 / 2.4)) - 0.055;
    return mix(hi, lo, lessThanEqual(c, vec3(0.0031308)));
}

vec3 dec(vec3 c) {
    if (LINEAR_LIGHT == 1u) {
        return c * c;
    }
    if (LINEAR_LIGHT == 2u) {
        return srgbToLin(c);
    }
    return c;
}

vec3 enc(vec3 c) {
    if (LINEAR_LIGHT == 1u) {
        return sqrt(max(c, vec3(0.0)));
    }
    if (LINEAR_LIGHT == 2u) {
        return linToSrgb(c);
    }
    return c;
}

void main() {
    vec2 uv = vUv;
${
    taps === 1
        ? `    vec3 c = dec(S(uv));
${downSharpenGlsl()}`
        : `    vec2 span = uTexelScale.xy / uTexelScale.zw * R_DOWN;
    vec3 acc = vec3(0.0);
${lines.join('\n')}
${
    taps === 4
        ? `    vec3 c = acc * ${wgslNumber('f32', 1 / taps)};
${downSharpenGlsl()}`
        : `    oColor = vec4(enc(acc * ${wgslNumber('f32', 1 / taps)}), 1.0);`
}`
}
}
`;
}

// The part every GLSL text shares: the uniforms (the block of mwsrUniforms()
// in six vec4), the varying and the output of WebGlRenderer's full-screen
// triangle, the feed, the overrides as constants and the luma.
function glslPreamble(overrides) {
    for (const k of Object.keys(overrides))
        if (!(k in MWSR_OVERRIDE_SPEC)) throw new Error('mwsr: override ' + k + '?');
    const consts = Object.entries(MWSR_OVERRIDE_SPEC)
        .map(
            ([k, [t, v]]) =>
                `const ${t === 'u32' ? 'uint' : 'float'} ${k} = ${wgslNumber(t, overrides[k] ?? v)};`,
        )
        .join('\n');
    const [lr, lg, lb] = MWSR_CONSTANTS.LUMA;
    return `#version 300 es
precision highp float;
precision highp int;
uniform sampler2D uTex;
uniform vec4 uTexelScale; // texel.xy, scale.xy: the first four of mwsrUniforms()
uniform vec4 uDimsAcross; // the input's size, the across taps' distance
uniform vec4 uKernel; // kc, ft, kg, kwc
uniform vec4 uZone; // the gaze zone's centre.xy and half-axes.xy (GAZE 1)
uniform vec4 uZoneBonus; // its ring, its threshold and kg at the centre, its weight
uniform vec4 uBound; // kl, zref, then the block's pad
in vec2 vUv;
out vec4 oColor;

vec3 S(vec2 uv) {
    return textureLod(uTex, uv, 0.0).rgb;
}

${consts}

const vec3 LUMA = vec3(${wgslNumber('f32', lr)}, ${wgslNumber('f32', lg)}, ${wgslNumber('f32', lb)});
`;
}

/**
 * The GLSL ES 3.00 twin of one mode (plan §3.6): the same kernel and the same
 * constants as mwsrWgsl(), from the same tables, for WebGlRenderer's
 * full-screen triangle (`vUv` in, `oColor` out). WebGL2 has no pipeline
 * constants, so the overrides are baked into the text (`overrides`, by name;
 * the rest at their defaults); six vec4 uniforms carry the block of
 * mwsrUniforms(). The colour's arithmetic is mediump: f16 on the GPUs that have
 * it, the twin of the `half` module, and highp on the others.
 * No sRGB views on a WebGL2 drawing buffer: the reduction takes the software
 * light (MWSR_DOWN_TAPS, LINEAR_LIGHT 1). LINEAR_LIGHT 3 has no twin here:
 * WebGL2 draws no HDR feed.
 * @param {{mode?: 'up'|'down', taps?: number, overrides?: Object<string, number>}} [opts]
 * @returns {string}
 */
export function mwsrGlsl({ mode = 'up', taps = 4, overrides = {} } = {}) {
    if (mode === 'up') return glslPreamble(overrides) + UP_GLSL;
    if (mode === 'down') return glslPreamble(overrides) + downGlsl(taps);
    throw new Error('mwsr: mode ' + mode + '?');
}

// The part every HLSL text shares: the vertex stage, the constant buffer (the
// block of mwsrUniforms(), the same 24 floats in the same order), the feed,
// the constants and the luma. Each constant is a define of its name prefixed
// MWSR_, at its default unless the compiler is handed another (D3DCompile's
// D3D_SHADER_MACRO, DXC's -D), the way the WGSL text takes pipeline constants;
// the registers are defines too, for a root signature that wants others.
function hlslPreamble(half) {
    const consts = Object.entries(MWSR_OVERRIDE_SPEC)
        .map(
            ([k, [t, v]]) =>
                `#ifndef MWSR_${k}\n#define MWSR_${k} ${wgslNumber(t, v)}\n#endif\n` +
                `static const ${t === 'u32' ? 'uint' : 'float'} ${k} = MWSR_${k};`,
        )
        .join('\n');
    const [lr, lg, lb] = MWSR_CONSTANTS.LUMA;
    const types = half
        ? 'typedef min16float hf;\ntypedef min16float3 h3;'
        : 'typedef float hf;\ntypedef float3 h3;';
    return `#ifndef MWSR_CB_REGISTER
#define MWSR_CB_REGISTER b0
#endif
#ifndef MWSR_TEX_REGISTER
#define MWSR_TEX_REGISTER t0
#endif
#ifndef MWSR_SAMPLER_REGISTER
#define MWSR_SAMPLER_REGISTER s0
#endif

struct VSOut {
    float4 pos : SV_Position;
    float2 uv : TEXCOORD0;
};

VSOut MwsrVS(uint vid : SV_VertexID) {
    VSOut o;
    float2 uv = float2(float((vid << 1) & 2u), float(vid & 2u));
    o.uv = uv;
    o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

cbuffer MwsrConstants : register(MWSR_CB_REGISTER) {
    float2 texel;
    float2 scale;
    float2 dims;
    float2 across;
    float kc;
    float ft;
    float kg;
    float kwc;
    float2 zc;
    float2 zr;
    float zring;
    float zft;
    float zkg;
    float gaze;
    float kl;
    float zref;
    float pad1;
    float pad2;
};

Texture2D<float4> tex : register(MWSR_TEX_REGISTER);
SamplerState samp : register(MWSR_SAMPLER_REGISTER);

float3 S(float2 uv) {
    return tex.SampleLevel(samp, uv, 0.0).rgb;
}

${types}

${consts}

static const float3 LUMA = float3(${wgslNumber('f32', lr)}, ${wgslNumber('f32', lg)}, ${wgslNumber('f32', lb)});
`;
}

// The `up` pixel stage in HLSL: the kernel of the WGSL text, line for line.
// Direct3D's screen y runs down as WebGPU's does: the derivatives keep their
// sign. The colour's arithmetic is hf, min16float in the `half` text, the twin
// of the WGSL f16 module.
const UP_HLSL = `
// The gaze zone's weight, the WGSL text's gazeWeight line for line.
float gazeWeight(float2 uv) {
    if (GAZE != 1u) {
        return 0.0;
    }
    float d = length((uv - zc) / zr);
    return (1.0 - smoothstep(1.0, 1.0 + zring, d)) * gaze;
}

float4 MwsrPS(VSOut i) : SV_Target {
    float2 uv = i.uv;
    float2 sp = uv * dims;
    float2 f0 = floor(sp - 0.5);
    float2 t = sp - 0.5 - f0;
    float2 s = 1.0 - t;
    float2 ku = kc * t * s;
    float2 w12 = 1.0 + ku;
    float2 p12 = (f0 + 0.5 + (t * t * (3.0 - 2.0 * t) + ku * s) / w12) * texel;
    float3 rM = S(p12);
    float zg = gazeWeight(uv);
    float vote = ft;
    if (GAZE == 1u) {
        vote = lerp(ft, zft, zg);
    }
    float2 gM;
    bool isFlat;
    if (CHROMA444 == 1u) {
        float3 dx = ddx_coarse(rM) * scale.x;
        float3 dy = ddy_coarse(rM) * scale.y;
        float3 m = abs(dx) + abs(dy);
        bool rg = m.g > m.r;
        bool b = m.b > max(m.r, m.g);
        gM = float2(b ? dx.b : (rg ? dx.g : dx.r), b ? dy.b : (rg ? dy.g : dy.r));
        isFlat = max(m.r, max(m.g, m.b)) < vote;
    } else {
        float lM = dot(rM, LUMA);
        gM = float2(ddx_coarse(lM), ddy_coarse(lM)) * scale;
        isFlat = abs(gM.x) + abs(gM.y) < vote;
    }
    if (SHOW == 1u) {
        return float4((isFlat ? 0.0 : 1.0).xxx, 1.0);
    }
    if (SHOW == 2u) {
        return float4(zg.xxx, 1.0);
    }
    if (isFlat) {
        return float4(rM, 1.0);
    }
    float2 p0 = (f0 - 0.5) * texel;
    float2 p3 = (f0 + 2.5) * texel;
    float3 rT = S(float2(p12.x, p0.y));
    float3 rL = S(float2(p0.x, p12.y));
    float3 rR = S(float2(p3.x, p12.y));
    float3 rB = S(float2(p12.x, p3.y));
    float2 dn = gM * rsqrt(dot(gM, gM)) * across;
    float3 ra = S(uv - dn);
    float3 rb = S(uv + dn);
    float sharp = kg;
    if (GAZE == 1u) {
        sharp = lerp(kg, zkg, zg);
    }
    h3 hM = (h3)rM;
    h3 hA = (h3)ra;
    h3 hB = (h3)rb;
    hf hux = (hf)ku.x;
    hf huy = (hf)ku.y;
    hf h12x = (hf)w12.x;
    hf h12y = (hf)w12.y;
    h3 K = (h12x * (h12y * hM - huy * ((hf)s.y * (h3)rT + (hf)t.y * (h3)rB)) - h12y * hux * ((hf)s.x * (h3)rL + (hf)t.x * (h3)rR)) / ((hf)1.0 - hux * huy);
    h3 D = K - (hf)0.5 * (hA + hB);
    h3 R = K + (hf)sharp * D + (hf)kwc * (D - (h3)dot(D, (h3)LUMA));
    h3 mn = min(hM, min(hA, hB));
    h3 mx = max(hM, max(hA, hB));
    h3 mg = (mx - mn) * (hf)kl;
    return float4((float3)clamp(R, max(mn - mg, (h3)(hf)0.0), mx + mg), 1.0);
}
`;

// The reduction's sharpening in HLSL, the WGSL text's line for line.
function downSharpenHlsl() {
    const [r0, r1] = MWSR_DOWN_LAW.range.map((v) => wgslNumber('f32', v));
    return `    float l = dot(sqrt(max(c, 0.0)), LUMA);
    float2 g = float2(ddx_coarse(l), ddy_coarse(l));
    if (kg == 0.0 || abs(g.x) + abs(g.y) < ft) {
        return float4(enc(c), 1.0);
    }
    float2 px = texel / scale;
    float3 a = dec(S(uv - float2(px.x, 0.0)));
    float3 b = dec(S(uv + float2(px.x, 0.0)));
    float3 d = dec(S(uv - float2(0.0, px.y)));
    float3 e = dec(S(uv + float2(0.0, px.y)));
    float3 mean = 0.25 * (a + b + d + e);
    float3 mn = min(c, min(a, min(b, min(d, e))));
    float3 mx = max(c, max(a, max(b, max(d, e))));
    float t = smoothstep(${r0}, ${r1}, dot(sqrt(max(mx, 0.0)) - sqrt(max(mn, 0.0)), LUMA));
    float3 R = c + lerp(kg, kwc, t) * (c - mean);
    float3 mg = (mx - mn) * lerp(kl, zref, t);
    return float4(enc(clamp(R, max(mn - mg, 0.0), mx + mg)), 1.0);`;
}

// The `down` pixel stage in HLSL: the same grid, from the same offsets, and
// the WGSL text's light, LINEAR_LIGHT 3 included (an HDR feed in linear light).
function downHlsl(taps) {
    const lines = downOffsets(taps).map(
        ([ox, oy]) =>
            `    acc += dec(S(uv + float2(${wgslNumber('f32', ox)}, ${wgslNumber('f32', oy)}) * span));`,
    );
    return `
float3 srgbToLin(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow((max(c, 0.0) + 0.055) / 1.055, (float3)2.4);
    return float3(c.r <= 0.04045 ? lo.r : hi.r, c.g <= 0.04045 ? lo.g : hi.g, c.b <= 0.04045 ? lo.b : hi.b);
}

float3 linToSrgb(float3 c) {
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), (float3)(1.0 / 2.4)) - 0.055;
    return float3(c.r <= 0.0031308 ? lo.r : hi.r, c.g <= 0.0031308 ? lo.g : hi.g,
                  c.b <= 0.0031308 ? lo.b : hi.b);
}

float3 dec(float3 c) {
    if (LINEAR_LIGHT == 1u) {
        return c * c;
    }
    if (LINEAR_LIGHT == 2u) {
        return srgbToLin(c);
    }
    return c;
}

float3 enc(float3 c) {
    if (LINEAR_LIGHT == 1u) {
        return sqrt(max(c, 0.0));
    }
    if (LINEAR_LIGHT >= 2u) {
        return linToSrgb(c);
    }
    return c;
}

float4 MwsrPS(VSOut i) : SV_Target {
    float2 uv = i.uv;
${
    taps === 1
        ? `    float3 c = dec(S(uv));
${downSharpenHlsl()}`
        : `    float2 span = texel / scale * R_DOWN;
    float3 acc = 0.0;
${lines.join('\n')}
${
    taps === 4
        ? `    float3 c = acc * ${wgslNumber('f32', 1 / taps)};
${downSharpenHlsl()}`
        : `    return float4(enc(acc * ${wgslNumber('f32', 1 / taps)}), 1.0);`
}`
}
}
`;
}

/**
 * The HLSL twin of one mode, for Direct3D 11 (FXC, ps_5_0) and 12 (DXC,
 * ps_6_0): the same kernel and the same constants as mwsrWgsl(), from the
 * same tables. Entry points MwsrVS (the full-screen triangle, three vertices
 * from SV_VertexID) and MwsrPS. Registers b0 (the 24 floats of
 * mwsrUniforms()), t0 (the input) and s0 (a linear, clamp-to-edge sampler),
 * each a define (MWSR_CB_REGISTER, MWSR_TEX_REGISTER, MWSR_SAMPLER_REGISTER).
 * The constants are defines of their name prefixed MWSR_ (MWSR_CHROMA444,
 * MWSR_R_DOWN...), at their default unless the compiler is handed another.
 * `half` runs the up mode's colour arithmetic in min16float, the twin of the
 * WGSL f16 module.
 * @param {{mode?: 'up'|'down', taps?: number, half?: boolean}} [opts]
 * @returns {string}
 */
export function mwsrHlsl({ mode = 'up', taps = 4, half = false } = {}) {
    if (mode === 'up') return hlslPreamble(half) + UP_HLSL;
    if (mode === 'down') return hlslPreamble(false) + downHlsl(taps);
    throw new Error('mwsr: mode ' + mode + '?');
}
