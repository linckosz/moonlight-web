import { describe, it, expect } from 'vitest';
import {
    MWSR_CONSTANTS,
    MWSR_DOWN_TAPS,
    MWSR_DOWN_TAPS_SRGB,
    MWSR_LAW,
    MWSR_OVERRIDES,
    MWSR_OVERRIDE_SPEC,
    MWSR_UNIFORM_FLOATS,
    MWSR_ZONE,
    mwsrClassOverrides,
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
        expect(tex).toContain('let lC = dot(C, LUMA);');
        expect(tex).toContain('gx = dpdxCoarse(lC) * u.scale.x;');
        expect(tex).toContain('if (CHROMA444 == 1u) {');
        expect(tex).toContain('let dx = dpdxCoarse(C) * u.scale.x;');
        expect(tex).toContain('gx = select(select(dx.r, dx.g, rg), dx.b, b);');
        expect(tex).not.toContain('LUMA_SHARP');
    });

    it('has a half-precision variant for the blend of the up mode only', () => {
        const half = mwsrWgsl({ feed: 'ext', half: true });
        expect(half.startsWith('enable f16;')).toBe(true);
        expect(half).toContain('alias hf = f16;');
        expect(half).toContain('alias h3 = vec3<f16>;');
        expect(tex.startsWith('enable')).toBe(false);
        expect(tex).toContain('alias hf = f32;');
        // the vote and the frame stay in f32 whatever the variant
        for (const text of [tex, half]) {
            expect(text).toContain('gx = dpdxCoarse(lC) * u.scale.x;');
            expect(text).toContain('let dn = vec2f(gx, gy) * inverseSqrt(');
            expect(text).toContain('var R = vec3f((h3(C) + hf(wX) * h3(X1 + X2)) * hf(kB));');
        }
        expect(kernel(half)).toBe(kernel(mwsrWgsl({ feed: 'tex', half: true })));
        // the reduction ignores `half`: its sums stay in f32
        expect(mwsrWgsl({ mode: 'down', half: true })).toBe(down);
    });

    it("has a second up text that takes the native host's zone drop off the law (plan B §12 c)", () => {
        const decl = '@group(0) @binding(3) var zoneMap : texture_2d<f32>;';
        const law =
            '    // From here the arithmetic is hf (f32, or f16 when the module says so).\n';
        const zoned =
            '    wX += W_ACROSS * textureSampleLevel(zoneMap, samp, uv, 0.0).r;\n' +
            '    kB = 1.0 / (1.0 + 2.0 * wX);\n' +
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

    it('reduces with an unrolled grid of 4, 9 or 16 samples over the footprint, or one sample', () => {
        for (const taps of [4, 9, 16]) {
            const text = mwsrWgsl({ mode: 'down', taps });
            expect(text.match(/acc \+= dec\(S\(uv \+ vec2f\(/g)).toHaveLength(taps);
            expect(text).toContain(`enc(acc * ${1 / taps})`);
            expect(text).toContain('let span = u.texel / u.scale * R_DOWN;');
            expect(text).toContain('if (LINEAR_LIGHT == 2u) {');
            expect(text).toContain('fn srgbToLin(');
            expect(text).not.toMatch(/dpd[xy]|fwidth|blend\(/);
        }
        expect(down).toBe(mwsrWgsl({ mode: 'down', taps: 4 }));
        expect(down).toContain('acc += dec(S(uv + vec2f(-0.25, -0.25) * span));');
        expect(down).toContain('acc += dec(S(uv + vec2f(0.25, 0.25) * span));');
        for (const taps of [0, 2, 5, 8, 25])
            expect(() => mwsrWgsl({ mode: 'down', taps })).toThrow(/taps/);
        // one sample through sRGB views: the hardware's bilinear in linear light
        const one = mwsrWgsl({ mode: 'down', taps: 1 });
        expect(one).toContain('return vec4f(enc(dec(S(uv))), 1.0);');
        expect(one).not.toContain('span');
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
            taps: 5,
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
            expect(regs[0]).toEqual({ mode: 'up', taps: 5, rDown: 0, linearLight: 0 });
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
            expect(text).toContain('uniform vec4 uTauFc;');
            expect(text).not.toMatch(/\btexture\(/);
        }
        // the overrides are baked into the text, by name
        const c444 = mwsrGlsl({ overrides: { CHROMA444: 1, DERING: 1 } });
        expect(c444).toContain('const uint CHROMA444 = 1u;');
        expect(c444).toContain('const uint DERING = 1u;');
        expect(c444).toContain('const float W_ACROSS = 0.3;');
        expect(() => mwsrGlsl({ overrides: { NOPE: 1 } })).toThrow(/override/);
        expect(() => mwsrGlsl({ mode: 'side' })).toThrow(/mode/);
        // the same kernel: derivatives first, explicit level and texel fetches, the bound
        const main = glsl.slice(glsl.indexOf('void main()'));
        expect(main.indexOf('dFdx(lC)')).toBeGreaterThan(0);
        // GL's window y runs up: the WGSL dpdy is -dFdy here
        expect(main).toContain('gy = -dFdy(lC) * uTexelScale.w;');
        expect(tex).toContain('gy = dpdyCoarse(lC) * u.scale.y;');
        expect(main).toContain('vec3 dy = -dFdy(C) * uTexelScale.w;');
        expect(tex).toContain('let dy = dpdyCoarse(C) * u.scale.y;');
        expect(main.indexOf('dFdx(lC)')).toBeLessThan(main.indexOf('if (isFlat)'));
        expect(main.slice(main.indexOf('if (isFlat)'))).not.toMatch(/dFd[xy]/);
        expect(glsl).toContain('textureLod(uTex, uv, 0.0)');
        expect(glsl).toContain('texelFetch(uTex, c, 0)');
        expect(glsl).toContain('mediump vec3 Xh = X1 + X2;');
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
        expect(mwsrGlsl({ mode: 'down', taps: 1 })).toContain(
            'oColor = vec4(enc(dec(S(uv))), 1.0);',
        );
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
            'tau',
            'fc',
            'wacross',
            'kblend',
            'zc',
            'zr',
            'zring',
            'ztau',
            'zsharp',
            'gaze',
            'sharp',
            'strength',
            'wmin',
            'pad2',
        ]);
        // Direct3D's y runs down as WebGPU's: the derivatives keep their sign
        const ps = hlsl.slice(hlsl.indexOf('float4 MwsrPS'));
        expect(ps).toContain('gy = ddy_coarse(lC) * scale.y;');
        expect(ps).toContain('float3 dy = ddy_coarse(C) * scale.y;');
        expect(ps.indexOf('ddx_coarse(lC)')).toBeGreaterThan(0);
        expect(ps.indexOf('ddx_coarse(lC)')).toBeLessThan(ps.indexOf('if (isFlat)'));
        expect(ps.slice(ps.indexOf('if (isFlat)'))).not.toMatch(/dd[xy]/);
        // the blend in min16float for the half text, and in f32 otherwise
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
        expect(mwsrHlsl({ mode: 'down', taps: 1 })).toContain(
            'return float4(enc(dec(S(uv))), 1.0);',
        );
        expect(hlslDown).toContain('if (LINEAR_LIGHT >= 2u) {\n        return linToSrgb(c);');
        expect(() => mwsrHlsl({ mode: 'side' })).toThrow(/mode/);
    });

    it('packs the uniform block the way the text reads it', () => {
        const u = mwsrUniforms(1280, 720, 2560, 1440);
        expect(u.length).toBe(MWSR_UNIFORM_FLOATS);
        expect(u[0]).toBeCloseTo(1 / 1280, 7);
        expect(u[1]).toBeCloseTo(1 / 720, 7);
        expect(u[2]).toBe(2);
        expect(u[3]).toBe(2);
        expect(u[4]).toBe(1280);
        expect(u[5]).toBe(720);
        // every value not named comes from the law at these sizes, in the block's order
        const law = mwsrLaw(1280, 720, 2560, 1440);
        expect(u[6]).toBeCloseTo(law.racross / 1280, 7);
        expect(u[7]).toBeCloseTo(law.racross / 720, 7);
        expect(u[8]).toBeCloseTo(law.tau, 6);
        expect(u[9]).toBeCloseTo(law.fc, 6);
        // the across weight and the blend's inverse divisor, once per draw
        expect(u[10]).toBeCloseTo(-MWSR_CONSTANTS.W_ACROSS * law.sharp, 6);
        expect(u[11]).toBeCloseTo(1 / (1 + 2 * u[10]), 5);
        expect(u[20]).toBeCloseTo(law.sharp, 6);
        expect(u[21]).toBe(1);
        expect(u[22]).toBeCloseTo(law.wmin, 6);
        const zoned = mwsrLaw(1280, 720, 2560, 1440, { zones: true });
        const uz = mwsrUniforms(1280, 720, 2560, 1440, { zones: true });
        expect(uz[10]).toBeCloseTo(-MWSR_CONSTANTS.W_ACROSS * zoned.sharp, 6);
        expect(uz[6]).toBeCloseTo(zoned.racross / 1280, 7);
        const v = mwsrUniforms(1920, 1080, 2560, 1440, { tau: 0.03, sharp: 0.5 });
        expect(v[2]).toBeCloseTo(4 / 3, 6);
        expect(v[8]).toBeCloseTo(0.03, 6);
        expect(v[20]).toBe(0.5);
        expect(v[10]).toBeCloseTo(-MWSR_CONSTANTS.W_ACROSS * 0.5, 6);
        const d = mwsrUniforms(2560, 1440, 1280, 720);
        expect(d[2]).toBe(0.5);
        expect(d[3]).toBe(0.5);
        expect(d[21]).toBe(0); // a reduction does not read the weight: no strength
        expect(d[11]).toBe(1);
        // the gaze zone (J34): no weight and the defaults unless asked
        expect(MWSR_UNIFORM_FLOATS).toBe(24);
        expect(u[19]).toBe(0);
        for (const [i, k] of [
            [12, 'cx'],
            [13, 'cy'],
            [14, 'rx'],
            [15, 'ry'],
            [16, 'ring'],
            [17, 'tau'],
            [18, 'sharp'],
        ])
            expect(u[i]).toBeCloseTo(MWSR_ZONE[k], 6);
        const z = mwsrUniforms(1920, 1080, 2560, 1440, { gaze: 2, zone: { tau: 0.01, ring: 0 } });
        expect(z[19]).toBe(1); // clamped
        expect(z[17]).toBeCloseTo(0.01, 6);
        expect(z[16]).toBeCloseTo(0.001, 6); // a ring of no width would fold the smoothstep
        expect(z[18]).toBeCloseTo(MWSR_ZONE.sharp, 6);
    });

    it('fades the across weight in from 1:1, to full strength at S_FULL', () => {
        // none at 1:1, half way at half way, full from S_FULL on
        const at = (s, u) => mwsrUniforms(1000, 1000, 1000 * s, 1000 * s, { sharp: 1, ...u });
        expect(at(1)[21]).toBe(0);
        expect(at(1)[10]).toBe(-0);
        expect(at(1)[11]).toBe(1);
        expect(at(1.05)[21]).toBeCloseTo(0.5, 6);
        expect(at(1.05)[10]).toBeCloseTo(-MWSR_CONSTANTS.W_ACROSS * 0.5, 6);
        expect(at(MWSR_CONSTANTS.S_FULL)[21]).toBeCloseTo(1, 6);
        expect(at(2)[21]).toBe(1);
        // a named S_FULL moves it
        expect(at(1.25, { sfull: 1.5 })[21]).toBeCloseTo(0.5, 6);
    });

    it("slides the threshold and the sharpness towards the centre's values in the gaze zone (J34)", () => {
        // the product's texts have no zone
        expect(MWSR_OVERRIDES.GAZE).toBe(0);
        expect(MWSR_OVERRIDES.FLAT_ZONE).toBe(0);
        // the zone: the centre of the picture, a tall ellipse, a ring around it,
        // a lower threshold and more sharpness than the table's
        expect(MWSR_ZONE.cx).toBe(0.5);
        expect(MWSR_ZONE.rx).toBeLessThan(MWSR_ZONE.ry);
        expect(MWSR_ZONE.ring).toBeGreaterThan(0);
        expect(MWSR_ZONE.sharp).toBeGreaterThan(mwsrLaw(1920, 1080, 2560, 1440).sharp);
        for (const text of [tex, glsl]) {
            expect(text).toContain('if (GAZE != 1u) {');
            expect(text).toContain('smoothstep(1.0, 1.0 + ');
            expect(text).toContain('if (SHOW == 2u) {');
            expect(text).toContain('if (GAZE == 1u && FLAT_ZONE == 1u && zg <= 0.0) {');
            expect(text).toContain('return mix(C, R, zg);');
        }
        // the weight before the vote, in uniform control flow; the threshold
        // and the sharpness slide by it, in both twins
        expect(tex).toContain('let zg = gazeWeight(uv);');
        expect(tex).toContain('tau = mix(u.tau, u.ztau, zg);');
        expect(tex).toContain(
            'wX = max(-W_ACROSS * mix(u.sharp, u.zsharp, zg) * u.strength, u.wmin);',
        );
        expect(tex.indexOf('let zg = gazeWeight(uv);')).toBeLessThan(
            tex.indexOf('if (CHROMA444 == 1u) {'),
        );
        expect(glsl).toContain('float zg = gazeWeight(uv);');
        expect(glsl).toContain('tau = mix(uTauFc.x, uZoneBonus.y, zg);');
        expect(glsl).toContain(
            'wX = max(-W_ACROSS * mix(uGaze.x, uZoneBonus.z, zg) * uGaze.y, uGaze.z);',
        );
        expect(glsl).toContain('uniform vec4 uZone;');
        expect(glsl).toContain('uniform vec4 uZoneBonus;');
        const zoned = mwsrGlsl({ overrides: { GAZE: 1, FLAT_ZONE: 1, FLAT_TAPS: 5 } });
        expect(zoned).toContain('const uint GAZE = 1u;');
        expect(zoned).toContain('const uint FLAT_ZONE = 1u;');
    });

    it('reads one texel on the flat path by default, on every device (J56)', () => {
        // Bruno's decision of 02/11/2026: one read, in the three twins, a phone
        // or a tablet too: the class layer lays nothing over the default
        expect(MWSR_OVERRIDES.FLAT_TAPS).toBe(2);
        expect(glsl).toContain('const uint FLAT_TAPS = 2u;');
        expect(mwsrClassOverrides({ handheld: false })).toEqual({});
        expect(mwsrClassOverrides({ handheld: true })).toEqual({});
        for (const text of [tex, mwsrWgsl({ feed: 'tex', half: true }), glsl, hlsl]) {
            expect(text).toContain('if (FLAT_TAPS == 2u) {');
            // the nearest texel, and what the cubic gives on a line one texel wide
            expect(text).toMatch(
                /return C \+ \(1\.0 - f\) \* \(1\.0 - 1\.5 \* f\) \* \(L\([\w.]*\(?tn\)?\) - C\);/,
            );
            // before the older paths, and with no sample of its own
            const branch = text.slice(
                text.indexOf('if (FLAT_TAPS == 2u) {'),
                text.indexOf('if (FLAT_TAPS != 3u'),
            );
            expect(branch).not.toMatch(/\bS\(/);
            expect(branch.match(/\bL\(/g)).toHaveLength(1);
        }
    });

    it('reconstructs the flat path by a cubic of bilinear taps when FLAT_TAPS asks (J33)', () => {
        // the product's default on a computer from 24/10 to 02/11/2026: the
        // Catmull-Rom of five taps in the twins, the quadratic of three on a
        // phone or a tablet; the lab's until phones and the 780M are read
        expect(mwsrLaw(1920, 1080, 2560, 1440).fc).toBeCloseTo(0.5, 6);
        for (const text of [tex, glsl]) {
            expect(text).toContain('if (FLAT_TAPS != 3u && FLAT_TAPS != 5u && FLAT_TAPS != 9u) {');
            expect(text).toContain('if (FLAT_TAPS == 9u) {');
            // the quadratic of three samples (J34): the far texel's weight ≤ 0,
            // the near pair's midpoint, the corner dropped and renormalised
            expect(text).toContain('if (FLAT_TAPS == 3u) {');
            expect(text).toContain('ah * ah - 0.5 * ah;');
            expect(text).toContain('(tn + s * wn / wm)');
            expect(text).toContain('wm.x * wm.y + wf.x * wm.y + wm.x * wf.y;');
            // the flat quad goes through it, the samples bound it
            expect(text).toContain('flatPath(uv, C, zg)');
            expect(text).toContain('R = clamp(acc / norm, lo, hi);');
        }
        // the middle pair of an axis by one sample at its weighted midpoint
        expect(tex).toContain('let p12 = (tc + w2 / w12) * u.texel;');
        expect(glsl).toContain('vec2 p12 = (tc + w2 / w12) * texel;');
        // the twin bakes a chosen reconstruction into its text
        const nine = mwsrGlsl({ overrides: { FLAT_TAPS: 9 } });
        expect(nine).toContain('const uint FLAT_TAPS = 9u;');
        expect(nine).toContain('float c = uTauFc.y;');
        expect(mwsrGlsl({ overrides: { FLAT_TAPS: 3 } })).toContain('const uint FLAT_TAPS = 3u;');
    });

    it('bounds the edge path by the four texels alone, so a thin line keeps its depth (J35; the default since J37)', () => {
        // the product's default since 23/10/2026 (Bruno): the texels alone
        expect(MWSR_OVERRIDES.DERING).toBe(2);
        for (const text of [tex, glsl]) {
            // the samples' own bound is lifted at 2 only, the texels' kept at 1 and 2
            expect(text).toContain('if (DERING != 2u) {');
            expect(text).toContain('if (DERING != 0u) {');
            // the bound is one function (J39)
            expect(text).toContain(
                'return clamp(R, min(min(q00, q10), min(q01, q11)), max(max(q00, q10), max(q01, q11)));',
            );
            expect(text).toContain('R = texelBound(uv, R);');
        }
        expect(mwsrGlsl({ overrides: { DERING: 1 } })).toContain('const uint DERING = 1u;');
        // the sampler's own footprint, at the picture's borders too
        expect(tex).toContain('let i0 = max(f, vec2i(0));');
        expect(glsl).toContain('ivec2 i0 = max(f, ivec2(0));');
        expect(hlsl).toContain('int2 i0 = max(f, int2(0, 0));');
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

    it('keeps the blend divisor above 0 under a named wmin', () => {
        const u = mwsrUniforms(1920, 1080, 2560, 1440, { sharp: 2, wmin: -0.5 });
        expect(u[22]).toBeCloseTo(-0.49, 6);
        expect(u[10]).toBeCloseTo(-0.49, 6);
        expect(u[11]).toBeCloseTo(50, 3);
        // a wmin within it stays as named, and holds the weight
        const v = mwsrUniforms(1920, 1080, 2560, 1440, { sharp: 2, wmin: -0.3 });
        expect(v[22]).toBeCloseTo(-0.3, 6);
        expect(v[10]).toBeCloseTo(-0.3, 6);
        expect(v[11]).toBeCloseTo(2.5, 5);
    });

    it('gives the kernel its values by the size of the stream and of the surface', () => {
        const at = (s, h, opts) =>
            mwsrLaw(1920, 1080, (1920 * s * h) / 1080, (1080 * s * h) / 1080, opts);
        // what the formula reads: the scale's smaller axis, the surface's height at 16:9 for its area
        const coef = MWSR_LAW.plain.sharp;
        const eq = (x, y) =>
            coef[0] +
            coef[1] * x +
            coef[2] * x * x +
            coef[3] * x * x * x +
            coef[4] * x * x * x * x +
            coef[5] * y +
            coef[6] * x * y;
        expect(mwsrLaw(1280, 720, 2560, 1440).sharp).toBeCloseTo(eq(Math.log(2), 0), 12);
        expect(mwsrLaw(1280, 720, 1920, 1080).sharp).toBeCloseTo(
            eq(Math.log(1.5), Math.log(1080 / 1440)),
            12,
        );
        expect(mwsrLaw(1280, 720, 2560, 1000).sharp).toBeCloseTo(
            eq(Math.log(1000 / 720), Math.log(Math.sqrt((2560 * 1000 * 9) / 16) / 1440)),
            12,
        );
        for (const zones of [false, true]) {
            const opts = { zones };
            // the ends of the measures are held: no extrapolation, in a ratio or a surface
            expect(mwsrLaw(1000, 500, 1100, 550, opts)).toEqual(
                mwsrLaw(1000, 500, 1140, 570, opts),
            );
            expect(mwsrLaw(320, 180, 2560, 1440, opts)).toEqual(
                mwsrLaw(640, 360, 2560, 1440, opts),
            );
            expect(mwsrLaw(1280, 720, 7680, 4320, opts)).toEqual(
                mwsrLaw(1280, 720, 3840, 2160, opts),
            );
            expect(mwsrLaw(160, 90, 640, 360, opts)).toEqual(mwsrLaw(480, 270, 1920, 1080, opts));
            // a reduction is held at the lowest scale: it does not read these values
            expect(mwsrLaw(2560, 1440, 1280, 720, opts)).toEqual(
                mwsrLaw(1200, 675, 1280, 720, opts),
            );
            // sane over the whole measured range, on both tables
            for (const s of [1.15, 1.25, 1.333, 1.5, 1.75, 2, 2.25, 2.5, 3]) {
                for (const h of [1080, 1200, 1440, 1600, 2160]) {
                    const l = at(s, h, opts);
                    expect(l.tau, `tau ${s} ${h}`).toBeGreaterThanOrEqual(0);
                    expect(l.sharp, `sharp ${s} ${h}`).toBeGreaterThan(0);
                    expect(l.wmin, `wmin ${s} ${h}`).toBeLessThan(0);
                    // the blend's divisor stays positive at the across weight's most negative
                    const across = Math.max(-MWSR_CONSTANTS.W_ACROSS * l.sharp, l.wmin);
                    expect(1 + 2 * across, `across ${s} ${h}`).toBeGreaterThan(0);
                    expect(l.racross, `racross ${s} ${h}`).toBeGreaterThan(0);
                    expect(l.fc, `fc ${s} ${h}`).toBeGreaterThanOrEqual(0);
                }
            }
        }
        // the kernel ramps from 1:1 to the law's first scale
        expect(MWSR_CONSTANTS.S_FULL).toBeLessThanOrEqual(MWSR_LAW.scale[0]);
    });
});
