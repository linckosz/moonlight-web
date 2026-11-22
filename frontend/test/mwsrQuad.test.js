/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

// mwsr's vote one slope per 2×2 quad where the GPU takes its slopes per pixel:
// the texts (MwsrShader's quad option), the probe's verdict, and the probe on
// a WebGPU device and a WebGL2 context, failures included.
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
    MWSR_QUAD_PROBE_GLSL,
    MWSR_QUAD_PROBE_SIZE,
    MWSR_QUAD_PROBE_WGSL,
    mwsrGlsl,
    mwsrQuadVerdict,
    mwsrWgsl,
} from '../js/stream/renderers/MwsrShader.js';
import { mwsrQuadProbeWebGl, mwsrQuadProbeWebGpu } from '../js/stream/renderers/mwsrQuadProbe.js';

const N = MWSR_QUAD_PROBE_SIZE;

/**
 * The probe's picture as a GPU would draw it, v = x·y: `native` and `emulated`
 * each 'fine' (the slope of the pixel's own row and column) or 'coarse' (the
 * quad's first row and column).
 */
function picture(native, emulated) {
    const px = new Uint8Array(N * N * 4);
    for (let y = 0; y < N; y++)
        for (let x = 0; x < N; x++) {
            const i = (y * N + x) * 4;
            const slopes = (kind) => (kind === 'fine' ? [y, x] : [y & ~1, x & ~1]);
            [px[i], px[i + 1]] = slopes(native);
            [px[i + 2], px[i + 3]] = slopes(emulated);
        }
    return px;
}

describe('MwsrShader — the quad option', () => {
    const cases = [
        { mode: 'up' },
        { mode: 'up', map: true },
        { mode: 'up', half: true },
        { mode: 'up', feed: 'ext' },
        { mode: 'down', taps: 1 },
        { mode: 'down', taps: 4 },
        { mode: 'down', taps: 9 },
        { mode: 'down', taps: 16 },
    ];

    it("keeps every WGSL and GLSL text as it was, to the byte, under 'native'", () => {
        for (const c of cases) {
            expect(mwsrWgsl({ ...c, quad: 'native' }), JSON.stringify(c)).toBe(mwsrWgsl(c));
            expect(mwsrWgsl(c)).not.toContain('mwsrQd');
            if (c.map || c.half || c.feed) continue;
            const g = { mode: c.mode, taps: c.taps };
            expect(mwsrGlsl({ ...g, quad: 'native' })).toBe(mwsrGlsl(g));
            expect(mwsrGlsl(g)).not.toContain('mwsrQd');
        }
    });

    it("emulates each slope of the vote under 'emulate', and leaves no native one", () => {
        for (const c of cases) {
            const key = JSON.stringify(c);
            const native = mwsrWgsl(c);
            const w = mwsrWgsl({ ...c, quad: 'emulate' });
            expect(w, key).not.toMatch(/dpd[xy]Coarse/);
            // the reductions of 9 and 16 samples take no slope at all: their text as it was
            const sloped = c.mode === 'up' || c.taps <= 4;
            if (!sloped) expect(w, key).toBe(native);
            else {
                expect(w.match(/mwsrQd[xy][13]\(\w+, in\.pos\)/g).length, key).toBe(
                    c.mode === 'up' ? 4 : 2,
                );
                // the stage itself is the native one's, call for call
                const back = w.replace(/mwsrQd([xy])[13]\((\w+), in\.pos\)/g, 'dpd$1Coarse($2)');
                expect(back.endsWith(native.slice(native.indexOf('@fragment'))), key).toBe(true);
                expect(back.startsWith(native.slice(0, 400)), key).toBe(true);
            }
            if (c.map || c.half || c.feed) continue;
            const gNative = mwsrGlsl({ mode: c.mode, taps: c.taps });
            const g = mwsrGlsl({ mode: c.mode, taps: c.taps, quad: 'emulate' });
            if (!sloped) {
                expect(g, key).toBe(gNative);
                continue;
            }
            const stage = g.slice(g.indexOf('}', g.lastIndexOf('highp vec3 mwsrQdy')) + 1);
            expect(stage, key).not.toMatch(/dFd[xy]\(/);
            expect(g.match(/mwsrQd[xy]\((rM|lM|l)\)/g).length, key).toBe(c.mode === 'up' ? 4 : 2);
        }
    });

    it('says which derivatives it cannot take', () => {
        expect(() => mwsrWgsl({ quad: 'quadBroadcast' })).toThrow(/quad/);
        expect(() => mwsrWgsl({ quad: 'emulate', fine: true })).toThrow(/fine/);
        expect(() => mwsrGlsl({ quad: 'maybe' })).toThrow(/quad/);
    });

    it('draws its probe from the same emulation', () => {
        expect(MWSR_QUAD_PROBE_WGSL).toContain('fn mwsrQdx1');
        expect(MWSR_QUAD_PROBE_WGSL).toContain(
            'dpdxCoarse(v), dpdyCoarse(v), mwsrQdx1(v, pos), mwsrQdy1(v, pos)',
        );
        expect(MWSR_QUAD_PROBE_GLSL).toContain('dFdx(v), dFdy(v), mwsrQdx(v), mwsrQdy(v)');
        expect(MWSR_QUAD_PROBE_GLSL.startsWith('#version 300 es')).toBe(true);
    });
});

describe('mwsrQuadVerdict', () => {
    it('emulates only where the native slopes are per pixel and the emulated ones per quad', () => {
        expect(mwsrQuadVerdict(picture('fine', 'coarse'))).toEqual({
            native: 'fine',
            emulated: 'coarse',
            quad: 'emulate',
        });
        expect(mwsrQuadVerdict(picture('coarse', 'coarse')).quad).toBe('native');
        expect(mwsrQuadVerdict(picture('fine', 'fine'))).toEqual({
            native: 'fine',
            emulated: 'fine',
            quad: 'native',
        });
    });

    it('keeps the texts on a picture that says neither, or nothing', () => {
        const mixed = picture('fine', 'coarse');
        mixed[0] = mixed[N * 4]; // one quad column keeps one slope: fine no more
        expect(mwsrQuadVerdict(mixed)).toMatchObject({ native: 'mixed', quad: 'native' });
        expect(mwsrQuadVerdict(new Uint8Array(N * N * 4))).toEqual({
            native: 'none',
            emulated: 'none',
            quad: 'native',
        });
    });
});

/** A WebGPU device that draws the probe's picture `px`, or fails where told. */
function fakeDevice(px, { scopeError = null, mapNever = false, throwAt = null } = {}) {
    const made = { textures: [], buffers: [] };
    const scopes = [];
    const device = {
        pushErrorScope: vi.fn((f) => scopes.push(f)),
        popErrorScope: vi.fn(async () => {
            const f = scopes.pop();
            return f === 'validation' ? scopeError : null;
        }),
        createTexture: vi.fn(() => {
            if (throwAt === 'texture') throw new Error('no texture');
            const t = { createView: () => ({}), destroy: vi.fn() };
            made.textures.push(t);
            return t;
        }),
        createBuffer: vi.fn(() => {
            const rows = new Uint8Array(256 * N);
            for (let y = 0; y < N; y++) rows.set(px.subarray(y * N * 4, (y + 1) * N * 4), y * 256);
            const b = {
                mapAsync: vi.fn(() => (mapNever ? new Promise(() => {}) : Promise.resolve())),
                getMappedRange: () => rows.buffer,
                unmap: vi.fn(),
                destroy: vi.fn(),
            };
            made.buffers.push(b);
            return b;
        }),
        createShaderModule: vi.fn(() => ({})),
        createRenderPipeline: vi.fn(() => ({})),
        createCommandEncoder: () => ({
            beginRenderPass: () => ({ setPipeline() {}, draw() {}, end() {} }),
            copyTextureToBuffer: vi.fn(),
            finish: () => ({}),
        }),
        queue: { submit: vi.fn() },
    };
    return { device, made, scopes };
}

describe('mwsrQuadProbeWebGpu', () => {
    beforeEach(() => {
        vi.stubGlobal('GPUTextureUsage', { COPY_SRC: 1, RENDER_ATTACHMENT: 16 });
        vi.stubGlobal('GPUBufferUsage', { MAP_READ: 1, COPY_DST: 8 });
        vi.stubGlobal('GPUMapMode', { READ: 1 });
        vi.spyOn(console, 'info').mockImplementation(() => {});
    });
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
    });

    it('emulates on a GPU whose coarse slopes are per pixel, and says so in one line without the GPU', async () => {
        const { device, made, scopes } = fakeDevice(picture('fine', 'coarse'));
        expect(await mwsrQuadProbeWebGpu(device)).toBe('emulate');
        expect(made.textures[0].destroy).toHaveBeenCalled();
        expect(made.buffers[0].destroy).toHaveBeenCalled();
        expect(scopes).toHaveLength(0);
        expect(device.pushErrorScope.mock.calls.map(([f]) => f).sort()).toEqual([
            'out-of-memory',
            'validation',
        ]);
        expect(console.info).toHaveBeenCalledTimes(1);
        expect(console.info.mock.calls[0][0]).toMatch(
            /^\[mwsr\] quad probe \(webgpu\): fine slopes, emulated per quad \(\d+\.\d ms\)$/,
        );
    });

    it('keeps the texts on a GPU that keeps one slope per quad', async () => {
        const { device } = fakeDevice(picture('coarse', 'coarse'));
        expect(await mwsrQuadProbeWebGpu(device)).toBe('native');
    });

    it('keeps the texts, and frees what it made, when its draw has an error', async () => {
        const { device, made, scopes } = fakeDevice(picture('fine', 'coarse'), {
            scopeError: { message: 'invalid' },
        });
        expect(await mwsrQuadProbeWebGpu(device)).toBe('native');
        expect(made.textures[0].destroy).toHaveBeenCalled();
        expect(made.buffers[0].destroy).toHaveBeenCalled();
        expect(scopes).toHaveLength(0);
    });

    it('keeps the texts when a call throws, its error scopes closed', async () => {
        const { device, scopes } = fakeDevice(picture('fine', 'coarse'), { throwAt: 'texture' });
        expect(await mwsrQuadProbeWebGpu(device)).toBe('native');
        expect(scopes).toHaveLength(0);
    });

    it('goes on without an answer past its bound, and drops one that comes later', async () => {
        const { device, made } = fakeDevice(picture('fine', 'coarse'), { mapNever: true });
        const t0 = performance.now();
        expect(await mwsrQuadProbeWebGpu(device, { timeoutMs: 20 })).toBe('native');
        expect(performance.now() - t0).toBeLessThan(1000);
        expect(made.buffers[0].destroy).toHaveBeenCalled();
        expect(console.info.mock.calls.at(-1)[0]).toMatch(/no answer within 20 ms/);
    });
});

/** A WebGL2 context that draws the probe's picture `px` and records its state. */
function fakeGl(px, { lostAt = null, compiles = true } = {}) {
    let lost = false;
    const state = {
        fb: 'canvas',
        readFb: 'canvas',
        viewport: [0, 0, 640, 360],
        program: 'kernel',
        vao: 'vao',
    };
    const deleted = [];
    const gl = {
        FRAMEBUFFER_BINDING: 'FRAMEBUFFER_BINDING',
        READ_FRAMEBUFFER_BINDING: 'READ_FRAMEBUFFER_BINDING',
        VIEWPORT: 'VIEWPORT',
        CURRENT_PROGRAM: 'CURRENT_PROGRAM',
        VERTEX_ARRAY_BINDING: 'VERTEX_ARRAY_BINDING',
        ACTIVE_TEXTURE: 'ACTIVE_TEXTURE',
        TEXTURE_BINDING_2D: 'TEXTURE_BINDING_2D',
        PACK_ALIGNMENT: 'PACK_ALIGNMENT',
        PIXEL_PACK_BUFFER_BINDING: 'PIXEL_PACK_BUFFER_BINDING',
        FRAMEBUFFER: 'FRAMEBUFFER',
        READ_FRAMEBUFFER: 'READ_FRAMEBUFFER',
        FRAMEBUFFER_COMPLETE: 'complete',
        isContextLost: () => lost,
        getParameter: (k) =>
            ({
                FRAMEBUFFER_BINDING: state.fb,
                READ_FRAMEBUFFER_BINDING: state.readFb,
                VIEWPORT: state.viewport,
                CURRENT_PROGRAM: state.program,
                VERTEX_ARRAY_BINDING: state.vao,
                ACTIVE_TEXTURE: 'TEXTURE1',
                TEXTURE_BINDING_2D: 'input',
                PACK_ALIGNMENT: 1,
                PIXEL_PACK_BUFFER_BINDING: null,
            })[k],
        activeTexture() {},
        createShader: (t) => ({ t }),
        shaderSource() {},
        compileShader() {},
        getShaderParameter: () => compiles,
        createProgram: () => ({ p: 'probe' }),
        attachShader() {},
        linkProgram() {},
        getProgramParameter: () => true,
        createTexture: () => ({ tex: 'probe' }),
        bindTexture() {},
        texImage2D() {},
        createFramebuffer: () => ({ fb: 'probe' }),
        bindFramebuffer: (target, fb) => {
            if (target === 'FRAMEBUFFER') state.fb = state.readFb = fb;
            else state.readFb = fb;
        },
        framebufferTexture2D() {},
        checkFramebufferStatus: () => 'complete',
        createVertexArray: () => ({ vao: 'probe' }),
        bindVertexArray: (v) => void (state.vao = v),
        viewport: (...v) => void (state.viewport = v),
        useProgram: (p) => void (state.program = p),
        drawArrays: () => {
            if (lostAt === 'draw') lost = true;
        },
        bindBuffer() {},
        pixelStorei() {},
        readPixels: (x, y, w, h, f, t, out) => out.set(px),
        deleteVertexArray: (o) => deleted.push(o),
        deleteFramebuffer: (o) => deleted.push(o),
        deleteTexture: (o) => deleted.push(o),
        deleteProgram: (o) => deleted.push(o),
        deleteShader: (o) => deleted.push(o),
    };
    return { gl, state, deleted };
}

describe('mwsrQuadProbeWebGl', () => {
    beforeEach(() => vi.spyOn(console, 'info').mockImplementation(() => {}));
    afterEach(() => vi.restoreAllMocks());

    it('emulates where dFdx is per pixel, and gives every binding back', () => {
        const { gl, state, deleted } = fakeGl(picture('fine', 'coarse'));
        expect(mwsrQuadProbeWebGl(gl, 'vs')).toBe('emulate');
        expect(state).toEqual({
            fb: 'canvas',
            readFb: 'canvas',
            viewport: [0, 0, 640, 360],
            program: 'kernel',
            vao: 'vao',
        });
        expect(deleted).toHaveLength(6);
        expect(console.info.mock.calls[0][0]).toMatch(
            /^\[mwsr\] quad probe \(webgl\): fine slopes, emulated per quad/,
        );
    });

    it('keeps the texts where dFdx is per quad', () => {
        expect(mwsrQuadProbeWebGl(fakeGl(picture('coarse', 'coarse')).gl, 'vs')).toBe('native');
    });

    it('keeps the texts on a context lost before or during the probe', () => {
        const before = fakeGl(picture('fine', 'coarse'));
        before.gl.isContextLost = () => true;
        expect(mwsrQuadProbeWebGl(before.gl, 'vs')).toBe('native');
        expect(
            mwsrQuadProbeWebGl(fakeGl(picture('fine', 'coarse'), { lostAt: 'draw' }).gl, 'vs'),
        ).toBe('native');
    });

    it('keeps the texts, and frees what it made, when its program does not compile', () => {
        const { gl, state, deleted } = fakeGl(picture('fine', 'coarse'), { compiles: false });
        expect(mwsrQuadProbeWebGl(gl, 'vs')).toBe('native');
        expect(state.program).toBe('kernel');
        expect(deleted).toHaveLength(2);
    });
});
