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
 * WebGpuRenderer — mwsr's reduction of an HDR frame on the float canvas
 * (`linear`). The bilinear taps of the grid mixed the encoded feed, which
 * took 1000- and 0.5-nit stripes down to 0.72-0.92 of their light
 * (hdr_check.py, journal J45). Now pass 0 writes the linear light for a
 * reduction (curve 3) and the kernel reads it on the views' grids, encoding
 * at the output (LINEAR_LIGHT 3); an upscale, 'off' and the `tonemap` path
 * keep the encoded feed. Driven against a fake device: what matters is the
 * curve pass 0 is given and the pipeline the kernel's pass takes.
 */
/* global console */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { WebGpuRenderer } from '../js/stream/renderers/WebGpuRenderer.js';

function fakeGpu() {
    const pass = { setPipeline: vi.fn(), setBindGroup: vi.fn(), draw: vi.fn(), end: vi.fn() };
    const encoder = {
        beginRenderPass: vi.fn(() => pass),
        beginComputePass: vi.fn(() => pass),
        finish: () => ({}),
        clearBuffer: vi.fn(),
        copyTextureToBuffer: vi.fn(),
    };
    const canvasView = { canvas: true };
    const device = {
        features: new Set(),
        limits: {},
        lost: new Promise(() => {}),
        addEventListener() {},
        destroy() {},
        createSampler: () => ({}),
        createBindGroupLayout: () => ({}),
        createPipelineLayout: () => ({}),
        createShaderModule: () => ({ getCompilationInfo: async () => ({ messages: [] }) }),
        pushErrorScope() {},
        popErrorScope: async () => null,
        createRenderPipeline: vi.fn((desc) => ({
            target: desc.fragment.targets[0].format,
            constants: desc.fragment.constants || {},
        })),
        createComputePipeline: () => ({}),
        createBindGroup: vi.fn((desc) => {
            if (!desc || !desc.layout) throw new TypeError('required member layout is undefined');
            return {};
        }),
        createBuffer: () => ({ destroy() {} }),
        createTexture: vi.fn((desc) => {
            const t = { desc, destroy() {} };
            t.createView = () => ({ of: t });
            return t;
        }),
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
        getCurrentTexture: () => ({ createView: () => canvasView }),
    };
    const canvas = { width: 0, height: 0, getContext: (k) => (k === 'webgpu' ? ctx : null) };
    return { device, adapter, encoder, pass, canvas, canvasView };
}

/** A 640×360 HDR frame as a software decoder hands it: I420P10, PQ, BT.2020. */
function hdrFrame() {
    const ySize = 640 * 360 * 2;
    const cSize = 320 * 180 * 2;
    return {
        displayWidth: 640,
        displayHeight: 360,
        codedWidth: 640,
        codedHeight: 360,
        timestamp: 0,
        format: 'I420P10',
        colorSpace: { primaries: 'bt2020', transfer: 'pq', matrix: 'bt2020-ncl', fullRange: false },
        close: vi.fn(),
        allocationSize: () => ySize + 2 * cSize,
        copyTo: vi.fn(async () => [
            { offset: 0, stride: 1280 },
            { offset: ySize, stride: 640 },
            { offset: ySize + cSize, stride: 640 },
        ]),
    };
}

async function renderer(gpu, opts, outW, outH) {
    Object.defineProperty(globalThis.navigator, 'gpu', {
        value: {
            requestAdapter: vi.fn(async () => gpu.adapter),
            getPreferredCanvasFormat: () => 'bgra8unorm',
        },
        configurable: true,
    });
    const r = await WebGpuRenderer.create(gpu.canvas, {
        algo: 'mwsr',
        videoCodec: 'hevc',
        ...opts,
    });
    r.setOutputSize(outW, outH);
    return r;
}

/** The curve the draw gave the YUV pass 0 (its params' fourth value). */
function curve(gpu, r) {
    const writes = gpu.device.queue.writeBuffer.mock.calls.filter(([buf]) => buf === r._yuvParams);
    expect(writes).toHaveLength(1);
    return writes[0][2][3];
}

/** The pipeline of the draw's last render pass (the kernel's, or the copy's). */
function lastPipeline(gpu) {
    const calls = gpu.pass.setPipeline.mock.calls;
    return calls[calls.length - 1][0];
}

const LINEAR = { hdr: true, hdrLinear: true };

describe("WebGpuRenderer — mwsr's HDR reduction in linear light", () => {
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

    it('builds the linear grids beside the software ones on the float canvas only', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, LINEAR, 320, 180);
        expect([...r._mwsrPipelines.keys()].sort()).toEqual(
            ['up', 'down4', 'down9', 'down16', 'down1lin', 'down9lin', 'down16lin'].sort(),
        );
        expect(r._mwsrPipelines.get('down1lin').constants).toEqual({ LINEAR_LIGHT: 3 });
        expect(r._mwsrPipelines.get('down4').constants).toEqual({ R_DOWN: 1, LINEAR_LIGHT: 1 });
        expect(r._mwsrPipelines.get('down1lin').target).toBe('rgba16float');
        const sdr = fakeGpu();
        const s = await renderer(sdr, { hdrTonemap: true }, 320, 180);
        expect([...s._mwsrPipelines.keys()].some((k) => k.endsWith('lin'))).toBe(false);
    });

    it('reduces in linear light: pass 0 left linear, the one-sample grid encoding at the output', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, LINEAR, 320, 180);
        await r.draw(hdrFrame());
        expect(curve(gpu, r)).toBe(3);
        expect(lastPipeline(gpu)).toBe(r._mwsrPipelines.get('down1lin'));
        expect(r.lastDraw.path).toBe('mwsr+yuv');
        expect(console.error).not.toHaveBeenCalled();
    });

    it('keeps the encoded feed for an upscale', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, LINEAR, 1280, 720);
        await r.draw(hdrFrame());
        expect(curve(gpu, r)).toBe(2);
        expect(lastPipeline(gpu)).toBe(r._mwsrPipelines.get('up'));
    });

    it("keeps the encoded feed for 'off' and at 1:1", async () => {
        for (const [algo, w, h] of [
            ['off', 320, 180],
            ['mwsr', 640, 360],
        ]) {
            const gpu = fakeGpu();
            const r = await renderer(gpu, { ...LINEAR, algo }, w, h);
            await r.draw(hdrFrame());
            expect(curve(gpu, r)).toBe(2);
        }
    });

    it('keeps the encoded feed and the 2×2 grid when the lab asks for the reduction of before', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, LINEAR, 320, 180);
        r._mwsrLinearDown = false;
        await r.draw(hdrFrame());
        expect(curve(gpu, r)).toBe(2);
        expect(lastPipeline(gpu)).toBe(r._mwsrPipelines.get('down4'));
    });

    it('takes the tone-map, not the float canvas, where the browser does not know toneMapping', async () => {
        const gpu = fakeGpu();
        const ctx = gpu.canvas.getContext('webgpu');
        // Chrome before 129: the member is never read, and nothing is thrown
        ctx.configure.mockImplementation(() => {});
        const r = await renderer(gpu, LINEAR, 320, 180);
        expect(r.hdrActive).toBe(false);
        expect(r._hdrLinear).toBe(false);
        expect(r._hdrTonemap).toBe(true);
        expect(r._refWhite).toBe(433);
        const last = ctx.configure.mock.calls[ctx.configure.mock.calls.length - 1][0];
        expect(last.format).toBe('bgra8unorm');
        expect(last.colorSpace).toBe('srgb');
    });

    it('leaves the tonemap path to its tone curve and its sRGB views', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { hdrTonemap: true }, 320, 180);
        await r.draw(hdrFrame());
        expect(curve(gpu, r)).toBe(1);
        expect(lastPipeline(gpu)).toBe(r._mwsrPipelines.get('down1'));
        expect(r._mwsrPipelines.get('down1').constants).toEqual({ LINEAR_LIGHT: 0 });
    });
});
