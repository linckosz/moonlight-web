/**
 * WebGlRenderer — gl-mwsr's two modes, the setting's (MwsrShader's
 * MWSR_MODES). 'performance' is the kernel alone, pushed by the mode's force;
 * 'quality' draws the kernel into the intermediate framebuffer and a
 * sharpening pass from there to the canvas, where the stream is scaled up by
 * less than ×2. Driven against a context that draws nothing and records its
 * calls: what matters is where each draw goes, and with which program.
 */
/* global console */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { WebGlRenderer } from '../js/stream/renderers/WebGlRenderer.js';
import { mwsrLaw } from '../js/stream/renderers/MwsrShader.js';

/** A WebGL2 context that records every call; objects it creates carry their kind. */
function fakeGl() {
    const calls = [];
    const sources = new Map();
    const own = {
        isContextLost: () => false,
        getContextAttributes: () => ({}),
        getExtension: () => null,
        createShader: () => ({}),
        shaderSource: (sh, src) => void sources.set(sh, src),
        createProgram: () => ({ kind: 'program', fs: null }),
        attachShader: (prog, sh) => {
            if (sources.get(sh).includes('oColor')) prog.fs = sources.get(sh);
        },
        createFramebuffer: () => {
            calls.push(['createFramebuffer']);
            return { kind: 'fbo' };
        },
        getUniformLocation: (prog, name) => ({ prog, name }),
    };
    const gl = new Proxy(own, {
        get: (t, k) =>
            k in t
                ? t[k]
                : (...args) => {
                      calls.push([k, ...args]);
                      return {};
                  },
    });
    return { gl, calls };
}

/** A renderer drawing a 1280×720 frame into outW×outH (×1.5 unless told otherwise). */
async function renderer(opts, outW = 1920, outH = 1080) {
    const ctx = fakeGl();
    const canvas = { width: 0, height: 0, getContext: () => ctx.gl, addEventListener() {} };
    const r = await WebGlRenderer.create(canvas, {
        videoCodec: 'hevc',
        algo: 'gl-mwsr',
        ...opts,
    });
    r.setOutputSize(outW, outH);
    return { r, ...ctx };
}

/** One draw: for each drawArrays, the framebuffer bound and the program in use. */
async function drawsOf(r, calls) {
    calls.length = 0;
    await r.draw({ displayWidth: 1280, displayHeight: 720, timestamp: 0, close() {} });
    const draws = [];
    let fbo = null;
    let prog = null;
    for (const [name, a, b] of calls) {
        if (name === 'bindFramebuffer') fbo = b;
        else if (name === 'useProgram') prog = a;
        else if (name === 'drawArrays') draws.push({ fbo, prog });
    }
    return draws;
}

/** The last value given to a uniform of that name. */
function uniform(calls, name) {
    const hit = calls.filter(
        ([fn, loc]) => typeof fn === 'string' && fn.startsWith('uniform') && loc && loc.name === name,
    );
    return hit.length ? hit[hit.length - 1][2] : null;
}

describe("WebGlRenderer — gl-mwsr's two modes", () => {
    beforeEach(() => {
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
    });
    afterEach(() => vi.restoreAllMocks());

    it('builds nothing of the pass without the quality mode, and draws by the kernel alone', async () => {
        for (const mwsrMode of [undefined, 'performance', 'nonsense']) {
            const { r, calls } = await renderer({ mwsrMode });
            expect(r._mwsrSharpen, String(mwsrMode)).toBeNull();
            expect(r._fbo).toBeNull();
            const draws = await drawsOf(r, calls);
            expect(draws).toHaveLength(1);
            expect(draws[0].fbo).toBeNull();
            expect(r.lastDraw.path).toBe('gl-mwsr:frame');
        }
    });

    it('pushes the kernel by the mode in performance, and leaves it to the law otherwise', async () => {
        const kernel = async (mwsrMode) => {
            const { r, calls } = await renderer({ mwsrMode });
            await drawsOf(r, calls);
            return [Array.from(uniform(calls, 'uKernel')), Array.from(uniform(calls, 'uBound'))];
        };
        const law = mwsrLaw(1280, 720, 1920, 1080);
        const pushed = mwsrLaw(1280, 720, 1920, 1080, { mode: 'performance' });
        const [plain, plainBound] = await kernel(undefined);
        // uKernel: kc, ft, kg, kwc; uBound: kl first
        expect(plain[2]).toBeCloseTo(law.kg, 6);
        expect(await kernel('quality')).toEqual([plain, plainBound]);
        const [perf, perfBound] = await kernel('performance');
        expect(pushed.kg).toBeGreaterThan(law.kg);
        expect(perf[2]).toBeCloseTo(pushed.kg, 6);
        expect(perfBound[0]).toBeCloseTo(pushed.kl, 6);
    });

    it('draws quality in two passes: the kernel into the framebuffer, then the sharpening into the canvas', async () => {
        const { r, calls } = await renderer({ mwsrMode: 'quality' });
        // built with the kernel's programs: nothing compiles at the first frame
        expect(r._mwsrSharpen.prog.fs).toContain('uniform float uStrength;');
        expect(r._fbo).toEqual({ kind: 'fbo' });

        const draws = await drawsOf(r, calls);
        expect(draws).toHaveLength(2);
        expect(draws[0].fbo).toBe(r._fbo);
        expect(draws[0].prog).toBe(r._mwsr.get('up').prog);
        expect(draws[1].fbo).toBeNull();
        expect(draws[1].prog).toBe(r._mwsrSharpen.prog);
        // the framebuffer's picture is the output's size; the strength, ×1.5's
        expect([r._intermW, r._intermH]).toEqual([1920, 1080]);
        expect(uniform(calls, 'uStrength')).toBe(0.25);
        expect(r.lastDraw.path).toBe('gl-mwsr+sharp:frame');
    });

    it('follows the scale: the pass at ×1.33, none from ×2 on, none for a reduction', async () => {
        const { r, calls } = await renderer({ mwsrMode: 'quality' }, 1706, 960);
        expect(await drawsOf(r, calls)).toHaveLength(2);
        expect(uniform(calls, 'uStrength')).toBe(0.5);

        r.setOutputSize(2560, 1440);
        let draws = await drawsOf(r, calls);
        expect(draws).toHaveLength(1);
        expect(draws[0].fbo).toBeNull();
        expect(draws[0].prog).toBe(r._mwsr.get('up').prog);
        expect(r.lastDraw.path).toBe('gl-mwsr:frame');

        r.setOutputSize(960, 540);
        draws = await drawsOf(r, calls);
        expect(draws).toHaveLength(1);
        expect(draws[0].fbo).toBeNull();
        expect(r.lastDraw.path).toBe('gl-mwsr:frame');

        r.setOutputSize(1920, 1080);
        expect(await drawsOf(r, calls)).toHaveLength(2);
        expect(uniform(calls, 'uStrength')).toBe(0.25);
    });

    it("shares the framebuffer with gl-fsr1's, whichever pass is built first", async () => {
        const { r } = await renderer({ mwsrMode: 'quality' });
        const fbo = r._fbo;
        const tex = r._intermTex;
        expect(r.setAlgo('gl-fsr1')).toBe(true);
        expect(r._fbo).toBe(fbo);
        expect(r._intermTex).toBe(tex);
    });
});
