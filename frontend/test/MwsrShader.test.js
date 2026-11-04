import { describe, it, expect } from 'vitest';
import {
    MWSR_CONSTANTS,
    MWSR_DOWN_LAW,
    MWSR_DOWN_TAPS,
    MWSR_DOWN_TAPS_SRGB,
    MWSR_LAW,
    MWSR_OVERRIDES,
    MWSR_OVERRIDE_SPEC,
    MWSR_UNIFORM_FLOATS,
    MWSR_ZONE,
    mwsrClassOverrides,
    mwsrDownConstants,
    mwsrDownLaw,
    mwsrGlsl,
    mwsrHlsl,
    mwsrLaw,
    mwsrRegime,
    mwsrRegimeKey,
    mwsrRegimes,
    mwsrUniforms,
    mwsrWgsl,
} from '../js/stream/renderers/MwsrShader.js';

describe('the mwsr shader text', () => {
    const tex = mwsrWgsl({ feed: 'tex' });
    const ext = mwsrWgsl({ feed: 'ext' });
    const down = mwsrWgsl({ mode: 'down' });
    const glsl = mwsrGlsl();
    const glslDown = mwsrGlsl({ mode: 'down' });
    const hlsl = mwsrHlsl();
    const hlslDown = mwsrHlsl({ mode: 'down' });
    const kernel = (s) => s.slice(s.indexOf('override '));

    it('reads either feed with the same kernel', () => {
        expect(tex).toContain('var tex : texture_2d<f32>;');
        expect(tex).toContain('textureSampleLevel(tex, samp, uv, 0.0)');
        expect(ext).toContain('var tex : texture_external;');
        expect(ext).toContain('textureSampleBaseClampToEdge(tex, samp, uv)');
        expect(kernel(tex)).toBe(kernel(ext));
        expect(mwsrWgsl()).toBe(tex);
        expect(() => mwsrWgsl({ feed: 'yuv' })).toThrow(/feed/);
        expect(() => mwsrWgsl({ mode: 'side' })).toThrow(/mode/);
    });

    it('declares every override with its default, in both modes', () => {
        for (const text of [tex, down]) {
            for (const [name, [type, value]] of Object.entries(MWSR_OVERRIDE_SPEC)) {
                const lit =
                    type === 'u32'
                        ? `${value}u`
                        : Number.isInteger(value)
                          ? `${value}.0`
                          : `${value}`;
                expect(text).toContain(`override ${name} : ${type} = ${lit};`);
                expect(MWSR_OVERRIDES[name]).toBe(value);
            }
            expect(text).toContain('const LUMA : vec3f = vec3f(0.25, 0.625, 0.125);');
        }
        expect(MWSR_CONSTANTS.LUMA.reduce((a, b) => a + b, 0)).toBeCloseTo(1, 12);
    });

    it('takes its derivatives before the branch, and samples with an explicit level only', () => {
        const fs = tex.slice(tex.indexOf('@fragment'));
        const vote = fs.indexOf('dpdxCoarse');
        const branch = fs.indexOf('if (flat) {');
        expect(vote).toBeGreaterThan(0);
        expect(branch).toBeGreaterThan(vote);
        expect(fs.slice(branch)).not.toMatch(/dpd[xy]|fwidth/);
        for (const text of [tex, down]) expect(text).not.toMatch(/textureSample\(/);
    });

    it('votes from the luma, or from the steepest channel when CHROMA444 is set', () => {
        expect(tex).toContain('let lM = dot(rM, LUMA);');
        expect(tex).toContain('gM = vec2f(dpdxCoarse(lM), dpdyCoarse(lM)) * u.scale;');
        expect(tex).toContain('if (CHROMA444 == 1u) {');
        expect(tex).toContain('let dx = dpdxCoarse(rM) * u.scale.x;');
        expect(tex).toContain(
            'gM = vec2f(select(select(dx.r, dx.g, rg), dx.b, b), select(select(dy.r, dy.g, rg), dy.b, b));',
        );
        expect(tex).not.toContain('LUMA_SHARP');
    });

    it('has a half-precision variant for the colour arithmetic of the up mode only', () => {
        const half = mwsrWgsl({ feed: 'ext', half: true });
        expect(half.startsWith('enable f16;')).toBe(true);
        expect(half).toContain('alias hf = f16;');
        expect(half).toContain('alias h3 = vec3<f16>;');
        expect(tex.startsWith('enable')).toBe(false);
        expect(tex).toContain('alias hf = f32;');
        // the coordinates and the vote stay in f32 whatever the variant
        for (const text of [tex, half]) {
            expect(text).toContain('gM = vec2f(dpdxCoarse(lM), dpdyCoarse(lM)) * u.scale;');
            expect(text).toContain('let dn = gM * inverseSqrt(dot(gM, gM)) * u.across;');
            expect(text).toContain('let R = K + hf(kg) * D + hf(kw) * (D - h3(dot(D, h3(LUMA))));');
        }
        expect(kernel(half)).toBe(kernel(mwsrWgsl({ feed: 'tex', half: true })));
        // the reduction ignores `half`: its sums stay in f32
        expect(mwsrWgsl({ mode: 'down', half: true })).toBe(down);
    });

    it("has a second up text that takes the native host's zone drop off the sharpness (plan B §12 c)", () => {
        const decl = '@group(0) @binding(3) var zoneMap : texture_2d<f32>;';
        const law =
            '    // From here the arithmetic is hf (f32, or f16 when the module says so).\n';
        const zoned =
            '    let zf = saturate(1.0 - textureSampleLevel(zoneMap, samp, uv, 0.0).r / u.zref);\n' +
            '    kg *= zf;\n' +
            '    kw *= zf;\n' +
            law;
        for (const opts of [{ feed: 'tex' }, { feed: 'ext' }, { feed: 'tex', half: true }]) {
            const base = mwsrWgsl(opts);
            const map = mwsrWgsl({ ...opts, map: true });
            // without the zones, the text of before them, to the byte
            expect(mwsrWgsl({ ...opts, map: false })).toBe(base);
            expect(base).not.toContain('zoneMap');
            // with them: binding 3 under the uniform block, and the one line
            expect(map.split(decl)).toHaveLength(2);
            expect(map).toContain('var<uniform> u : U;\n' + decl + '\n');
            expect(map.split(zoned)).toHaveLength(2);
            expect(map.replace('\n' + decl, '').replace(zoned, law)).toBe(base);
        }
        expect(() => mwsrWgsl({ mode: 'down', map: true })).toThrow(/up mode only/);
    });

    it('gives each reduction only the constants its text reads: WebKit refuses a pipeline otherwise', () => {
        const regs = [
            ...mwsrRegimes({ srgb: true }),
            ...mwsrRegimes({ srgb: false }),
            ...mwsrRegimes({ linear: true }),
        ].filter((r) => r.mode === 'down');
        expect(regs.map((r) => r.taps)).toContain(1);
        for (const reg of regs) {
            const read = mwsrWgsl({ mode: 'down', taps: reg.taps }).replace(/^override .*$/gm, '');
            for (const name of Object.keys(mwsrDownConstants(reg)))
                expect(read, `${name} in the ${reg.taps}-read text`).toMatch(
                    new RegExp(`\\b${name}\\b`),
                );
        }
        expect(mwsrDownConstants({ taps: 1, rDown: 1, linearLight: 3 })).toEqual({
            LINEAR_LIGHT: 3,
        });
        expect(mwsrDownConstants({ taps: 9, rDown: 1.25, linearLight: 0 })).toEqual({
            R_DOWN: 1.25,
            LINEAR_LIGHT: 0,
        });
    });

    it('reduces with an unrolled grid of 4, 9 or 16 samples over the footprint, or one sample', () => {
        for (const taps of [4, 9, 16]) {
            const text = mwsrWgsl({ mode: 'down', taps });
            expect(text.match(/acc \+= dec\(S\(uv \+ vec2f\(/g)).toHaveLength(taps);
            expect(text).toContain(`acc * ${1 / taps}`);
            expect(text).toContain('let span = u.texel / u.scale * R_DOWN;');
            expect(text).toContain('if (LINEAR_LIGHT == 2u) {');
            expect(text).toContain('fn srgbToLin(');
        }
        // below ×0.5 nothing sharpens; from ×0.5 the vote and a cross of 4 reads
        for (const taps of [9, 16]) {
            const text = mwsrWgsl({ mode: 'down', taps });
            expect(text).toContain(`return vec4f(enc(acc * ${1 / taps}), 1.0);`);
            expect(text).not.toMatch(/dpd[xy]|fwidth|blend\(|smoothstep/);
            expect(mwsrGlsl({ mode: 'down', taps })).not.toMatch(/dFd[xy]|smoothstep/);
            expect(mwsrHlsl({ mode: 'down', taps })).not.toMatch(/dd[xy]_coarse|smoothstep/);
        }
        for (const taps of [1, 4]) {
            const wgsl = mwsrWgsl({ mode: 'down', taps });
            expect(wgsl).toContain('let g = vec2f(dpdxCoarse(l), dpdyCoarse(l));');
            expect(wgsl).toContain('if (u.kg == 0.0 || abs(g.x) + abs(g.y) < u.ft) {');
            expect(wgsl).toContain('return vec4f(enc(c), 1.0);');
            expect(wgsl.indexOf('dpdxCoarse')).toBeLessThan(wgsl.indexOf('if (u.kg == 0.0'));
            expect(wgsl.indexOf('if (u.kg == 0.0')).toBeLessThan(wgsl.indexOf('let a = dec(S('));
            expect(wgsl).toContain('let R = c + mix(u.kg, u.kwc, t) * (c - mean);');
            expect(wgsl).toContain('let mg = (mx - mn) * mix(u.kl, u.zref, t);');
            const [r0, r1] = MWSR_DOWN_LAW.range;
            expect(wgsl).toContain(`smoothstep(${r0}, ${r1}, dot(sqrt(max(mx,`);
            const glsl = mwsrGlsl({ mode: 'down', taps });
            expect(glsl).toContain('vec2 g = vec2(dFdx(l), dFdy(l));');
            expect(glsl).toContain('vec3 R = c + mix(uKernel.z, uKernel.w, t) * (c - mean);');
            expect(glsl).toContain('vec3 mg = (mx - mn) * mix(uBound.x, uBound.y, t);');
            const hlsl = mwsrHlsl({ mode: 'down', taps });
            expect(hlsl).toContain('float2 g = float2(ddx_coarse(l), ddy_coarse(l));');
            expect(hlsl).toContain('float3 R = c + lerp(kg, kwc, t) * (c - mean);');
            expect(hlsl).toContain('float3 mg = (mx - mn) * lerp(kl, zref, t);');
        }
        expect(down).toBe(mwsrWgsl({ mode: 'down', taps: 4 }));
        expect(down).toContain('acc += dec(S(uv + vec2f(-0.25, -0.25) * span));');
        expect(down).toContain('acc += dec(S(uv + vec2f(0.25, 0.25) * span));');
        for (const taps of [0, 2, 5, 8, 25])
            expect(() => mwsrWgsl({ mode: 'down', taps })).toThrow(/taps/);
        // one sample through sRGB views: the hardware's bilinear in linear light
        const one = mwsrWgsl({ mode: 'down', taps: 1 });
        expect(one).toContain('let c = dec(S(uv));');
        expect(one).not.toContain('span');
    });

    it('joins the settings of the reduction by a line in the scale', () => {
        const at = (s) => mwsrDownLaw(1000, 1000, 1000 * s, 1000 * s);
        const { gain, edgeGain, margin, edgeMargin, ft } = MWSR_DOWN_LAW;
        expect(at(0.5)).toEqual({
            ft,
            kg: gain[0],
            kwc: edgeGain[0],
            kl: margin[0],
            zref: edgeMargin[0],
        });
        for (const s of [2 / 3, 0.75, 0.9]) {
            const v = at(s);
            expect(v.kg).toBeCloseTo(gain[1], 12);
            expect(v.kwc).toBeCloseTo(edgeGain[1], 12);
            expect(v.kl).toBeCloseTo(margin[1], 12);
            expect(v.zref).toBeCloseTo(edgeMargin[1], 12);
        }
        const mid = at(0.6);
        expect(mid.kg).toBeCloseTo(0.37, 12);
        expect(mid.kwc).toBeCloseTo(0.14, 12);
        expect(mid.kl).toBeCloseTo(0.19, 12);
        expect(mid.zref).toBeCloseTo(0.06, 12);
        // the gains fall to none on the way to 1:1, the margins stay
        const near = at(0.965);
        expect(near.kg).toBeCloseTo(gain[1] / 2, 9);
        expect(near.kwc).toBeCloseTo(edgeGain[1] / 2, 9);
        expect(near.kl).toBeCloseTo(margin[1], 12);
        // below ×0.5, none
        expect(at(0.4)).toEqual({ ft, kg: 0, kwc: 0, kl: 0, zref: 0 });
        // the smaller axis, as the regime
        expect(mwsrDownLaw(1000, 1000, 500, 900).kg).toBe(gain[0]);
    });

    it('picks the mode, the grid, its span and its light by the scale and the views', () => {
        const soft = { linearLight: 1 };
        expect(mwsrRegime(1920, 1080, 1920, 1080)).toEqual({
            mode: 'copy',
            taps: 0,
            rDown: 0,
            linearLight: 0,
        });
        expect(mwsrRegime(1280, 720, 2560, 1440).mode).toBe('up');
        expect(mwsrRegime(1920, 1080, 2560, 1440).mode).toBe('up');
        expect(mwsrRegime(2560, 1440, 1920, 1080)).toEqual({
            mode: 'down',
            taps: 4,
            rDown: 1,
            ...soft,
        });
        expect(mwsrRegime(2560, 1440, 1280, 720)).toEqual({
            mode: 'down',
            taps: 4,
            rDown: 1,
            ...soft,
        });
        expect(mwsrRegime(3840, 2160, 1280, 720)).toEqual({
            mode: 'down',
            taps: 9,
            rDown: 1.25,
            ...soft,
        });
        expect(mwsrRegime(3840, 2160, 640, 360)).toEqual({
            mode: 'down',
            taps: 16,
            rDown: 1.25,
            ...soft,
        });
        // through sRGB views the hardware makes the light: one sample down to ×0.5
        const srgb = { srgb: true };
        const hw = { linearLight: 0 };
        expect(mwsrRegime(1920, 1080, 1920, 1080, srgb).mode).toBe('copy');
        expect(mwsrRegime(1280, 720, 2560, 1440, srgb).mode).toBe('up');
        expect(mwsrRegime(2560, 1440, 1920, 1080, srgb)).toEqual({
            mode: 'down',
            taps: 1,
            rDown: 1,
            ...hw,
        });
        expect(mwsrRegime(2560, 1440, 1280, 720, srgb)).toEqual({
            mode: 'down',
            taps: 1,
            rDown: 1,
            ...hw,
        });
        expect(mwsrRegime(3840, 2160, 1280, 720, srgb)).toEqual({
            mode: 'down',
            taps: 9,
            rDown: 1.25,
            ...hw,
        });
        expect(mwsrRegime(3840, 2160, 640, 360, srgb)).toEqual({
            mode: 'down',
            taps: 16,
            rDown: 1.25,
            ...hw,
        });
        // on a linear feed (the HDR pass 0's) the sampler mixes the light: the
        // same grids, encoded at the output; an upscale keeps the encoded feed
        const lin = { linear: true };
        const sw = { linearLight: 3 };
        expect(mwsrRegime(1280, 720, 2560, 1440, lin)).toEqual({
            mode: 'up',
            taps: 0,
            rDown: 0,
            linearLight: 0,
        });
        expect(mwsrRegime(2560, 1440, 1920, 1080, lin)).toEqual({
            mode: 'down',
            taps: 1,
            rDown: 1,
            ...sw,
        });
        expect(mwsrRegime(2560, 1440, 1280, 720, lin)).toEqual({
            mode: 'down',
            taps: 1,
            rDown: 1,
            ...sw,
        });
        expect(mwsrRegime(3840, 2160, 1280, 720, lin)).toEqual({
            mode: 'down',
            taps: 9,
            rDown: 1.25,
            ...sw,
        });
        expect(mwsrRegime(3840, 2160, 640, 360, lin)).toEqual({
            mode: 'down',
            taps: 16,
            rDown: 1.25,
            ...sw,
        });
        // the smaller axis decides, and a 1.001 × 0.999 box is a reduction
        expect(mwsrRegime(1000, 1000, 1001, 999).mode).toBe('down');
        expect(mwsrRegime(1000, 1000, 1001, 1000).mode).toBe('up');
        for (const table of [MWSR_DOWN_TAPS, MWSR_DOWN_TAPS_SRGB])
            for (const [, taps] of table)
                expect(() => mwsrWgsl({ mode: 'down', taps })).not.toThrow();
        expect(MWSR_OVERRIDES.LINEAR_LIGHT).toBe(1);
    });

    it('lists every regime a renderer builds its pipelines for', () => {
        for (const srgb of [false, true]) {
            const regs = mwsrRegimes({ srgb });
            const table = srgb ? MWSR_DOWN_TAPS_SRGB : MWSR_DOWN_TAPS;
            expect(regs[0]).toEqual({ mode: 'up', taps: 0, rDown: 0, linearLight: 0 });
            expect(regs.slice(1)).toEqual(
                table.map(([, taps, rDown]) => ({
                    mode: 'down',
                    taps,
                    rDown,
                    linearLight: srgb ? 0 : 1,
                })),
            );
            // every answer of mwsrRegime() at scale is one of them
            for (const s of [0.1, 0.2, 0.3, 0.49, 0.5, 0.75, 0.99, 1.33, 2, 3])
                expect(regs).toContainEqual(
                    mwsrRegime(100, 100, Math.round(100 * s), Math.round(100 * s), { srgb }),
                );
            expect(regs).not.toContainEqual(mwsrRegime(100, 100, 100, 100, { srgb }));
        }
        expect(mwsrRegimes()).toEqual(mwsrRegimes({ srgb: false }));
        // a linear feed adds its grids beside the software light's, which a
        // frame the HDR pass 0 did not take keeps
        const both = mwsrRegimes({ linear: true });
        expect(both.slice(0, 4)).toEqual(mwsrRegimes());
        expect(both.slice(4)).toEqual(
            MWSR_DOWN_TAPS_SRGB.map(([, taps, rDown]) => ({
                mode: 'down',
                taps,
                rDown,
                linearLight: 3,
            })),
        );
        for (const s of [0.1, 0.2, 0.3, 0.49, 0.5, 0.75, 0.99, 1.33, 2, 3])
            expect(both).toContainEqual(
                mwsrRegime(100, 100, Math.round(100 * s), Math.round(100 * s), { linear: true }),
            );
        expect(new Set(both.map(mwsrRegimeKey)).size).toBe(both.length);
        // through sRGB views nothing is added
        expect(mwsrRegimes({ srgb: true, linear: true })).toEqual(mwsrRegimes({ srgb: true }));
    });

    it('encodes a linear feed once, at the output (LINEAR_LIGHT 3)', () => {
        const fs = down.slice(down.indexOf('fn dec('));
        // dec leaves 3 alone, enc takes the sRGB transfer for 2 and 3
        expect(fs.slice(0, fs.indexOf('fn enc('))).not.toMatch(/LINEAR_LIGHT == 3u|>= 2u/);
        expect(fs).toContain('if (LINEAR_LIGHT >= 2u) {\n        return linToSrgb(c);');
        expect(mwsrRegimeKey({ mode: 'down', taps: 9, linearLight: 3 })).toBe('down9lin');
        expect(mwsrRegimeKey({ mode: 'down', taps: 9, linearLight: 1 })).toBe('down9');
    });

    it('uses no gather, no discard, no loop, no dynamic indexing (plan §3.4)', () => {
        for (const text of [
            tex,
            mwsrWgsl({ map: true }),
            down,
            mwsrWgsl({ mode: 'down', taps: 16 }),
            glsl,
            glslDown,
            hlsl,
            hlslDown,
        ])
            expect(text).not.toMatch(
                /textureGather|\.Gather|discard|clip\(|\bfor\s*\(|\bloop\b|\bwhile\b|\[\s*i\s*\]/,
            );
    });

    it('has a GLSL ES 3.00 twin written from the same tables (plan §3.6)', () => {
        expect(glsl.startsWith('#version 300 es\n')).toBe(true);
        expect(glslDown.startsWith('#version 300 es\n')).toBe(true);
        for (const text of [glsl, glslDown]) {
            for (const [name, [type, value]] of Object.entries(MWSR_OVERRIDE_SPEC)) {
                const lit =
                    type === 'u32'
                        ? `${value}u`
                        : Number.isInteger(value)
                          ? `${value}.0`
                          : `${value}`;
                expect(text).toContain(
                    `const ${type === 'u32' ? 'uint' : 'float'} ${name} = ${lit};`,
                );
            }
            expect(text).toContain('const vec3 LUMA = vec3(0.25, 0.625, 0.125);');
            expect(text).toContain('uniform vec4 uTexelScale;');
            expect(text).toContain('uniform vec4 uDimsAcross;');
            expect(text).toContain('uniform vec4 uKernel;');
            expect(text).not.toMatch(/\btexture\(/);
        }
        // the overrides are baked into the text, by name
        const c444 = mwsrGlsl({ overrides: { CHROMA444: 1, SHOW: 1 } });
        expect(c444).toContain('const uint CHROMA444 = 1u;');
        expect(c444).toContain('const uint SHOW = 1u;');
        expect(() => mwsrGlsl({ overrides: { NOPE: 1 } })).toThrow(/override/);
        expect(() => mwsrGlsl({ mode: 'side' })).toThrow(/mode/);
        // the same kernel: derivatives first, explicit level and texel fetches, the bound
        const main = glsl.slice(glsl.indexOf('void main()'));
        expect(main.indexOf('dFdx(lM)')).toBeGreaterThan(0);
        // GL's window y runs up: the WGSL dpdy is -dFdy here
        expect(main).toContain('gM = vec2(dFdx(lM) * uTexelScale.z, -dFdy(lM) * uTexelScale.w);');
        expect(main).toContain('vec3 dy = -dFdy(rM) * uTexelScale.w;');
        expect(tex).toContain('let dy = dpdyCoarse(rM) * u.scale.y;');
        expect(main.indexOf('dFdx(lM)')).toBeLessThan(main.indexOf('if (isFlat)'));
        expect(main.slice(main.indexOf('if (isFlat)'))).not.toMatch(/dFd[xy]/);
        expect(glsl).toContain('textureLod(uTex, uv, 0.0)');
        expect(glsl).not.toContain('texelFetch');
        expect(glsl).toContain('mediump vec3 D = K - 0.5 * (hA + hB);');
        // the same grids, from the same offsets
        const offs = (t) =>
            [...t.matchAll(/vec2f?\(([-\d.]+), ([-\d.]+)\) \* span/g)].map(
                (m) => m[1] + ',' + m[2],
            );
        for (const taps of [4, 9, 16]) {
            const w = offs(mwsrWgsl({ mode: 'down', taps }));
            expect(w.length).toBe(taps);
            expect(offs(mwsrGlsl({ mode: 'down', taps }))).toEqual(w);
        }
        expect(mwsrGlsl({ mode: 'down', taps: 1 })).toContain('vec3 c = dec(S(uv));');
        expect(mwsrGlsl({ mode: 'down', overrides: { LINEAR_LIGHT: 0, R_DOWN: 1.25 } })).toContain(
            'const float R_DOWN = 1.25;',
        );
        // the key both renderers keep a regime's pipeline or program under
        expect(mwsrRegimeKey({ mode: 'up', taps: 5 })).toBe('up');
        expect(mwsrRegimeKey({ mode: 'down', taps: 9 })).toBe('down9');
        expect(new Set(mwsrRegimes().map(mwsrRegimeKey)).size).toBe(mwsrRegimes().length);
    });

    it('copies no constant of the kernels it was written next to (plan §10.2)', () => {
        for (const bad of [
            '25.0 / 16.0',
            '2.0 / 5.0',
            '(x - 1.0) * (x - 4.0)',
            '0.55',
            '2.181818',
            '10.14185',
            '0.1875',
            '6.0 / 255.0',
            '8.0 / 255.0',
            '23.0 / 255.0',
        ]) {
            for (const text of [tex, down, glsl, glslDown, hlsl, hlslDown])
                expect(text, bad).not.toContain(bad);
        }
    });

    it('has an HLSL twin written from the same tables, its constants as defines (the mwsr/ folder)', () => {
        for (const text of [hlsl, hlslDown]) {
            for (const [name, [type, value]] of Object.entries(MWSR_OVERRIDE_SPEC)) {
                const lit =
                    type === 'u32'
                        ? `${value}u`
                        : Number.isInteger(value)
                          ? `${value}.0`
                          : `${value}`;
                expect(text).toContain(
                    `#ifndef MWSR_${name}\n#define MWSR_${name} ${lit}\n#endif\n`,
                );
                expect(text).toContain(
                    `static const ${type === 'u32' ? 'uint' : 'float'} ${name} = MWSR_${name};`,
                );
            }
            expect(text).toContain('static const float3 LUMA = float3(0.25, 0.625, 0.125);');
            expect(text).toContain('cbuffer MwsrConstants : register(MWSR_CB_REGISTER) {');
            expect(text).toContain('VSOut MwsrVS(uint vid : SV_VertexID) {');
            expect(text).toContain('float4 MwsrPS(VSOut i) : SV_Target {');
            expect(text).toContain('tex.SampleLevel(samp, uv, 0.0)');
            expect(text).not.toMatch(/\.Sample\(/);
        }
        // the block's 24 floats, in the order of mwsrUniforms()
        const cb = hlsl.slice(hlsl.indexOf('cbuffer'), hlsl.indexOf('};', hlsl.indexOf('cbuffer')));
        expect([...cb.matchAll(/^\s+(float\d?) (\w+);/gm)].map((m) => m[2])).toEqual([
            'texel',
            'scale',
            'dims',
            'across',
            'kc',
            'ft',
            'kg',
            'kwc',
            'zc',
            'zr',
            'zring',
            'zft',
            'zkg',
            'gaze',
            'kl',
            'zref',
            'pad1',
            'pad2',
        ]);
        // Direct3D's y runs down as WebGPU's: the derivatives keep their sign
        const ps = hlsl.slice(hlsl.indexOf('float4 MwsrPS'));
        expect(ps).toContain('gM = float2(ddx_coarse(lM), ddy_coarse(lM)) * scale;');
        expect(ps).toContain('float3 dy = ddy_coarse(rM) * scale.y;');
        expect(ps.indexOf('ddx_coarse(lM)')).toBeGreaterThan(0);
        expect(ps.indexOf('ddx_coarse(lM)')).toBeLessThan(ps.indexOf('if (isFlat)'));
        expect(ps.slice(ps.indexOf('if (isFlat)'))).not.toMatch(/dd[xy]/);
        // the colours in min16float for the half text, and in f32 otherwise
        expect(mwsrHlsl({ half: true })).toContain('typedef min16float3 h3;');
        expect(hlsl).toContain('typedef float3 h3;');
        expect(mwsrHlsl({ mode: 'down', half: true })).toBe(hlslDown);
        // the same grids, from the same offsets, and the WGSL text's light (3 included)
        const offs = (t) =>
            [...t.matchAll(/(?:vec2f?|float2)\(([-\d.]+), ([-\d.]+)\) \* span/g)].map(
                (m) => m[1] + ',' + m[2],
            );
        for (const taps of [4, 9, 16]) {
            const w = offs(mwsrWgsl({ mode: 'down', taps }));
            expect(offs(mwsrHlsl({ mode: 'down', taps }))).toEqual(w);
        }
        expect(mwsrHlsl({ mode: 'down', taps: 1 })).toContain('float3 c = dec(S(uv));');
        expect(hlslDown).toContain('if (LINEAR_LIGHT >= 2u) {\n        return linToSrgb(c);');
        expect(() => mwsrHlsl({ mode: 'side' })).toThrow(/mode/);
    });

    it('packs the uniform block the way the text reads it', () => {
        const u = mwsrUniforms(1280, 720, 2560, 1440);
        expect(u.length).toBe(MWSR_UNIFORM_FLOATS);
        expect(MWSR_UNIFORM_FLOATS).toBe(24);
        expect(u[0]).toBeCloseTo(1 / 1280, 7);
        expect(u[1]).toBeCloseTo(1 / 720, 7);
        expect(u[2]).toBe(2);
        expect(u[3]).toBe(2);
        expect(u[4]).toBe(1280);
        expect(u[5]).toBe(720);
        // every value not named comes from the law at these sizes, in the block's order
        const law = mwsrLaw(1280, 720, 2560, 1440);
        expect(u[6]).toBeCloseTo(law.kr / 1280, 7);
        expect(u[7]).toBeCloseTo(law.kr / 720, 7);
        expect(u[8]).toBeCloseTo(law.kc, 6);
        expect(u[9]).toBeCloseTo(law.ft, 6);
        expect(u[10]).toBeCloseTo(law.kg, 6);
        expect(u[11]).toBeCloseTo(law.kwc, 6);
        expect(u[20]).toBeCloseTo(law.kl, 6);
        expect(u[21]).toBe(1);
        const v = mwsrUniforms(1920, 1080, 2560, 1440, { ft: 0.03, kg: 0.5, kl: 0.2 });
        expect(v[2]).toBeCloseTo(4 / 3, 6);
        expect(v[9]).toBeCloseTo(0.03, 6);
        expect(v[10]).toBe(0.5);
        expect(v[20]).toBeCloseTo(0.2, 6);
        // a reduction's own values, in the same places
        const d = mwsrUniforms(2560, 1440, 1280, 720);
        expect(d[2]).toBe(0.5);
        expect(d[3]).toBe(0.5);
        expect(d[9]).toBeCloseTo(0.003, 6);
        expect(d[10]).toBeCloseTo(0.25, 6);
        expect(d[11]).toBeCloseTo(0.05, 6);
        expect(d[20]).toBeCloseTo(0.1, 6);
        expect(d[21]).toBe(0);
        const third = mwsrUniforms(3840, 2160, 2560, 1440);
        expect(third[10]).toBeCloseTo(0.45, 6);
        expect(third[11]).toBeCloseTo(0.2, 6);
        expect(third[20]).toBeCloseTo(0.25, 6);
        expect(third[21]).toBeCloseTo(0.1, 6);
        // an HDR feed does not sharpen on the way down
        const hdrDown = mwsrUniforms(3840, 2160, 2560, 1440, { hdr: true });
        expect(hdrDown[10]).toBe(0);
        expect(hdrDown[11]).toBe(0);
        // the edge's direction divides by the slope: a vote of 0 stays above 0
        expect(mwsrUniforms(1920, 1080, 2560, 1440, { ft: 0 })[9]).toBeGreaterThan(0);
        // the gaze zone (J34): no weight and the defaults unless asked
        expect(u[19]).toBe(0);
        for (const [i, k] of [
            [12, 'cx'],
            [13, 'cy'],
            [14, 'rx'],
            [15, 'ry'],
            [16, 'ring'],
            [17, 'ft'],
        ])
            expect(u[i]).toBeCloseTo(MWSR_ZONE[k], 6);
        expect(u[18]).toBeCloseTo(law.kg * MWSR_ZONE.boost, 5);
        const z = mwsrUniforms(1920, 1080, 2560, 1440, { gaze: 2, zone: { ft: 0.01, ring: 0 } });
        expect(z[19]).toBe(1); // clamped
        expect(z[17]).toBeCloseTo(0.01, 6);
        expect(z[16]).toBeCloseTo(0.001, 6); // a ring of no width would fold the smoothstep
        // an HDR feed: no margin on the bound, the rest as the law
        const h = mwsrUniforms(1280, 720, 2560, 1440, { hdr: true });
        expect(h[20]).toBe(0);
        expect(h[10]).toBeCloseTo(law.kg, 6);
        // the frames with the host's zones count their drops against a positive sharpness
        expect(mwsrUniforms(1280, 720, 2560, 1440, { zones: true })[21]).toBeGreaterThan(0.1);
    });

    it('fades the sharpening in from 1:1, to the whole of it at S_FULL', () => {
        // none at 1:1, half way at half way, whole from S_FULL on
        const at = (s, u) =>
            mwsrUniforms(1000, 1000, 1000 * s, 1000 * s, { kg: 1, kwc: 0.1, ...u });
        expect(at(1)[10]).toBe(0);
        expect(at(1)[11]).toBe(0);
        const half = 1 + (MWSR_CONSTANTS.S_FULL - 1) / 2;
        expect(at(half)[10]).toBeCloseTo(0.5, 5);
        expect(at(half)[11]).toBeCloseTo(0.05, 5);
        expect(at(MWSR_CONSTANTS.S_FULL)[10]).toBeCloseTo(1, 5);
        expect(at(2)[10]).toBe(1);
        // a named S_FULL moves it
        expect(at(1.25, { sfull: 1.5 })[10]).toBeCloseTo(0.5, 6);
    });

    it("slides the threshold and the sharpness towards the centre's values in the gaze zone (J34)", () => {
        // the product's texts have no zone
        expect(MWSR_OVERRIDES.GAZE).toBe(0);
        // the zone: the centre of the picture, a tall ellipse, a ring around it,
        // more sharpness than the law's
        expect(MWSR_ZONE.cx).toBe(0.5);
        expect(MWSR_ZONE.rx).toBeLessThan(MWSR_ZONE.ry);
        expect(MWSR_ZONE.ring).toBeGreaterThan(0);
        expect(MWSR_ZONE.boost).toBeGreaterThan(1);
        for (const text of [tex, glsl, hlsl]) {
            expect(text).toContain('if (GAZE != 1u) {');
            expect(text).toContain('smoothstep(1.0, 1.0 + ');
            expect(text).toContain('if (SHOW == 2u) {');
        }
        // the weight before the vote, in uniform control flow; the threshold
        // and the sharpness slide by it, in both twins
        expect(tex).toContain('let zg = gazeWeight(uv);');
        expect(tex).toContain('ft = mix(u.ft, u.zft, zg);');
        expect(tex).toContain('kg = mix(u.kg, u.zkg, zg);');
        expect(tex.indexOf('let zg = gazeWeight(uv);')).toBeLessThan(
            tex.indexOf('if (CHROMA444 == 1u) {'),
        );
        expect(glsl).toContain('float zg = gazeWeight(uv);');
        expect(glsl).toContain('ft = mix(uKernel.y, uZoneBonus.y, zg);');
        expect(glsl).toContain('kg = mix(uKernel.z, uZoneBonus.z, zg);');
        expect(glsl).toContain('uniform vec4 uZone;');
        expect(glsl).toContain('uniform vec4 uZoneBonus;');
        expect(mwsrGlsl({ overrides: { GAZE: 1 } })).toContain('const uint GAZE = 1u;');
    });

    it('keeps one read on a flat quad, seven on an edge, on every device', () => {
        expect(mwsrClassOverrides({ handheld: false })).toEqual({});
        expect(mwsrClassOverrides({ handheld: true })).toEqual({});
        const branch = (text, a) => text.slice(0, text.indexOf(a));
        for (const [text, flat] of [
            [tex, 'if (flat) {'],
            [mwsrWgsl({ feed: 'tex', half: true }), 'if (flat) {'],
            [glsl, 'if (isFlat) {'],
            [hlsl, 'if (isFlat) {'],
        ]) {
            const main = text.slice(text.search(/fn fs\(|void main\(\)|float4 MwsrPS/));
            // one sample before the branch, the middle read, and the branch returns it
            expect(branch(main, flat).match(/\bS\(/g)).toHaveLength(1);
            // six after it: the outer lobes and the pair across the edge
            expect(main.slice(main.indexOf(flat)).match(/\bS\(/g)).toHaveLength(6);
        }
    });

    it('reads the cubic by bilinear samples, its weights summing to one', () => {
        // the weights the text multiplies out, at any phase and any kc
        for (const kc of [0, 0.5, 0.65, 0.9]) {
            for (const t of [0, 0.1, 0.5, 0.9, 1]) {
                const s = 1 - t;
                const ku = kc * t * s;
                const w0 = -ku * s;
                const w3 = -ku * t;
                const w12 = 1 + ku;
                expect(w0 + w12 + w3).toBeCloseTo(1, 12);
                // the middle pair's second weight over the pair, what p12 reads
                const w2 = t * t * (3 - 2 * t) + ku * s;
                expect(w2 / w12).toBeGreaterThanOrEqual(0);
                expect(w2 / w12).toBeLessThanOrEqual(1 + 1e-12);
                // at the texels the cubic is the texel
                if (t === 0) expect(w2 / w12).toBe(0);
                if (t === 1) expect(w2 / w12).toBeCloseTo(1, 12);
            }
        }
        // the five reads' weights over the plane sum to 1 - kux kuy, the text's divisor
        const [kux, kuy] = [0.1, 0.2];
        const [w12x, w12y] = [1 + kux, 1 + kuy];
        expect(w12x * (w12y - kuy) - w12y * kux).toBeCloseTo(1 - kux * kuy, 12);
        expect(tex).toContain('(hf(1.0) - hu.x * hu.y)');
        expect(glsl).toContain('(1.0 - hu.x * hu.y)');
        expect(hlsl).toContain('((hf)1.0 - hux * huy)');
    });

    it('bounds the result by the three reads, widened by kl, and never below zero', () => {
        for (const text of [tex, mwsrWgsl({ feed: 'tex', half: true })]) {
            expect(text).toContain('let mn = min(hM, min(hA, hB));');
            expect(text).toContain('let mg = (mx - mn) * hf(u.kl);');
            expect(text).toContain('clamp(R, max(mn - mg, h3(hf(0.0))), mx + mg)');
        }
        expect(glsl).toContain('clamp(R, max(mn - mg, vec3(0.0)), mx + mg)');
        expect(hlsl).toContain('clamp(R, max(mn - mg, (h3)(hf)0.0), mx + mg)');
    });

    it('has no ×2 law left, in the three twins (A29: neutral by default, never used)', () => {
        // X2_BASE, X2_FLAT and X2_R_ACROSS, and S_X2 that weighed them in, are gone
        for (const k of ['S_X2', 'X2_BASE', 'X2_FLAT', 'X2_R_ACROSS'])
            expect(MWSR_OVERRIDES).not.toHaveProperty(k);
        for (const text of [tex, mwsrWgsl({ feed: 'tex', half: true }), glsl, hlsl]) {
            expect(text).not.toMatch(/X2_|S_X2|\bx2 = /);
            // the across pair at the block's distance
            expect(text).toMatch(/ \* (?:u\.across|uDimsAcross\.zw|across);/);
        }
    });

    it('has no along taps and no coherence left, in the three twins (J56: walong and ralong 0 in the law)', () => {
        for (const text of [tex, mwsrWgsl({ feed: 'tex', half: true }), glsl, hlsl]) {
            expect(text).not.toMatch(/walong|ralong|\bT[12]\b|coherence|EPS_H|lumah|\bdt\b/);
            // three samples on an edge: the centre and the pair across it
            expect(text.match(/\bS\(uv [-+] dn\)/g)).toHaveLength(2);
        }
    });

    it('gives the kernel its values by the scale, the force of the eye over the fit', () => {
        const at = (s) => mwsrLaw(1000, 1000, 1000 * s, 1000 * s);
        const { curves, force, ft, kwc } = MWSR_LAW;
        const fit = (c, s) => {
            const x = Math.log(s);
            return c[0] + c[1] * x + c[2] * x * x;
        };
        // up to ×1.33, the fit
        expect(at(4 / 3).kg).toBeCloseTo(fit(curves.kg, 4 / 3), 12);
        expect(at(1.2).kl).toBeCloseTo(fit(curves.kl, 1.2), 12);
        // Bruno's two marks and his ×1.5, to the digit of the comparators' values
        expect(at(4 / 3).kg).toBeCloseTo(0.51, 2);
        expect(at(4 / 3).kl).toBeCloseTo(0.256, 3);
        expect(at(2).kg).toBeCloseTo(1.427, 3);
        expect(at(2).kl).toBeCloseTo(0.23, 3);
        expect(at(1.5).kg).toBeCloseTo(0.783, 2);
        expect(at(1.5).kl).toBeCloseTo(0.251, 3);
        // the force is a straight line in ln(s) between its ends, and only on kg and kl
        const mid = Math.sqrt(force.from * force.to);
        expect(at(mid).kg).toBeCloseTo(fit(curves.kg, mid) * (1 + (force.top - 1) / 2), 10);
        expect(at(mid).kl).toBeCloseTo(fit(curves.kl, mid) * (1 + (force.top - 1) / 2), 10);
        expect(at(mid).kc).toBeCloseTo(fit(curves.kc, mid), 12);
        expect(at(mid).kr).toBeCloseTo(fit(curves.kr, mid), 12);
        // beyond ×2 it stays: nothing higher was seen
        expect(at(3).kg).toBeCloseTo(fit(curves.kg, 3) * force.top, 10);
        // the ends of the measures are held: no extrapolation
        expect(at(1.02)).toEqual(at(MWSR_LAW.scale[0]));
        expect(at(9)).toEqual(at(MWSR_LAW.scale[1]));
        // a reduction is held at the lowest scale: it does not read these values
        expect(mwsrLaw(2560, 1440, 1280, 720)).toEqual(at(MWSR_LAW.scale[0]));
        // the smaller axis decides; the surface's size does not enter
        expect(mwsrLaw(1280, 720, 2560, 1000)).toEqual(at(1000 / 720));
        expect(mwsrLaw(1920, 1080, 3840, 2160)).toEqual(mwsrLaw(1280, 720, 2560, 1440));
        // sane over the whole measured range
        for (const s of [1.07, 1.15, 1.25, 4 / 3, 1.5, 1.75, 2, 2.5, 3, 4]) {
            const l = at(s);
            expect(l.ft, `ft ${s}`).toBeGreaterThan(0);
            expect(l.kc, `kc ${s}`).toBeGreaterThan(0.5);
            expect(l.kc, `kc ${s}`).toBeLessThan(1);
            expect(l.kg, `kg ${s}`).toBeGreaterThanOrEqual(0);
            expect(l.kg, `kg ${s}`).toBeLessThan(2);
            expect(l.kr, `kr ${s}`).toBeGreaterThan(0.4);
            expect(l.kr, `kr ${s}`).toBeLessThan(1.1);
            expect(l.kl, `kl ${s}`).toBeGreaterThan(0);
            expect(l.kl, `kl ${s}`).toBeLessThan(0.5);
            expect(l.kwc).toBe(kwc);
            expect(l.ft).toBe(ft);
        }
        // the kernel ramps from 1:1 to the law's first scale
        expect(MWSR_CONSTANTS.S_FULL).toBeLessThanOrEqual(MWSR_LAW.scale[0]);
    });
});
