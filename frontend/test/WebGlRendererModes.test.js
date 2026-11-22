/**
 * WebGlRenderer — mwsr's two modes (MwsrShader's MWSR_MODES), each a pass of
 * its own name. 'gl-mwsr' is the kernel alone, pushed by the `performance`
 * mode's force in the product's sessions (opts.mwsrMode); 'gl-mwsr-q' is
 * `quality`: the kernel into the intermediate framebuffer and a sharpening
 * pass from there to the canvas, where the stream is scaled up by less than
 * ×2. Driven against a context that draws nothing and records its calls: what
 * matters is where each draw goes, and with which program.
 */
/* global console */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { WebGlRenderer, WEBGL_ALGOS } from '../js/stream/renderers/WebGlRenderer.js';
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
        createProgram: () => {
            calls.push(['createProgram']);
            return { kind: 'program', fs: null };
        },
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
        ([fn, loc]) =>
            typeof fn === 'string' && fn.startsWith('uniform') && loc && loc.name === name,
    );
    return hit.length ? hit[hit.length - 1][2] : null;
}

const programsBuilt = (calls) => calls.filter(([fn]) => fn === 'createProgram').length;

describe("WebGlRenderer — mwsr's two modes", () => {
    beforeEach(() => {
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
    });
    afterEach(() => vi.restoreAllMocks());

    it('names both passes, quality above the kernel alone', () => {
        expect(WEBGL_ALGOS).toContain('gl-mwsr-q');
        expect(WEBGL_ALGOS.indexOf('gl-mwsr-q')).toBeLessThan(WEBGL_ALGOS.indexOf('gl-mwsr'));
    });

    it('builds nothing of the pass under gl-mwsr, and draws by the kernel alone', async () => {
        for (const mwsrMode of [undefined, 'performance', 'quality']) {
            const { r, calls } = await renderer({ mwsrMode });
            expect(r._mwsrSharpen, String(mwsrMode)).toBeNull();
            expect(r._fbo).toBeNull();
            const draws = await drawsOf(r, calls);
            expect(draws).toHaveLength(1);
            expect(draws[0].fbo).toBeNull();
            expect(r.lastDraw.path).toBe('gl-mwsr:frame');
            // the mode comes from the pass and the session, never from that word alone
            expect(r.mwsrMode).toBe(mwsrMode === 'performance' ? 'performance' : null);
        }
    });

    it('pushes the kernel by the performance mode, the default, and draws it at the force 1 under the quality pass', async () => {
        const kernel = async (opts) => {
            const { r, calls } = await renderer(opts);
            await drawsOf(r, calls);
            return [Array.from(uniform(calls, 'uKernel')), Array.from(uniform(calls, 'uBound'))];
        };
        const q = mwsrLaw(1280, 720, 1920, 1080, { mode: 'quality' });
        const pushed = mwsrLaw(1280, 720, 1920, 1080, { mode: 'performance' });
        // the laboratory, without the product's option: the default, performance.
        // uKernel: kc, ft, kg, kwc; uBound: kl first
        const [plain, plainBound] = await kernel({});
        expect(plain[2]).toBeCloseTo(pushed.kg, 6);
        expect(plainBound[0]).toBeCloseTo(pushed.kl, 6);
        expect(await kernel({ mwsrMode: 'performance' })).toEqual([plain, plainBound]);
        // quality draws the kernel at its own force, in the product's sessions too
        const [quality, qualityBound] = await kernel({ algo: 'gl-mwsr-q' });
        expect(quality[2]).toBeCloseTo(q.kg, 6);
        expect(qualityBound[0]).toBeCloseTo(q.kl, 6);
        expect(await kernel({ algo: 'gl-mwsr-q', mwsrMode: 'performance' })).toEqual([
            quality,
            qualityBound,
        ]);
        expect(pushed.kg).toBeGreaterThan(q.kg);
    });

    it('draws quality in two passes: the kernel into the framebuffer, then the sharpening into the canvas', async () => {
        const { r, calls } = await renderer({ algo: 'gl-mwsr-q' });
        expect(r.mwsrMode).toBe('quality');
        expect(r.algoName).toBe('WebGL MWSR');
        // built with the kernel's programs: nothing compiles at the first frame
        expect(r._mwsrSharpen.prog.fs).toContain('clamp(c + uGain * (c - mean), lo, hi)');
        expect(r._mwsrSharpen.prog.fs).not.toContain('lobe');
        expect(r._fbo).toEqual({ kind: 'fbo' });
        const built = programsBuilt(calls);

        const draws = await drawsOf(r, calls);
        expect(draws).toHaveLength(2);
        expect(draws[0].fbo).toBe(r._fbo);
        expect(draws[0].prog).toBe(r._mwsr.get('up').prog);
        expect(draws[1].fbo).toBeNull();
        expect(draws[1].prog).toBe(r._mwsrSharpen.prog);
        // the framebuffer's picture is the output's size; the gain, ×1.5's
        expect([r._intermW, r._intermH]).toEqual([1920, 1080]);
        expect(uniform(calls, 'uGain')).toBe(0.15);
        expect(r.lastDraw.path).toBe('gl-mwsr-q+sharp:frame');
        expect(programsBuilt(calls)).toBe(0);
        expect(built).toBeGreaterThan(0);
    });

    it('follows the scale: the pass at ×1.33, none from ×2 on, none for a reduction', async () => {
        const { r, calls } = await renderer({ algo: 'gl-mwsr-q' }, 1706, 960);
        expect(await drawsOf(r, calls)).toHaveLength(2);
        expect(uniform(calls, 'uGain')).toBe(0.3);

        r.setOutputSize(2560, 1440);
        let draws = await drawsOf(r, calls);
        expect(draws).toHaveLength(1);
        expect(draws[0].fbo).toBeNull();
        expect(draws[0].prog).toBe(r._mwsr.get('up').prog);
        expect(r.lastDraw.path).toBe('gl-mwsr-q:frame');

        r.setOutputSize(960, 540);
        draws = await drawsOf(r, calls);
        expect(draws).toHaveLength(1);
        expect(draws[0].fbo).toBeNull();
        expect(r.lastDraw.path).toBe('gl-mwsr-q:frame');

        r.setOutputSize(1920, 1080);
        expect(await drawsOf(r, calls)).toHaveLength(2);
        expect(uniform(calls, 'uGain')).toBe(0.15);
    });

    it("steps between the two modes as the ladder does, without rebuilding the kernel's programs", async () => {
        const { r, calls } = await renderer({ algo: 'gl-mwsr-q', mwsrMode: 'performance' });
        // the quality mode's kernel, the force 1
        const law = mwsrLaw(1280, 720, 1920, 1080, { mode: 'quality' });
        const pushed = mwsrLaw(1280, 720, 1920, 1080, { mode: 'performance' });
        expect(await drawsOf(r, calls)).toHaveLength(2);
        expect(Array.from(uniform(calls, 'uKernel'))[2]).toBeCloseTo(law.kg, 6);

        // down: the kernel alone, pushed by the performance mode
        calls.length = 0;
        expect(r.setAlgo('gl-mwsr-q')).toBe(false);
        expect(r.setAlgo('gl-mwsr')).toBe(true);
        expect(programsBuilt(calls)).toBe(0);
        expect(r.mwsrMode).toBe('performance');
        let draws = await drawsOf(r, calls);
        expect(draws).toHaveLength(1);
        expect(draws[0].fbo).toBeNull();
        expect(Array.from(uniform(calls, 'uKernel'))[2]).toBeCloseTo(pushed.kg, 6);
        expect(r.lastDraw.path).toBe('gl-mwsr:frame');

        // and back up: the pass again, the kernel at the force 1
        calls.length = 0;
        expect(r.setAlgo('gl-mwsr-q')).toBe(true);
        expect(programsBuilt(calls)).toBe(0);
        expect(r.mwsrMode).toBe('quality');
        draws = await drawsOf(r, calls);
        expect(draws).toHaveLength(2);
        expect(draws[1].prog).toBe(r._mwsrSharpen.prog);
        expect(Array.from(uniform(calls, 'uKernel'))[2]).toBeCloseTo(law.kg, 6);
    });

    it('builds the pass at the first step up to quality when the session started below it', async () => {
        const { r, calls } = await renderer({ mwsrMode: 'performance' });
        expect(r._mwsrSharpen).toBeNull();
        calls.length = 0;
        expect(r.setAlgo('gl-mwsr-q')).toBe(true);
        // the pass's program alone: the kernel's stay
        expect(programsBuilt(calls)).toBe(1);
        expect(r._mwsrSharpen).not.toBeNull();
        expect(await drawsOf(r, calls)).toHaveLength(2);
    });

    it("shares the framebuffer with gl-fsr1's, whichever pass is built first", async () => {
        const { r } = await renderer({ algo: 'gl-mwsr-q' });
        const fbo = r._fbo;
        const tex = r._intermTex;
        expect(r.setAlgo('gl-fsr1')).toBe(true);
        expect(r._fbo).toBe(fbo);
        expect(r._intermTex).toBe(tex);
    });
});
