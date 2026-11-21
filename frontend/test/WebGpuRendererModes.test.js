/**
 * WebGpuRenderer — mwsr's two modes (MwsrShader's MWSR_MODES), each a pass of
 * its own name. 'mwsr' is the kernel alone, pushed by the `performance` mode's
 * force in the product's sessions (opts.mwsrMode); 'mwsr-q' is `quality`: the
 * kernel into intermTex and a sharpening pass from there to the canvas, where
 * the stream is scaled up by less than ×2 and is SDR. Driven against a fake
 * device: what matters is which passes a frame takes, into which target, and
 * what the two uniform blocks hold.
 */
/* global console */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { WebGpuRenderer } from '../js/stream/renderers/WebGpuRenderer.js';
import { MWSR_MODES, mwsrLaw } from '../js/stream/renderers/MwsrShader.js';

const CANVAS_VIEW = { canvas: true };

function fakeGpu() {
    const pass = { setPipeline: vi.fn(), setBindGroup: vi.fn(), draw: vi.fn(), end: vi.fn() };
    const encoder = {
        beginRenderPass: vi.fn(() => pass),
        beginComputePass: vi.fn(() => pass),
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
        createShaderModule: vi.fn((desc) => ({
            code: desc.code,
            getCompilationInfo: async () => ({ messages: [] }),
        })),
        pushErrorScope() {},
        popErrorScope: async () => null,
        createRenderPipeline: vi.fn((desc) => ({
            code: desc.fragment.module.code,
            target: desc.fragment.targets[0].format,
            getBindGroupLayout: () => ({}),
        })),
        createComputePipeline: vi.fn(() => ({})),
        createBindGroup: vi.fn((desc) => ({ desc })),
        createBuffer: vi.fn((desc) => ({ desc, destroy() {} })),
        createTexture: vi.fn((desc) => ({ desc, createView: () => ({ of: desc }), destroy() {} })),
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
        configure: vi.fn((cfg) => void cfg.toneMapping),
        unconfigure: vi.fn(),
        getCurrentTexture: () => ({ createView: () => CANVAS_VIEW }),
    };
    const canvas = { width: 0, height: 0, getContext: (k) => (k === 'webgpu' ? ctx : null) };
    return { device, adapter, pass, encoder, canvas };
}

/** A 1280×720 frame. */
function frame(timestamp = 100) {
    return {
        displayWidth: 1280,
        displayHeight: 720,
        codedWidth: 1280,
        codedHeight: 720,
        timestamp,
        close: vi.fn(),
    };
}

/** A renderer drawing that frame into outW×outH (×1.5 unless told otherwise). */
async function renderer(gpu, opts, outW = 1920, outH = 1080) {
    Object.defineProperty(globalThis.navigator, 'gpu', {
        value: {
            requestAdapter: vi.fn(async () => gpu.adapter),
            getPreferredCanvasFormat: () => 'bgra8unorm',
        },
        configurable: true,
    });
    const r = await WebGpuRenderer.create(gpu.canvas, {
        videoCodec: 'h264',
        algo: 'mwsr',
        ...opts,
    });
    r.setOutputSize(outW, outH);
    return r;
}

/** One draw: the target and the pipeline of each of its render passes, in order. */
async function passesOf(gpu, r) {
    gpu.encoder.beginRenderPass.mockClear();
    gpu.pass.setPipeline.mockClear();
    await r.draw(frame());
    const views = gpu.encoder.beginRenderPass.mock.calls.map(([d]) => d.colorAttachments[0].view);
    const pipelines = gpu.pass.setPipeline.mock.calls.map(([p]) => p);
    return views.map((view, i) => ({ view, pipeline: pipelines[i] }));
}

/** The last block written into a uniform buffer. */
function written(gpu, buffer) {
    const w = gpu.device.queue.writeBuffer.mock.calls.filter(([b]) => b === buffer);
    return w.length ? Array.from(w[w.length - 1][2]) : null;
}

describe("WebGpuRenderer — mwsr's two modes", () => {
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

    it('builds nothing of the pass under mwsr, and draws by the kernel alone', async () => {
        for (const mwsrMode of [undefined, 'performance', 'quality']) {
            const gpu = fakeGpu();
            const r = await renderer(gpu, { mwsrMode });
            expect(r._mwsrSharpenPipeline, String(mwsrMode)).toBeNull();
            expect(r._mwsrPipelines.has('up>interm')).toBe(false);
            const passes = await passesOf(gpu, r);
            // pass 0 into inputTex, the kernel into the canvas
            expect(passes).toHaveLength(2);
            expect(passes[1].view).toBe(CANVAS_VIEW);
            expect(passes[1].pipeline).toBe(r._mwsrPipelines.get('up'));
            expect(r._intermTex).toBeNull();
            expect(r.lastDraw.path).toBe('mwsr');
            // the mode comes from the pass and the session, never from that word alone
            expect(r.mwsrMode).toBe(mwsrMode === 'performance' ? 'performance' : null);
        }
    });

    it('pushes the kernel by the performance mode in a session of the product, not in quality', async () => {
        const law = mwsrLaw(1280, 720, 1920, 1080);
        const block = async (opts) => {
            const gpu = fakeGpu();
            const r = await renderer(gpu, opts);
            await r.draw(frame());
            return written(gpu, r._mwsrUniform);
        };
        // the laboratory: the law alone
        const plain = await block({});
        expect(plain[10]).toBeCloseTo(law.kg, 6);
        // quality draws the kernel by the law, in the product's sessions too
        expect(await block({ algo: 'mwsr-q' })).toEqual(plain);
        expect(await block({ algo: 'mwsr-q', mwsrMode: 'performance' })).toEqual(plain);
        const perf = await block({ mwsrMode: 'performance' });
        const pushed = mwsrLaw(1280, 720, 1920, 1080, { mode: 'performance' });
        expect(pushed.kg).toBeGreaterThan(law.kg);
        expect(perf[10]).toBeCloseTo(pushed.kg, 6);
        expect(perf[20]).toBeCloseTo(pushed.kl, 6);
        expect(MWSR_MODES.performance.sharpen).toBeNull();
    });

    it('draws quality in three passes: the kernel into intermTex, then the sharpening into the canvas', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr-q' });
        expect(r.mwsrMode).toBe('quality');
        expect(r.algoName).toBe('MWSR');
        // built with the kernel's pipelines: nothing compiles at the first frame
        expect(r._mwsrSharpenPipeline.code).toContain('clamp(c + u.gain * (c - mean), lo, hi)');
        expect(r._mwsrSharpenPipeline.code).not.toContain('lobe');
        expect(r._mwsrSharpenPipeline.target).toBe('bgra8unorm');
        expect(r._mwsrPipelines.get('up>interm').target).toBe('rgba8unorm');
        expect(r._mwsrPipelines.get('up>interm').code).toBe(r._mwsrPipelines.get('up').code);
        const built = gpu.device.createRenderPipeline.mock.calls.length;

        const passes = await passesOf(gpu, r);
        expect(passes).toHaveLength(3);
        expect(r._intermTex.desc).toMatchObject({
            size: { width: 1920, height: 1080 },
            format: 'rgba8unorm',
        });
        expect(passes[1].view).toBe(r._intermTexView);
        expect(passes[1].pipeline).toBe(r._mwsrPipelines.get('up>interm'));
        expect(passes[2].view).toBe(CANVAS_VIEW);
        expect(passes[2].pipeline).toBe(r._mwsrSharpenPipeline);
        // the pass reads intermTex, with the gain of ×1.5
        expect(r._rcasBindGroup.desc.entries[0].resource).toBe(r._intermTexView);
        const u = written(gpu, r._rcasUniform);
        expect(u[0]).toBeCloseTo(1 / 1920, 9);
        expect(u[1]).toBeCloseTo(1 / 1080, 9);
        expect(u[2]).toBeCloseTo(0.15, 6);
        expect(r.lastDraw.path).toBe('mwsr+sharp');

        // a second frame at the same sizes writes neither block again, builds nothing
        const writes = gpu.device.queue.writeBuffer.mock.calls.length;
        await passesOf(gpu, r);
        expect(gpu.device.queue.writeBuffer.mock.calls.length).toBe(writes);
        expect(gpu.device.createRenderPipeline.mock.calls.length).toBe(built);
    });

    it('follows the scale: the pass at ×1.33, none from ×2 on, none for a reduction', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr-q' }, 1706, 960);
        expect(await passesOf(gpu, r)).toHaveLength(3);
        expect(written(gpu, r._rcasUniform)[2]).toBeCloseTo(0.3, 6);

        r.setOutputSize(2560, 1440);
        let passes = await passesOf(gpu, r);
        expect(passes).toHaveLength(2);
        expect(passes[1].view).toBe(CANVAS_VIEW);
        expect(passes[1].pipeline).toBe(r._mwsrPipelines.get('up'));
        expect(r.lastDraw.path).toBe('mwsr');

        r.setOutputSize(960, 540);
        passes = await passesOf(gpu, r);
        expect(passes).toHaveLength(2);
        expect(passes[1].pipeline).not.toBe(r._mwsrSharpenPipeline);
        expect(r.lastDraw.path).toBe('mwsr');

        // and back: the pass again, its block rewritten for the new size
        r.setOutputSize(1920, 1080);
        expect(await passesOf(gpu, r)).toHaveLength(3);
        expect(written(gpu, r._rcasUniform)[2]).toBeCloseTo(0.15, 6);
    });

    it("steps between the two modes as the ladder does, without rebuilding the kernel's pipelines", async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr-q', mwsrMode: 'performance' });
        const law = mwsrLaw(1280, 720, 1920, 1080);
        const pushed = mwsrLaw(1280, 720, 1920, 1080, { mode: 'performance' });
        expect(await passesOf(gpu, r)).toHaveLength(3);
        expect(written(gpu, r._mwsrUniform)[10]).toBeCloseTo(law.kg, 6);
        const built = gpu.device.createRenderPipeline.mock.calls.length;

        // down: the kernel alone, pushed by the performance mode
        expect(r.setAlgo('mwsr-q')).toBe(false);
        expect(r.setAlgo('mwsr')).toBe(true);
        expect(r.mwsrMode).toBe('performance');
        let passes = await passesOf(gpu, r);
        expect(passes).toHaveLength(2);
        expect(passes[1].view).toBe(CANVAS_VIEW);
        expect(written(gpu, r._mwsrUniform)[10]).toBeCloseTo(pushed.kg, 6);
        expect(r.lastDraw.path).toBe('mwsr');
        expect(r.setAlgo('mwsr')).toBe(false);

        // and back up: the pass again, the kernel by the law
        expect(r.setAlgo('mwsr-q')).toBe(true);
        expect(r.mwsrMode).toBe('quality');
        passes = await passesOf(gpu, r);
        expect(passes).toHaveLength(3);
        expect(passes[2].pipeline).toBe(r._mwsrSharpenPipeline);
        expect(written(gpu, r._mwsrUniform)[10]).toBeCloseTo(law.kg, 6);
        expect(written(gpu, r._rcasUniform)[2]).toBeCloseTo(0.15, 6);
        expect(gpu.device.createRenderPipeline.mock.calls.length).toBe(built);

        // through 'off' and back, like a ladder at its last rung
        expect(r.setAlgo('off')).toBe(true);
        expect(r.mwsrMode).toBeNull();
        expect(r.setAlgo('mwsr-q')).toBe(true);
        expect(await passesOf(gpu, r)).toHaveLength(3);
    });

    it('builds the pass at the first step up to quality when the session started below it', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { mwsrMode: 'performance' });
        expect(r._mwsrSharpenPipeline).toBeNull();
        expect(r.setAlgo('mwsr-q')).toBe(true);
        expect(r._mwsrSharpenPipeline).not.toBeNull();
        expect(r._mwsrPipelines.get('up>interm').target).toBe('rgba8unorm');
        const passes = await passesOf(gpu, r);
        expect(passes).toHaveLength(3);
        expect(passes[2].pipeline).toBe(r._mwsrSharpenPipeline);
    });

    it('keeps the law alone on an HDR stream, tone-mapped or not: no pass, no push', async () => {
        for (const hdr of [{ hdrExtTonemap: true }, { hdrTonemap: true }, { hdr: true }]) {
            const gpu = fakeGpu();
            const r = await renderer(gpu, { algo: 'mwsr-q', mwsrMode: 'performance', ...hdr });
            expect(r._mwsrSharpenPipeline, JSON.stringify(hdr)).toBeNull();
            expect(r._mwsrPipelines.has('up>interm')).toBe(false);
            expect(r.mwsrMode).toBeNull();
            expect(r.setAlgo('mwsr')).toBe(true);
            expect(r.mwsrMode).toBeNull();
        }
    });
});
