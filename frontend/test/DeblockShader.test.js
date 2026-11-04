/*
 * MoonlightWeb — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * mwsr 2 and 3 (DeblockShader.js): the strengths, the uniform block, and the
 * WebGPU renderer driven against a fake device — the deblocking runs only
 * when asked for, and nothing of it (no pipeline, no buffer, no QP) exists
 * otherwise.
 */
/* global console */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import {
    DEBLOCK_DCT_WGSL,
    DEBLOCK_FIN_WGSL,
    DEBLOCK_FIXED,
    deblockDispatch,
    deblockStrength,
    deblockUniforms,
} from '../js/stream/renderers/DeblockShader.js';
import { VideoRenderer } from '../js/stream/renderers/VideoRenderer.js';
import { WebGpuRenderer } from '../js/stream/renderers/WebGpuRenderer.js';

describe('deblockStrength', () => {
    it('is the fixed t8c2 for mwsr 2, whatever the QP', () => {
        expect(deblockStrength('fixed', null)).toEqual({ thr: 8, cmax: 2 });
        expect(deblockStrength('fixed', 45)).toBe(DEBLOCK_FIXED);
    });

    it('follows the frame QP for mwsr 3: t12c4 from QP 34, about t6c2 at 28, none under 20', () => {
        expect(deblockStrength('qp', 34)).toEqual({ thr: 12, cmax: 4 });
        expect(deblockStrength('qp', 51)).toEqual({ thr: 12, cmax: 4 });
        const at28 = deblockStrength('qp', 28);
        expect(at28.thr).toBeCloseTo(6.4, 5);
        expect(at28.cmax).toBeCloseTo(6.4 / 3, 5);
        expect(deblockStrength('qp', 22).thr).toBeCloseTo(3.2, 5);
        expect(deblockStrength('qp', 22).cmax).toBe(2);
        expect(deblockStrength('qp', 19)).toBeNull();
        expect(deblockStrength('qp', 0)).toBeNull();
    });

    it("takes mwsr 2's strength for a frame whose QP could not be read", () => {
        expect(deblockStrength('qp', null)).toBe(DEBLOCK_FIXED);
        expect(deblockStrength('qp', undefined)).toBe(DEBLOCK_FIXED);
    });

    it('is null without a mode', () => {
        expect(deblockStrength(null, 30)).toBeNull();
        expect(deblockStrength('off', 30)).toBeNull();
    });
});

describe('deblockUniforms and deblockDispatch', () => {
    it('packs the size and the strengths on luma × 255 (× 255/219)', () => {
        const b = deblockUniforms(1280, 720, { thr: 8, cmax: 2 });
        expect(b.byteLength).toBe(16);
        expect(Array.from(new Int32Array(b, 0, 2))).toEqual([1280, 720]);
        const f = new Float32Array(b, 8, 2);
        expect(f[0]).toBeCloseTo((8 * 255) / 219, 5);
        expect(f[1]).toBeCloseTo((2 * 255) / 219, 5);
    });

    it('covers every pixel with one block of each of the 16 grids', () => {
        expect(deblockDispatch(1280, 720)).toEqual([161, 91, 16]);
        expect(deblockDispatch(1281, 721)).toEqual([162, 92, 16]);
    });

    it('sums with atomics in the compute pass and reads them in the final one', () => {
        expect(DEBLOCK_DCT_WGSL).toContain('array<atomic<i32>>');
        expect(DEBLOCK_DCT_WGSL).toContain('atomicAdd');
        expect(DEBLOCK_DCT_WGSL).toContain('@workgroup_size(8, 8)');
        expect(DEBLOCK_FIN_WGSL).toContain('var<storage, read> acc : array<i32>');
        expect(DEBLOCK_FIN_WGSL).toContain('/ (ACC_SCALE * 255.0 * 16.0)');
    });
});

function fakeGpu() {
    const pass = { setPipeline: vi.fn(), setBindGroup: vi.fn(), draw: vi.fn(), end: vi.fn() };
    const compute = {
        setPipeline: vi.fn(),
        setBindGroup: vi.fn(),
        dispatchWorkgroups: vi.fn(),
        end: vi.fn(),
    };
    const encoder = {
        beginRenderPass: vi.fn(() => pass),
        beginComputePass: vi.fn(() => compute),
        clearBuffer: vi.fn(),
        finish: () => ({}),
        copyTextureToBuffer: vi.fn(),
    };
    const device = {
        features: new Set(),
        limits: {},
        lost: new Promise(() => {}),
        addEventListener() {},
        destroy() {},
        createSampler: () => ({}),
        createBindGroupLayout: () => ({}),
        createPipelineLayout: () => ({}),
        createShaderModule: vi.fn(() => ({ getCompilationInfo: async () => ({ messages: [] }) })),
        pushErrorScope() {},
        popErrorScope: async () => null,
        createRenderPipeline: vi.fn(() => ({})),
        createComputePipeline: vi.fn(() => ({})),
        createBindGroup: vi.fn(() => ({})),
        createBuffer: vi.fn((desc) => ({ desc, destroy() {} })),
        createTexture: vi.fn((desc) => ({ desc, createView: () => ({}), destroy() {} })),
        createCommandEncoder: vi.fn(() => encoder),
        importExternalTexture: vi.fn(() => ({})),
        queue: {
            submit: vi.fn(),
            writeBuffer: vi.fn(),
            writeTexture: vi.fn(),
            onSubmittedWorkDone: vi.fn(async () => {}),
        },
    };
    const adapter = {
        info: { vendor: 'test' },
        features: new Set(),
        limits: {},
        requestDevice: vi.fn(async () => device),
    };
    const ctx = {
        // reads its dictionary as a browser does, toneMapping a member it knows
        configure: vi.fn((cfg) => void cfg.toneMapping),
        unconfigure: vi.fn(),
        getCurrentTexture: () => ({ createView: () => ({}) }),
    };
    const canvas = { width: 0, height: 0, getContext: (k) => (k === 'webgpu' ? ctx : null) };
    return { device, adapter, encoder, compute, canvas };
}

/** A 640×360 frame, drawn up to 1280×720. */
function frame(timestamp) {
    return {
        displayWidth: 640,
        displayHeight: 360,
        codedWidth: 640,
        codedHeight: 360,
        timestamp,
        close: vi.fn(),
    };
}

async function renderer(gpu, opts) {
    Object.defineProperty(globalThis.navigator, 'gpu', {
        value: {
            requestAdapter: vi.fn(async () => gpu.adapter),
            getPreferredCanvasFormat: () => 'bgra8unorm',
        },
        configurable: true,
    });
    const r = await WebGpuRenderer.create(gpu.canvas, { videoCodec: 'h264', ...opts });
    r.setOutputSize(1280, 720);
    return r;
}

const deblockModules = (gpu) =>
    gpu.device.createShaderModule.mock.calls.filter(
        ([d]) => d.code === DEBLOCK_DCT_WGSL || d.code === DEBLOCK_FIN_WGSL,
    ).length;

describe('WebGpuRenderer — mwsr 2 and 3', () => {
    beforeEach(() => {
        vi.stubGlobal('GPUShaderStage', { VERTEX: 1, FRAGMENT: 2, COMPUTE: 4 });
        vi.stubGlobal('GPUTextureUsage', {
            COPY_SRC: 1,
            COPY_DST: 2,
            TEXTURE_BINDING: 4,
            STORAGE_BINDING: 8,
            RENDER_ATTACHMENT: 16,
        });
        vi.stubGlobal('GPUBufferUsage', {
            MAP_READ: 1,
            MAP_WRITE: 2,
            COPY_SRC: 4,
            COPY_DST: 8,
            UNIFORM: 64,
            STORAGE: 128,
            QUERY_RESOLVE: 512,
        });
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
    });
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
        delete globalThis.navigator.gpu;
    });

    it('asks for no QP anywhere but mwsr 3: the decode paths read no slice header', async () => {
        expect(new VideoRenderer().wantsFrameQp).toBe(false);
        expect(new WebGpuRenderer().wantsFrameQp).toBe(false);
        const plain = await renderer(fakeGpu(), { algo: 'mwsr' });
        expect(plain.wantsFrameQp).toBe(false);
        const fixed = await renderer(fakeGpu(), { algo: 'mwsr', deblock: 'fixed' });
        expect(fixed.wantsFrameQp).toBe(false);
        const qp = await renderer(fakeGpu(), { algo: 'mwsr', deblock: 'qp' });
        expect(qp.wantsFrameQp).toBe(true);
        // Stepped down by the ladder: no pass of its own, no QP read.
        qp.setAlgo('off');
        expect(qp.wantsFrameQp).toBe(false);
    });

    it('builds and runs nothing of it for plain mwsr', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr' });
        await r.draw(frame(1));
        await r.draw(frame(2));
        expect(deblockModules(gpu)).toBe(0);
        expect(gpu.device.createComputePipeline).not.toHaveBeenCalled();
        expect(gpu.encoder.beginComputePass).not.toHaveBeenCalled();
        expect(gpu.encoder.clearBuffer).not.toHaveBeenCalled();
        const storage = gpu.device.createBuffer.mock.calls.filter(
            ([d]) => d.usage & 128 /* STORAGE */,
        );
        expect(storage).toHaveLength(0);
        expect(r.deblockLast).toBeNull();
    });

    it('deblocks every frame before the kernel for mwsr 2', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr', deblock: 'fixed' });
        await r.draw(frame(1));
        await r.draw(frame(2));
        expect(deblockModules(gpu)).toBe(2);
        expect(gpu.encoder.beginComputePass).toHaveBeenCalledTimes(2);
        expect(gpu.compute.dispatchWorkgroups).toHaveBeenCalledWith(81, 46, 16);
        // The sums' buffer: one i32 per pixel of the frame, cleared each frame.
        const acc = gpu.device.createBuffer.mock.calls.find(([d]) => d.usage & 128)[0];
        expect(acc.size).toBe(640 * 360 * 4);
        expect(gpu.encoder.clearBuffer).toHaveBeenCalledTimes(2);
        expect(r.deblockLast).toEqual({ qp: null, thr: 8 });
    });

    it("sets mwsr 3's strength from each frame's own QP, by its timestamp", async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr', deblock: 'qp' });
        r.noteFrameQp(100, 36);
        r.noteFrameQp(200, 18);
        r.noteFrameQp(300, 28);
        await r.draw(frame(100));
        expect(r.deblockLast).toEqual({ qp: 36, thr: 12 });
        expect(gpu.encoder.beginComputePass).toHaveBeenCalledTimes(1);
        // QP 18: fine enough, both passes left out.
        await r.draw(frame(200));
        expect(r.deblockLast).toEqual({ qp: 18, thr: 0 });
        expect(gpu.encoder.beginComputePass).toHaveBeenCalledTimes(1);
        await r.draw(frame(300));
        expect(r.deblockLast.qp).toBe(28);
        expect(r.deblockLast.thr).toBeCloseTo(6.4, 5);
        expect(gpu.encoder.beginComputePass).toHaveBeenCalledTimes(2);
        // A frame whose QP never came keeps the last one.
        await r.draw(frame(400));
        expect(r.deblockLast.qp).toBe(28);
    });

    it('forgets the QPs of frames the decoder dropped, and keeps at most 64', async () => {
        const r = await renderer(fakeGpu(), { algo: 'mwsr', deblock: 'qp' });
        for (let t = 1; t <= 100; t++) r.noteFrameQp(t, 30);
        expect(r._qpByTs.size).toBe(64);
        await r.draw(frame(90));
        // 91..100 left: the frames before 90 were drawn or dropped.
        expect([...r._qpByTs.keys()]).toEqual(Array.from({ length: 10 }, (_, i) => 91 + i));
    });

    it('turns itself off on HDR intermediates, its thresholds being 8-bit code values', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr', deblock: 'qp', hdr: true });
        expect(r._interFormat).toBe('rgba16float');
        expect(r.wantsFrameQp).toBe(false);
        await r.draw(frame(1));
        expect(gpu.encoder.beginComputePass).not.toHaveBeenCalled();
    });
});
