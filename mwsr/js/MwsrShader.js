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
 * `up` — the kernel, per output pixel (plan §3.2):
 *   1. one bilinear sample C at the pixel's position in the input, its luma;
 *   2. the vote: the screen derivatives of that luma, scaled to luma per input
 *      texel, decide per 2×2 quad whether the pixel sits on an edge; a flat
 *      quad returns C — one sample, a dozen operations;
 *   3. on an edge, four more bilinear samples, laid along and across the edge
 *      the derivatives measure: the along pair smooths the staircase, the
 *      across pair sharpens the ramp;
 *   4. a coherence term (a step or a ridge across, against all the variation
 *      around) doses both, so that grain, texture and compression blocks are
 *      left alone; the whole fades to C near 1:1;
 *   5. the result is bounded by the four input texels around the pixel
 *      (DERING 2, the default since J37: a line one texel wide keeps its
 *      depth, J35), or by the five samples first, then the texels (DERING 1).
 *   With CHROMA444 (4:4:4 streams, plan §4.2) the vote, the coherence and the
 *   weights are taken per channel, so that a border between two colours of one
 *   luma is seen and sharpened too.
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
 *   No vote, no sharpening: a reduction sharpens by itself.
 *
 * HDR (plan §4.1): the renderer's `linear` feed is sRGB-encoded past 1.0
 * (1.0 = reference white); both modes are per channel, bounded by their own
 * samples and clamp nothing to 1.0, so they run on it unchanged. For a
 * reduction, though, the renderer's HDR pass 0 writes the linear light itself
 * (LINEAR_LIGHT 3): the sampler then mixes linear light, as through the sRGB
 * views of SDR, and the grids are those views' (Bruno, 26/10/2026: a filter
 * mixing the encoded values took fine contrast down by 8 to 28 %, journal J45).
 *
 * Written from scratch (plan §10.2). Ideas taken, and credited: bilinear taps
 * instead of gathers (Atyuwen's mobile EASU, Jimenez, MJP); the early out on
 * an edge vote (SGSR1, NEDI); the local min/max bound against ringing (EASU,
 * SGSR1); sharpening held by the local range (CAS, RCAS). The vote by screen
 * derivatives and the taps laid along the measured edge are this project's.
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
    /** Weight of each tap along the edge, at full coherence and strength. */
    W_ALONG: ['f32', 0.15],
    /** Weight of each tap across the edge, negated and × sharpness. */
    W_ACROSS: ['f32', 0.3],
    /** Distance of the along taps, input texels. */
    R_ALONG: ['f32', 0.5],
    /** Distance of the across taps, input texels. */
    R_ACROSS: ['f32', 1.0],
    /** The scale (output / input) from which the kernel is at full strength; 0 at 1:1. */
    S_FULL: ['f32', 1.5],
    /** The most negative weight of an across tap: the blend's divisor stays >= 1 + 2 W_MIN
     *  (0.3 × 1.67 at sharpness 1.67; at the default sharpness 1 the cap is never reached). */
    W_MIN: ['f32', -0.5],
    /** The edge path's bound (the flat path keeps its taps' range): 0 the five samples of the blend; 1 the five samples, then the four input texels around the pixel (four loads); 2 the four texels alone, so that a line's pixel may pass its bilinear mix, as far as the texel (J35; the default since 23/10/2026, J37). */
    DERING: ['u32', 2],
    /** 1: the vote, the coherence and the weights per channel (4:4:4 streams); 0: from the luma, shared by the channels. */
    CHROMA444: ['u32', 0],
    /** The flat path's reconstruction: 1 the bilinear sample itself; 5 or 9 bilinear taps of a cubic (Mitchell-Netravali, B 0, C FLAT_C) over the 4×4 texels around, the four corners dropped at 5 (J33); 3 the interpolating quadratic of Dodgson over the 3×3 texels around, its corner dropped (J34). 5 is the product's default on a computer, 3 on a phone or a tablet (mwsrClassOverrides; Bruno, 24/10/2026, journal J36-J38). */
    FLAT_TAPS: ['u32', 5],
    /** The cubic's C, the depth of its negative lobe: 0.5 is Catmull-Rom; higher is steeper, with more ringing (bounded by the taps). */
    FLAT_C: ['f32', 0.5],
    /** 1: the gaze zone (plan §15.2): the vote threshold and the sharpness slide from the table's values to MWSR_ZONE's at the centre of the picture, by the pixel's place and the stream's motion (the uniform `gaze`); 0: the text has no zone. */
    GAZE: ['u32', 0],
    /** With GAZE 1: 1 the flat path's cubic or quadratic (FLAT_TAPS) inside the zone only, blended by its weight, the periphery keeping the bilinear sample; 0 everywhere. */
    FLAT_ZONE: ['u32', 0],
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
 * the laboratory's own overrides (Bruno's decisions of 24/10/2026, journal
 * J36-J38): a computer keeps the defaults, the flat path's Catmull-Rom of
 * five taps; a phone or a tablet takes the quadratic of three (FLAT_TAPS 3),
 * 9 to 13 % faster there (iPhone 13 Pro, realme Note 60) for a hundredth of
 * SSIM. The renderers call it once, with BrowserDetect's IS_HANDHELD.
 * @param {{handheld: boolean}} device a phone or a tablet, not a TV
 * @returns {Object<string, number>} name → value, empty on a computer
 */
export function mwsrClassOverrides({ handheld }) {
    return handheld ? { FLAT_TAPS: 3 } : {};
}

/** Constants shared by the WGSL text and, later, its GLSL twin. */
export const MWSR_CONSTANTS = Object.freeze({
    /** Luma weights: Rec. 709 rounded to powers of two, two multiply-adds. */
    LUMA: [0.25, 0.625, 0.125],
    /** Guards the divisions by a variation of luma. */
    EPS: 1e-5,
});

/** The per-draw values of the uniform block: the vote threshold (luma per
 *  input texel) and the sharpness (× W_ACROSS; 1.85 for every device since
 *  24/10/2026, Bruno's eye on his iPhone at ×1.33, J37: text near FSR1's,
 *  no overshoot; 2.0 before, from the lab's J31). That value holds up to
 *  ×1.33: mwsrUniforms() takes mwsrSharpness(scale) by default, which keeps
 *  the across weight of ×1.33 from there up (Bruno's yes, 24/10/2026, J40),
 *  then ramps to MWSR_SHARP_X2 at ×2 (his choice of 29/10/2026). */
export const MWSR_UNIFORMS = Object.freeze({ tau: 0.05, sharp: 1.85 });

/** The scale whose across weight the sharpness law holds from there up:
 *  ×1.33, the 1080p → 1440p step Bruno's eye set the sharpness at (J37). */
export const MWSR_SHARP_REF_SCALE = 4 / 3;

/** The sharpness from ×2 up, and the scale its ramp starts from: Bruno's
 *  choice of 29/10/2026 (plan B §8). At ×2 holding the across weight of
 *  ×1.33 alone gives 1.23, and 1.5 takes text +0.35 point of SSIM for 0 A.D.
 *  −0.08 and the étalon −0.41, the games' edges as sharp as FSR1's; at ×1.5
 *  and ×1.75 every picture wants 1.23 or less, hence a ramp from ×1.75. */
export const MWSR_SHARP_X2 = Object.freeze({ sharp: 1.5, from: 1.75 });

/**
 * The sharpness law by the scale (plan §12, journals J39 and J40), the
 * product's since Bruno's yes of 24/10/2026 (mwsrUniforms() takes it when no
 * sharpness is named; the comparator starts from it): the kernel's strength ramps from
 * 0 at 1:1 to 1 at S_FULL, so the across weight sharp × strength that the
 * eye validated at ×1.33 (1.85 × 2/3 = 1.23) grows to 1.85 at ×1.5 and
 * stays there at ×2, where the same picture pins to the texels and smears
 * (J39: at ×2 the sharpness 1.25 takes the étalon's centre from 28.8 to
 * 31.0 dB and its FLIP from 0.067 to 0.056, text's FLIP to FSR1's). The law
 * holds that weight from the reference scale up: 1.85 up to ×1.33, 1.542 at
 * ×1.4, 1.23 from ×1.5. From MWSR_SHARP_X2.from it ramps to MWSR_SHARP_X2's
 * sharpness at ×2 and holds it above (1.23 at ×1.75, 1.37 at ×1.875, 1.5
 * from ×2), where 1.23 cost text 0.66 point of SSIM (plan B §8).
 * @param {number} scale output / input, the smaller axis
 * @param {{sharp?: number, ref?: number, x2?: {sharp: number, from: number} | null}} [opts]
 *   the sharpness up to the reference scale (MWSR_UNIFORMS.sharp), that
 *   scale (MWSR_SHARP_REF_SCALE) and the ×2 end (MWSR_SHARP_X2; null: none);
 *   the result is never above sharp
 * @returns {number} the per-draw sharpness at that scale
 */
export function mwsrSharpness(
    scale,
    { sharp = MWSR_UNIFORMS.sharp, ref = MWSR_SHARP_REF_SCALE, x2 = MWSR_SHARP_X2 } = {},
) {
    const strength = (s) => Math.min(Math.max((s - 1) / (MWSR_OVERRIDES.S_FULL - 1), 0), 1);
    const at = strength(scale);
    if (at <= 0) return sharp;
    const held = sharp * Math.min(1, strength(ref) / at);
    if (!x2) return held;
    const t = Math.min(Math.max((scale - x2.from) / (2 - x2.from), 0), 1);
    return Math.min(sharp, held + t * (x2.sharp - held));
}

/**
 * The gaze zone's values (plan §15.2, Bruno's picture of 23/10/2026: a green
 * ring at the centre, about 40 % of the width and most of the height, an
 * orange ring around it at about 70 % of the width, the rest left readable):
 * the centre and the half-axes of the green ellipse as fractions of the
 * picture, the orange ring's width as a fraction of those half-axes (the
 * weight falls by a smoothstep from the ellipse to 1 + ring times it), and
 * the vote threshold and the sharpness at the centre, where the eye is — the
 * table's values (MWSR_UNIFORMS) stay the border's. Read by the `up` text when
 * its GAZE constant is 1, weighted per draw by the stream's motion (the
 * uniform `gaze`, 0 on a still picture: one quality everywhere). Values of
 * the lab, before Bruno's eye on his iPhone.
 */
export const MWSR_ZONE = Object.freeze({
    cx: 0.5,
    cy: 0.5,
    rx: 0.2,
    ry: 0.45,
    ring: 0.75,
    tau: 0.02,
    sharp: 2.5,
});

/** Number of f32 in the uniform block: texel.xy, scale.xy, tau, sharp, gaze,
 *  pad, then the zone: centre.xy, half-axes.xy, ring, tau, sharp, pad. */
export const MWSR_UNIFORM_FLOATS = 16;

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
    if (s >= 1) return { mode: 'up', taps: 5, rDown: 0, linearLight: 0 };
    const table = srgb || linear ? MWSR_DOWN_TAPS_SRGB : MWSR_DOWN_TAPS;
    const linearLight = srgb ? 0 : linear ? 3 : 1;
    for (const [min, taps, rDown] of table)
        if (s >= min) return { mode: 'down', taps, rDown, linearLight };
    const [, taps, rDown] = table[table.length - 1];
    return { mode: 'down', taps, rDown, linearLight };
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
        { mode: 'up', taps: 5, rDown: 0, linearLight: 0 },
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

/**
 * The uniform block for an input of inW×inH drawn at outW×outH.
 * @param {number} inW
 * @param {number} inH
 * @param {number} outW
 * @param {number} outH
 * @param {{tau?: number, sharp?: number, gaze?: number, zone?: Object}} [u]
 *   `sharp`: mwsrSharpness at this scale when absent (J40);
 *   `gaze`: the gaze zone's weight, 0 (none, the default) to 1, read by a
 *   text built with GAZE 1 only; `zone`: its values over MWSR_ZONE's.
 * @returns {Float32Array}
 */
export function mwsrUniforms(inW, inH, outW, outH, u = {}) {
    const tau = u.tau ?? MWSR_UNIFORMS.tau;
    // the sharpness law by the scale unless the caller names one (J40)
    const sharp = u.sharp ?? mwsrSharpness(Math.min(outW / inW, outH / inH));
    const gaze = Math.min(Math.max(u.gaze ?? 0, 0), 1);
    const z = { ...MWSR_ZONE, ...(u.zone || {}) };
    // a ring of no width would put the smoothstep's two edges together
    z.ring = Math.max(z.ring, 1e-3);
    return new Float32Array([
        1 / inW,
        1 / inH,
        outW / inW,
        outH / inH,
        tau,
        sharp,
        gaze,
        0,
        z.cx,
        z.cy,
        z.rx,
        z.ry,
        z.ring,
        z.tau,
        z.sharp,
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
}
fn L(c : vec2i) -> vec3f {
    return textureLoad(tex, c, 0).rgb;
}`,
    },
    ext: {
        decl: '@group(0) @binding(1) var tex : texture_external;',
        sample: `fn S(uv : vec2f) -> vec3f {
    return textureSampleBaseClampToEdge(tex, samp, uv).rgb;
}
fn L(c : vec2i) -> vec3f {
    return textureLoad(tex, c).rgb;
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
        ? 'alias hf = f16;\nalias h3 = vec3<f16>;\nconst EPS_H : hf = 0.001h;'
        : 'alias hf = f32;\nalias h3 = vec3<f32>;\nconst EPS_H : hf = 0.00001;';
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
    tau : f32,
    sharp : f32,
    gaze : f32,
    pad : f32,
    zc : vec2f,
    zr : vec2f,
    zring : f32,
    ztau : f32,
    zsharp : f32,
    pad2 : f32,
};

@group(0) @binding(0) var samp : sampler;
${f.decl}
@group(0) @binding(2) var<uniform> u : U;${map ? '\n' + MAP_DECL : ''}

${f.sample}

${aliases}

${overrides}

const LUMA : vec3f = vec3f(${wgslNumber('f32', lr)}, ${wgslNumber('f32', lg)}, ${wgslNumber('f32', lb)});

fn luma(c : vec3f) -> f32 {
    return dot(c, LUMA);
}

fn lumah(c : h3) -> hf {
    return dot(c, h3(LUMA));
}
`;
}

// The `up` fragment stage: the kernel of plan §3.2.
const UP_FS = `
// Five samples blended in the frame of an edge: the centre, a pair along it
// (weight wT each, smoothing) and a pair across it (weight wX each, negative:
// sharpening), per channel, the result bounded by the five (DERING 0, 1).
fn blend(C : h3, T1 : h3, T2 : h3, X1 : h3, X2 : h3, wT : h3, wX : h3) -> h3 {
    let R = (C + wT * (T1 + T2) + wX * (X1 + X2)) / (1.0 + 2.0 * (wT + wX));
    if (DERING == 2u) {
        // The texels alone bound it (below): on a line one texel wide, the
        // centre sample is the extremum of the five, and this bound would pin
        // the pixel at its bilinear mix, paler than the line.
        return R;
    }
    let lo = min(min(C, T1), min(T2, min(X1, X2)));
    let hi = max(max(C, T1), max(T2, max(X1, X2)));
    return clamp(R, lo, hi);
}

// A step (|X2 - X1|) or a ridge (|X1 + X2 - 2C|) across the edge, against
// all the variation around: 1 on a clean edge or line, near 0 on texture,
// grain and compression blocks.
fn coherence(c : hf, t1 : hf, t2 : hf, x1 : hf, x2 : hf) -> hf {
    let step = abs(x2 - x1);
    let ridge = abs(x1 + x2 - 2.0 * c);
    let variation = abs(x2 - c) + abs(c - x1) + abs(t2 - c) + abs(c - t1) + EPS_H;
    return max(step, ridge) / variation;
}

// The gaze zone's weight at a pixel (GAZE 1, plan §15.2): 1 inside the
// ellipse of centre u.zc and half-axes u.zr, falling by a smoothstep over the
// ring to 0 outside, times the stream's motion u.gaze — 0 on a still
// picture, where the table rules the whole screen.
fn gazeWeight(uv : vec2f) -> f32 {
    if (GAZE != 1u) {
        return 0.0;
    }
    let d = length((uv - u.zc) / u.zr);
    return (1.0 - smoothstep(1.0, 1.0 + u.zring, d)) * u.gaze;
}

// The flat path's picture: the bilinear sample C, or (FLAT_TAPS 5 or 9) the
// cubic of Mitchell and Netravali with B 0 and C FLAT_C (0.5: Catmull-Rom)
// over the 4×4 texels around the position, read as the bilinear samples the
// GPU weighs for free: on each axis the middle two texels by one sample at
// their weighted midpoint, the outer two by one sample each — nine samples,
// or five without the four corners, whose weights are the smallest, the
// rest renormalised. Or (FLAT_TAPS 3) the interpolating quadratic of Dodgson
// (1997) over the 3×3 texels around: on each axis the nearest texel and its
// neighbour on the sample's side by one sample at their weighted midpoint,
// the far one, of negative weight, by one sample — three samples without
// the corner (J34). Bounded by the samples themselves, as the edge path is.
// Inside the gaze zone only when FLAT_ZONE asks, blended by its weight zg.
fn flatPath(uv : vec2f, C : vec3f, zg : f32) -> vec3f {
    if (FLAT_TAPS != 3u && FLAT_TAPS != 5u && FLAT_TAPS != 9u) {
        return C;
    }
    if (GAZE == 1u && FLAT_ZONE == 1u && zg <= 0.0) {
        return C;
    }
    let sp = uv / u.texel;
    var R : vec3f;
    if (FLAT_TAPS == 3u) {
        let tn = floor(sp) + 0.5;
        let h = sp - tn;
        let s = select(vec2f(-1.0), vec2f(1.0), h >= vec2f(0.0));
        let ah = abs(h);
        let wf = ah * ah - 0.5 * ah;
        let wm = 1.0 - wf;
        let wn = ah * ah + 0.5 * ah;
        let pm = (tn + s * wn / wm) * u.texel;
        let pf = (tn - s) * u.texel;
        let sm = S(pm);
        let sfx = S(vec2f(pf.x, pm.y));
        let sfy = S(vec2f(pm.x, pf.y));
        let acc = sm * (wm.x * wm.y) + sfx * (wf.x * wm.y) + sfy * (wm.x * wf.y);
        let norm = wm.x * wm.y + wf.x * wm.y + wm.x * wf.y;
        let lo = min(sm, min(sfx, sfy));
        let hi = max(sm, max(sfx, sfy));
        R = clamp(acc / norm, lo, hi);
    } else {
        let tc = floor(sp - 0.5) + 0.5;
        let f = sp - tc;
        let c = FLAT_C;
        let w0 = -c * f * (1.0 - f) * (1.0 - f);
        let w1 = (2.0 - c) * f * f * f - (3.0 - c) * f * f + 1.0;
        let w3 = -c * (1.0 - f) * f * f;
        let w2 = 1.0 - w0 - w1 - w3;
        let w12 = w1 + w2;
        let p0 = (tc - 1.0) * u.texel;
        let p12 = (tc + w2 / w12) * u.texel;
        let p3 = (tc + 2.0) * u.texel;
        let s12 = S(p12);
        let s02 = S(vec2f(p0.x, p12.y));
        let s32 = S(vec2f(p3.x, p12.y));
        let s20 = S(vec2f(p12.x, p0.y));
        let s23 = S(vec2f(p12.x, p3.y));
        var acc = s12 * (w12.x * w12.y) + s02 * (w0.x * w12.y) + s32 * (w3.x * w12.y)
            + s20 * (w12.x * w0.y) + s23 * (w12.x * w3.y);
        var norm = w12.x * w12.y + w0.x * w12.y + w3.x * w12.y + w12.x * w0.y + w12.x * w3.y;
        var lo = min(min(s12, s02), min(s32, min(s20, s23)));
        var hi = max(max(s12, s02), max(s32, max(s20, s23)));
        if (FLAT_TAPS == 9u) {
            let s00 = S(p0);
            let s30 = S(vec2f(p3.x, p0.y));
            let s03 = S(vec2f(p0.x, p3.y));
            let s33 = S(p3);
            acc += s00 * (w0.x * w0.y) + s30 * (w3.x * w0.y) + s03 * (w0.x * w3.y) + s33 * (w3.x * w3.y);
            norm += w0.x * w0.y + w3.x * w0.y + w0.x * w3.y + w3.x * w3.y;
            lo = min(lo, min(min(s00, s30), min(s03, s33)));
            hi = max(hi, max(max(s00, s30), max(s03, s33)));
        }
        R = clamp(acc / norm, lo, hi);
    }
    if (GAZE == 1u && FLAT_ZONE == 1u) {
        return mix(C, R, zg);
    }
    return R;
}

// The exact bound: the four input texels around the pixel, the footprint of
// its bilinear sample (DERING 1 and 2).
fn texelBound(uv : vec2f, R : vec3f) -> vec3f {
    let dims = vec2i(textureDimensions(tex));
    let pt = uv * vec2f(dims) - 0.5;
    let i0 = clamp(vec2i(floor(pt)), vec2i(0), dims - vec2i(1));
    let i1 = min(i0 + vec2i(1), dims - vec2i(1));
    let q00 = L(i0);
    let q10 = L(vec2i(i1.x, i0.y));
    let q01 = L(vec2i(i0.x, i1.y));
    let q11 = L(i1);
    return clamp(R, min(min(q00, q10), min(q01, q11)), max(max(q00, q10), max(q01, q11)));
}

@fragment
fn fs(in : VSOut) -> @location(0) vec4f {
    let uv = in.uv;
    let C = S(uv);
    let lC = luma(C);
    // The gaze zone's weight, 0 without one: the vote threshold and, on the
    // edge path, the sharpness slide from the table's values to the centre's.
    let zg = gazeWeight(uv);
    let tau = mix(u.tau, u.ztau, zg);
    // The vote: the screen derivatives of the bilinear sample, scaled to a
    // change per input texel; coarse, so the whole quad takes the same branch.
    // From the luma, or (CHROMA444) from the channel with the steepest ramp,
    // so that a border between two colours of one luma is an edge too.
    let dx = dpdxCoarse(C) * u.scale.x;
    let dy = dpdyCoarse(C) * u.scale.y;
    let m = abs(dx) + abs(dy);
    let rg = m.g > m.r;
    let b = m.b > max(m.r, m.g);
    let c444 = CHROMA444 == 1u;
    let gx = select(dot(dx, LUMA), select(select(dx.r, dx.g, rg), dx.b, b), c444);
    let gy = select(dot(dy, LUMA), select(select(dy.r, dy.g, rg), dy.b, b), c444);
    let flat = select(abs(gx) + abs(gy), max(m.r, max(m.g, m.b)), c444) < tau;
    if (SHOW == 1u) {
        return vec4f(vec3f(select(1.0, 0.0, flat)), 1.0);
    }
    if (SHOW == 2u) {
        return vec4f(vec3f(zg), 1.0);
    }
    // Strength: none at 1:1 (the renderer copies the frame there), full from
    // S_FULL on.
    let smin = min(u.scale.x, u.scale.y);
    let strength = clamp((smin - 1.0) / (S_FULL - 1.0), 0.0, 1.0);
    if (flat) {
        return vec4f(flatPath(uv, C, zg), 1.0);
    }
    // The normal of the edge the derivatives measure, and the taps along and
    // across it.
    let n = vec2f(gx, gy) * inverseSqrt(max(gx * gx + gy * gy, 1e-12));
    let dt = vec2f(-n.y, n.x) * u.texel * R_ALONG;
    let dn = n * u.texel * R_ACROSS;
    // From here the arithmetic is hf (f32, or f16 when the module says so).
    let Ch = h3(C);
    let T1 = h3(S(uv - dt));
    let T2 = h3(S(uv + dt));
    let X1 = h3(S(uv - dn));
    let X2 = h3(S(uv + dn));
    let lCh = hf(lC);
    let lT1 = lumah(T1);
    let lT2 = lumah(T2);
    let lX1 = lumah(X1);
    let lX2 = lumah(X2);
    var a : h3;
    if (CHROMA444 == 1u) {
        a = h3(coherence(Ch.r, T1.r, T2.r, X1.r, X2.r),
               coherence(Ch.g, T1.g, T2.g, X1.g, X2.g),
               coherence(Ch.b, T1.b, T2.b, X1.b, X2.b)) * hf(strength);
    } else {
        a = h3(coherence(lCh, lT1, lT2, lX1, lX2) * hf(strength));
    }
    let wT = hf(W_ALONG) * a;
    let sharp = mix(u.sharp, u.zsharp, zg);
    let wX = max(-hf(W_ACROSS) * hf(sharp) * a, h3(hf(W_MIN)));
    var R = vec3f(blend(Ch, T1, T2, X1, X2, wT, wX));
    if (DERING != 0u) {
        R = texelBound(uv, R);
    }
    return vec4f(R, 1.0);
}
`;

// The `up` text that reads the zones (`map`): on the edge path the sharpness
// is the law's, less the zone's drop over the kernel's strength, the drop
// being one of the across weight (sharpness × strength), the bench's unit
// (plan B §12 c). A drop is never negative: the law stays the ceiling. The
// renderer reads the zones from ×1.3 up only, a strength of 0.6 or more; the
// guard keeps any other draw finite.
const SHARP_LAW = '    let sharp = mix(u.sharp, u.zsharp, zg);';
const SHARP_MAP = `    let sharp = mix(u.sharp, u.zsharp, zg)
        - textureSampleLevel(zoneMap, samp, uv, 0.0).r / max(strength, 0.01);`;
const UP_FS_MAP = UP_FS.replace(SHARP_LAW, SHARP_MAP);

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
    // The pixel's footprint in the input, 1 / scale texels per axis, spanned
    // by the grid (R_DOWN 1); ${k}×${k} samples at its ${k}×${k} centres.
    let span = u.texel / u.scale * R_DOWN;
    var acc = vec3f(0.0);
${lines.join('\n')}
    return vec4f(enc(acc * ${wgslNumber('f32', 1 / taps)}), 1.0);
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
 *   the lab). `half` runs the up mode's edge path in f16 (`enable f16`: the
 *   device must have 'shader-f16'; worth 9-10 % on the AMD iGPU, nothing on
 *   NVIDIA); the vote, the frame, the bound and the down mode stay in f32.
 *   `mode` 'down' is the reduction with `taps` samples (1, 4, 9 or 16; see
 *   mwsrRegime()). `map`, the up mode only: the second text, which reads the
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
// line. The colours and weights of the edge path are mediump — f16 where the
// GPU has it, the twin of the `half` module — the coordinates, the
// derivatives, the bound and the result highp.
const UP_GLSL = `
mediump vec3 blend(mediump vec3 C, mediump vec3 T1, mediump vec3 T2, mediump vec3 X1, mediump vec3 X2,
                   mediump vec3 wT, mediump vec3 wX) {
    mediump vec3 R = (C + wT * (T1 + T2) + wX * (X1 + X2)) / (1.0 + 2.0 * (wT + wX));
    if (DERING == 2u) {
        return R;
    }
    mediump vec3 lo = min(min(C, T1), min(T2, min(X1, X2)));
    mediump vec3 hi = max(max(C, T1), max(T2, max(X1, X2)));
    return clamp(R, lo, hi);
}

mediump float coherence(mediump float c, mediump float t1, mediump float t2, mediump float x1,
                        mediump float x2) {
    mediump float jump = abs(x2 - x1);
    mediump float ridge = abs(x1 + x2 - 2.0 * c);
    mediump float variation = abs(x2 - c) + abs(c - x1) + abs(t2 - c) + abs(c - t1) + EPS_H;
    return max(jump, ridge) / variation;
}

// The gaze zone's weight, the WGSL text's gazeWeight line for line.
float gazeWeight(vec2 uv) {
    if (GAZE != 1u) {
        return 0.0;
    }
    float d = length((uv - uZone.xy) / uZone.zw);
    return (1.0 - smoothstep(1.0, 1.0 + uZoneBonus.x, d)) * uTauSharp.z;
}

// The flat path's picture, the WGSL text's flatPath line for line.
vec3 flatPath(vec2 uv, vec3 C, float zg) {
    if (FLAT_TAPS != 3u && FLAT_TAPS != 5u && FLAT_TAPS != 9u) {
        return C;
    }
    if (GAZE == 1u && FLAT_ZONE == 1u && zg <= 0.0) {
        return C;
    }
    vec2 texel = uTexelScale.xy;
    vec2 sp = uv / texel;
    vec3 R;
    if (FLAT_TAPS == 3u) {
        vec2 tn = floor(sp) + 0.5;
        vec2 h = sp - tn;
        vec2 s = mix(vec2(-1.0), vec2(1.0), greaterThanEqual(h, vec2(0.0)));
        vec2 ah = abs(h);
        vec2 wf = ah * ah - 0.5 * ah;
        vec2 wm = 1.0 - wf;
        vec2 wn = ah * ah + 0.5 * ah;
        vec2 pm = (tn + s * wn / wm) * texel;
        vec2 pf = (tn - s) * texel;
        vec3 sm = S(pm);
        vec3 sfx = S(vec2(pf.x, pm.y));
        vec3 sfy = S(vec2(pm.x, pf.y));
        vec3 acc = sm * (wm.x * wm.y) + sfx * (wf.x * wm.y) + sfy * (wm.x * wf.y);
        float norm = wm.x * wm.y + wf.x * wm.y + wm.x * wf.y;
        vec3 lo = min(sm, min(sfx, sfy));
        vec3 hi = max(sm, max(sfx, sfy));
        R = clamp(acc / norm, lo, hi);
    } else {
        vec2 tc = floor(sp - 0.5) + 0.5;
        vec2 f = sp - tc;
        float c = FLAT_C;
        vec2 w0 = -c * f * (1.0 - f) * (1.0 - f);
        vec2 w1 = (2.0 - c) * f * f * f - (3.0 - c) * f * f + 1.0;
        vec2 w3 = -c * (1.0 - f) * f * f;
        vec2 w2 = 1.0 - w0 - w1 - w3;
        vec2 w12 = w1 + w2;
        vec2 p0 = (tc - 1.0) * texel;
        vec2 p12 = (tc + w2 / w12) * texel;
        vec2 p3 = (tc + 2.0) * texel;
        vec3 s12 = S(p12);
        vec3 s02 = S(vec2(p0.x, p12.y));
        vec3 s32 = S(vec2(p3.x, p12.y));
        vec3 s20 = S(vec2(p12.x, p0.y));
        vec3 s23 = S(vec2(p12.x, p3.y));
        vec3 acc = s12 * (w12.x * w12.y) + s02 * (w0.x * w12.y) + s32 * (w3.x * w12.y)
            + s20 * (w12.x * w0.y) + s23 * (w12.x * w3.y);
        float norm = w12.x * w12.y + w0.x * w12.y + w3.x * w12.y + w12.x * w0.y + w12.x * w3.y;
        vec3 lo = min(min(s12, s02), min(s32, min(s20, s23)));
        vec3 hi = max(max(s12, s02), max(s32, max(s20, s23)));
        if (FLAT_TAPS == 9u) {
            vec3 s00 = S(p0);
            vec3 s30 = S(vec2(p3.x, p0.y));
            vec3 s03 = S(vec2(p0.x, p3.y));
            vec3 s33 = S(p3);
            acc += s00 * (w0.x * w0.y) + s30 * (w3.x * w0.y) + s03 * (w0.x * w3.y) + s33 * (w3.x * w3.y);
            norm += w0.x * w0.y + w3.x * w0.y + w0.x * w3.y + w3.x * w3.y;
            lo = min(lo, min(min(s00, s30), min(s03, s33)));
            hi = max(hi, max(max(s00, s30), max(s03, s33)));
        }
        R = clamp(acc / norm, lo, hi);
    }
    if (GAZE == 1u && FLAT_ZONE == 1u) {
        return mix(C, R, zg);
    }
    return R;
}

// The exact bound, the WGSL text's texelBound line for line.
vec3 texelBound(vec2 uv, vec3 R) {
    ivec2 dims = textureSize(uTex, 0);
    vec2 pt = uv * vec2(dims) - 0.5;
    ivec2 i0 = clamp(ivec2(floor(pt)), ivec2(0), dims - 1);
    ivec2 i1 = min(i0 + 1, dims - 1);
    vec3 q00 = L(i0);
    vec3 q10 = L(ivec2(i1.x, i0.y));
    vec3 q01 = L(ivec2(i0.x, i1.y));
    vec3 q11 = L(i1);
    return clamp(R, min(min(q00, q10), min(q01, q11)), max(max(q00, q10), max(q01, q11)));
}

void main() {
    vec2 uv = vUv;
    vec3 C = S(uv);
    float lC = luma(C);
    float zg = gazeWeight(uv);
    float tau = mix(uTauSharp.x, uZoneBonus.y, zg);
    // GL's window y runs up, WebGPU's framebuffer y runs down: dFdy is the
    // WGSL text's dpdy negated, or the edge normal comes out mirrored and the
    // along and across taps swap on every diagonal (the first conformity run).
    vec3 dx = dFdx(C) * uTexelScale.z;
    vec3 dy = -dFdy(C) * uTexelScale.w;
    vec3 m = abs(dx) + abs(dy);
    bool rg = m.g > m.r;
    bool b = m.b > max(m.r, m.g);
    bool c444 = CHROMA444 == 1u;
    float gx = c444 ? (b ? dx.b : (rg ? dx.g : dx.r)) : dot(dx, LUMA);
    float gy = c444 ? (b ? dy.b : (rg ? dy.g : dy.r)) : dot(dy, LUMA);
    bool isFlat = (c444 ? max(m.r, max(m.g, m.b)) : abs(gx) + abs(gy)) < tau;
    if (SHOW == 1u) {
        oColor = vec4(vec3(isFlat ? 0.0 : 1.0), 1.0);
        return;
    }
    if (SHOW == 2u) {
        oColor = vec4(vec3(zg), 1.0);
        return;
    }
    float smin = min(uTexelScale.z, uTexelScale.w);
    float strength = clamp((smin - 1.0) / (S_FULL - 1.0), 0.0, 1.0);
    if (isFlat) {
        oColor = vec4(flatPath(uv, C, zg), 1.0);
        return;
    }
    vec2 n = vec2(gx, gy) * inversesqrt(max(gx * gx + gy * gy, 1e-12));
    vec2 dt = vec2(-n.y, n.x) * uTexelScale.xy * R_ALONG;
    vec2 dn = n * uTexelScale.xy * R_ACROSS;
    mediump vec3 Ch = C;
    mediump vec3 T1 = S(uv - dt);
    mediump vec3 T2 = S(uv + dt);
    mediump vec3 X1 = S(uv - dn);
    mediump vec3 X2 = S(uv + dn);
    mediump float lCh = lC;
    mediump float lT1 = lumah(T1);
    mediump float lT2 = lumah(T2);
    mediump float lX1 = lumah(X1);
    mediump float lX2 = lumah(X2);
    mediump vec3 a;
    if (CHROMA444 == 1u) {
        a = vec3(coherence(Ch.r, T1.r, T2.r, X1.r, X2.r),
                 coherence(Ch.g, T1.g, T2.g, X1.g, X2.g),
                 coherence(Ch.b, T1.b, T2.b, X1.b, X2.b)) * strength;
    } else {
        a = vec3(coherence(lCh, lT1, lT2, lX1, lX2) * strength);
    }
    mediump vec3 wT = W_ALONG * a;
    float sharp = mix(uTauSharp.y, uZoneBonus.z, zg);
    mediump vec3 wX = max(-W_ACROSS * sharp * a, vec3(W_MIN));
    vec3 R = blend(Ch, T1, T2, X1, X2, wT, wX);
    if (DERING != 0u) {
        R = texelBound(uv, R);
    }
    oColor = vec4(R, 1.0);
}
`;

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
    vec2 span = uTexelScale.xy / uTexelScale.zw * R_DOWN;
    vec3 acc = vec3(0.0);
${lines.join('\n')}
    oColor = vec4(enc(acc * ${wgslNumber('f32', 1 / taps)}), 1.0);
}
`;
}

// The part every GLSL text shares: the uniforms (the block of mwsrUniforms()
// in two vec4), the varying and the output of WebGlRenderer's full-screen
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
uniform vec4 uTauSharp; // tau, sharp, the gaze zone's weight, 0: the next four
uniform vec4 uZone; // the gaze zone's centre.xy and half-axes.xy (GAZE 1)
uniform vec4 uZoneBonus; // its ring, and its tau and sharpness at the centre, 0
in vec2 vUv;
out vec4 oColor;

vec3 S(vec2 uv) {
    return textureLod(uTex, uv, 0.0).rgb;
}
vec3 L(ivec2 c) {
    return texelFetch(uTex, c, 0).rgb;
}

const mediump float EPS_H = 0.001;

${consts}

const vec3 LUMA = vec3(${wgslNumber('f32', lr)}, ${wgslNumber('f32', lg)}, ${wgslNumber('f32', lb)});

float luma(vec3 c) {
    return dot(c, LUMA);
}

mediump float lumah(mediump vec3 c) {
    return dot(c, LUMA);
}
`;
}

/**
 * The GLSL ES 3.00 twin of one mode (plan §3.6): the same kernel and the same
 * constants as mwsrWgsl(), from the same tables, for WebGlRenderer's
 * full-screen triangle (`vUv` in, `oColor` out). WebGL2 has no pipeline
 * constants, so the overrides are baked into the text (`overrides`, by name;
 * the rest at their defaults); two vec4 uniforms carry the block of
 * mwsrUniforms(). The edge path's colours and weights are mediump: f16 on the
 * GPUs that have it, the twin of the `half` module, and highp on the others.
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
// block of mwsrUniforms(), the same 16 floats in the same order), the feed,
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
        ? 'typedef min16float hf;\ntypedef min16float3 h3;\nstatic const hf EPS_H = 0.001;'
        : 'typedef float hf;\ntypedef float3 h3;\nstatic const hf EPS_H = 0.00001;';
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
    float tau;
    float sharp;
    float gaze;
    float pad;
    float2 zc;
    float2 zr;
    float zring;
    float ztau;
    float zsharp;
    float pad2;
};

Texture2D<float4> tex : register(MWSR_TEX_REGISTER);
SamplerState samp : register(MWSR_SAMPLER_REGISTER);

float3 S(float2 uv) {
    return tex.SampleLevel(samp, uv, 0.0).rgb;
}
float3 L(int2 c) {
    return tex.Load(int3(c, 0)).rgb;
}

${types}

${consts}

static const float3 LUMA = float3(${wgslNumber('f32', lr)}, ${wgslNumber('f32', lg)}, ${wgslNumber('f32', lb)});

float luma(float3 c) {
    return dot(c, LUMA);
}

hf lumah(h3 c) {
    return dot(c, (h3)LUMA);
}
`;
}

// The `up` pixel stage in HLSL: the kernel of the WGSL text, line for line.
// Direct3D's screen y runs down as WebGPU's does: the derivatives keep their
// sign. The edge path's colours and weights are hf, min16float in the `half`
// text, the twin of the WGSL f16 module.
const UP_HLSL = `
h3 blend(h3 C, h3 T1, h3 T2, h3 X1, h3 X2, h3 wT, h3 wX) {
    h3 R = (C + wT * (T1 + T2) + wX * (X1 + X2)) / (1.0 + 2.0 * (wT + wX));
    if (DERING == 2u) {
        return R;
    }
    h3 lo = min(min(C, T1), min(T2, min(X1, X2)));
    h3 hi = max(max(C, T1), max(T2, max(X1, X2)));
    return clamp(R, lo, hi);
}

hf coherence(hf c, hf t1, hf t2, hf x1, hf x2) {
    hf jump = abs(x2 - x1);
    hf ridge = abs(x1 + x2 - 2.0 * c);
    hf variation = abs(x2 - c) + abs(c - x1) + abs(t2 - c) + abs(c - t1) + EPS_H;
    return max(jump, ridge) / variation;
}

// The gaze zone's weight, the WGSL text's gazeWeight line for line.
float gazeWeight(float2 uv) {
    if (GAZE != 1u) {
        return 0.0;
    }
    float d = length((uv - zc) / zr);
    return (1.0 - smoothstep(1.0, 1.0 + zring, d)) * gaze;
}

// The flat path's picture, the WGSL text's flatPath line for line.
float3 flatPath(float2 uv, float3 C, float zg) {
    if (FLAT_TAPS != 3u && FLAT_TAPS != 5u && FLAT_TAPS != 9u) {
        return C;
    }
    if (GAZE == 1u && FLAT_ZONE == 1u && zg <= 0.0) {
        return C;
    }
    float2 sp = uv / texel;
    float3 R;
    if (FLAT_TAPS == 3u) {
        float2 tn = floor(sp) + 0.5;
        float2 h = sp - tn;
        float2 s = float2(h.x >= 0.0 ? 1.0 : -1.0, h.y >= 0.0 ? 1.0 : -1.0);
        float2 ah = abs(h);
        float2 wf = ah * ah - 0.5 * ah;
        float2 wm = 1.0 - wf;
        float2 wn = ah * ah + 0.5 * ah;
        float2 pm = (tn + s * wn / wm) * texel;
        float2 pf = (tn - s) * texel;
        float3 sm = S(pm);
        float3 sfx = S(float2(pf.x, pm.y));
        float3 sfy = S(float2(pm.x, pf.y));
        float3 acc = sm * (wm.x * wm.y) + sfx * (wf.x * wm.y) + sfy * (wm.x * wf.y);
        float norm = wm.x * wm.y + wf.x * wm.y + wm.x * wf.y;
        float3 lo = min(sm, min(sfx, sfy));
        float3 hi = max(sm, max(sfx, sfy));
        R = clamp(acc / norm, lo, hi);
    } else {
        float2 tc = floor(sp - 0.5) + 0.5;
        float2 f = sp - tc;
        float c = FLAT_C;
        float2 w0 = -c * f * (1.0 - f) * (1.0 - f);
        float2 w1 = (2.0 - c) * f * f * f - (3.0 - c) * f * f + 1.0;
        float2 w3 = -c * (1.0 - f) * f * f;
        float2 w2 = 1.0 - w0 - w1 - w3;
        float2 w12 = w1 + w2;
        float2 p0 = (tc - 1.0) * texel;
        float2 p12 = (tc + w2 / w12) * texel;
        float2 p3 = (tc + 2.0) * texel;
        float3 s12 = S(p12);
        float3 s02 = S(float2(p0.x, p12.y));
        float3 s32 = S(float2(p3.x, p12.y));
        float3 s20 = S(float2(p12.x, p0.y));
        float3 s23 = S(float2(p12.x, p3.y));
        float3 acc = s12 * (w12.x * w12.y) + s02 * (w0.x * w12.y) + s32 * (w3.x * w12.y)
            + s20 * (w12.x * w0.y) + s23 * (w12.x * w3.y);
        float norm = w12.x * w12.y + w0.x * w12.y + w3.x * w12.y + w12.x * w0.y + w12.x * w3.y;
        float3 lo = min(min(s12, s02), min(s32, min(s20, s23)));
        float3 hi = max(max(s12, s02), max(s32, max(s20, s23)));
        if (FLAT_TAPS == 9u) {
            float3 s00 = S(p0);
            float3 s30 = S(float2(p3.x, p0.y));
            float3 s03 = S(float2(p0.x, p3.y));
            float3 s33 = S(p3);
            acc += s00 * (w0.x * w0.y) + s30 * (w3.x * w0.y) + s03 * (w0.x * w3.y) + s33 * (w3.x * w3.y);
            norm += w0.x * w0.y + w3.x * w0.y + w0.x * w3.y + w3.x * w3.y;
            lo = min(lo, min(min(s00, s30), min(s03, s33)));
            hi = max(hi, max(max(s00, s30), max(s03, s33)));
        }
        R = clamp(acc / norm, lo, hi);
    }
    if (GAZE == 1u && FLAT_ZONE == 1u) {
        return lerp(C, R, zg);
    }
    return R;
}

// The exact bound, the WGSL text's texelBound line for line.
float3 texelBound(float2 uv, float3 R) {
    uint w, h;
    tex.GetDimensions(w, h);
    int2 dims = int2(w, h);
    float2 pt = uv * float2(dims) - 0.5;
    int2 i0 = clamp(int2(floor(pt)), int2(0, 0), dims - 1);
    int2 i1 = min(i0 + 1, dims - 1);
    float3 q00 = L(i0);
    float3 q10 = L(int2(i1.x, i0.y));
    float3 q01 = L(int2(i0.x, i1.y));
    float3 q11 = L(i1);
    return clamp(R, min(min(q00, q10), min(q01, q11)), max(max(q00, q10), max(q01, q11)));
}

float4 MwsrPS(VSOut i) : SV_Target {
    float2 uv = i.uv;
    float3 C = S(uv);
    float lC = luma(C);
    float zg = gazeWeight(uv);
    float t = lerp(tau, ztau, zg);
    float3 dx = ddx_coarse(C) * scale.x;
    float3 dy = ddy_coarse(C) * scale.y;
    float3 m = abs(dx) + abs(dy);
    bool rg = m.g > m.r;
    bool b = m.b > max(m.r, m.g);
    bool c444 = CHROMA444 == 1u;
    float gx = c444 ? (b ? dx.b : (rg ? dx.g : dx.r)) : dot(dx, LUMA);
    float gy = c444 ? (b ? dy.b : (rg ? dy.g : dy.r)) : dot(dy, LUMA);
    bool isFlat = (c444 ? max(m.r, max(m.g, m.b)) : abs(gx) + abs(gy)) < t;
    if (SHOW == 1u) {
        return float4((isFlat ? 0.0 : 1.0).xxx, 1.0);
    }
    if (SHOW == 2u) {
        return float4(zg.xxx, 1.0);
    }
    float smin = min(scale.x, scale.y);
    float strength = clamp((smin - 1.0) / (S_FULL - 1.0), 0.0, 1.0);
    if (isFlat) {
        return float4(flatPath(uv, C, zg), 1.0);
    }
    float2 n = float2(gx, gy) * rsqrt(max(gx * gx + gy * gy, 1e-12));
    float2 dt = float2(-n.y, n.x) * texel * R_ALONG;
    float2 dn = n * texel * R_ACROSS;
    h3 Ch = (h3)C;
    h3 T1 = (h3)S(uv - dt);
    h3 T2 = (h3)S(uv + dt);
    h3 X1 = (h3)S(uv - dn);
    h3 X2 = (h3)S(uv + dn);
    hf lCh = (hf)lC;
    hf lT1 = lumah(T1);
    hf lT2 = lumah(T2);
    hf lX1 = lumah(X1);
    hf lX2 = lumah(X2);
    h3 a;
    if (CHROMA444 == 1u) {
        a = h3(coherence(Ch.r, T1.r, T2.r, X1.r, X2.r),
               coherence(Ch.g, T1.g, T2.g, X1.g, X2.g),
               coherence(Ch.b, T1.b, T2.b, X1.b, X2.b)) * (hf)strength;
    } else {
        a = (coherence(lCh, lT1, lT2, lX1, lX2) * (hf)strength).xxx;
    }
    h3 wT = (hf)W_ALONG * a;
    float sh = lerp(sharp, zsharp, zg);
    h3 wX = max(-(hf)W_ACROSS * (hf)sh * a, ((hf)W_MIN).xxx);
    float3 R = (float3)blend(Ch, T1, T2, X1, X2, wT, wX);
    if (DERING != 0u) {
        R = texelBound(uv, R);
    }
    return float4(R, 1.0);
}
`;

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
    float2 span = texel / scale * R_DOWN;
    float3 acc = 0.0;
${lines.join('\n')}
    return float4(enc(acc * ${wgslNumber('f32', 1 / taps)}), 1.0);
}
`;
}

/**
 * The HLSL twin of one mode, for Direct3D 11 (FXC, ps_5_0) and 12 (DXC,
 * ps_6_0): the same kernel and the same constants as mwsrWgsl(), from the
 * same tables. Entry points MwsrVS (the full-screen triangle, three vertices
 * from SV_VertexID) and MwsrPS. Registers b0 (the 16 floats of
 * mwsrUniforms()), t0 (the input) and s0 (a linear, clamp-to-edge sampler),
 * each a define (MWSR_CB_REGISTER, MWSR_TEX_REGISTER, MWSR_SAMPLER_REGISTER).
 * The constants are defines of their name prefixed MWSR_ (MWSR_DERING,
 * MWSR_R_DOWN...), at their default unless the compiler is handed another.
 * `half` runs the up mode's edge path in min16float, the twin of the WGSL
 * f16 module.
 * @param {{mode?: 'up'|'down', taps?: number, half?: boolean}} [opts]
 * @returns {string}
 */
export function mwsrHlsl({ mode = 'up', taps = 4, half = false } = {}) {
    if (mode === 'up') return hlslPreamble(half) + UP_HLSL;
    if (mode === 'down') return hlslPreamble(false) + downHlsl(taps);
    throw new Error('mwsr: mode ' + mode + '?');
}
