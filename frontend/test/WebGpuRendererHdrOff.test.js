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
 * WebGpuRenderer — an HDR frame on the readback paths with no upscaler
 * ('off'). The YUV pass 0 reads its planes texel by texel: drawn straight at
 * the canvas' size it scaled by the nearest texel (hdr_check.py, journal J45).
 * A scaled frame now takes it at its own size into inputTex, then the bilinear
 * copy an SDR frame gets; at 1:1 it still goes straight to the canvas. Driven
 * against a fake device whose createBindGroup refuses a missing layout, as a
 * browser does: what matters is which passes draw() runs, and into what.
 */
/* global console */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { WebGpuRenderer } from '../js/stream/renderers/WebGpuRenderer.js';

function fakeGpu() {
    const pass = { setPipeline: vi.fn(), setBindGroup: vi.fn(), draw: vi.fn(), end: vi.fn() };
    const encoder = {
        beginRenderPass: vi.fn(() => pass),
        finish: () => ({}),
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
        createRenderPipeline: vi.fn((desc) => ({ target: desc.fragment.targets[0].format })),
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
    const r = await WebGpuRenderer.create(gpu.canvas, { algo: 'off', videoCodec: 'hevc', ...opts });
    r.setOutputSize(outW, outH);
    return r;
}

/** The view each render pass of the draw drew into, and its pipeline, in order. */
function passes(gpu) {
    const views = gpu.encoder.beginRenderPass.mock.calls.map(([d]) => d.colorAttachments[0].view);
    const pipelines = gpu.pass.setPipeline.mock.calls.map(([p]) => p);
    return views.map((view, i) => ({ view, pipeline: pipelines[i] }));
}

describe('WebGpuRenderer — an HDR frame with no upscaler', () => {
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

    for (const [mode, opts, inter] of [
        ['linear (an HDR display)', { hdr: true, hdrLinear: true }, 'rgba16float'],
        ['tonemap (an SDR display)', { hdrTonemap: true }, 'rgba8unorm'],
    ]) {
        it(`scales a frame by pass 0 at its size, then the bilinear copy: ${mode}`, async () => {
            const gpu = fakeGpu();
            const r = await renderer(gpu, opts, 1280, 720);
            await r.draw(hdrFrame());
            const p = passes(gpu);
            expect(p).toHaveLength(2);
            // pass 0 at the frame's size into inputTex, in the intermediates' format
            expect(p[0].view.of).toBe(r._inputTex);
            expect(r._inputTex.desc.size).toEqual({ width: 640, height: 360 });
            expect(r._inputTex.desc.format).toBe(inter);
            expect(p[0].pipeline).toBe(r._yuvPipeline);
            // then the copy that samples inputTex, into the canvas
            expect(p[1].view).toBe(gpu.canvasView);
            expect(p[1].pipeline).toBe(r._texBlitPipeline);
            expect(gpu.pass.setBindGroup.mock.calls[1][1]).toBe(r._texBlitBindGroup);
            expect(r.lastDraw.path).toBe('off+yuv');
            expect(console.error).not.toHaveBeenCalled();
            expect(r.failed).toBe(false);
        });

        it(`draws a frame at 1:1 straight to the canvas: ${mode}`, async () => {
            const gpu = fakeGpu();
            const r = await renderer(gpu, opts, 640, 360);
            await r.draw(hdrFrame());
            const p = passes(gpu);
            expect(p).toHaveLength(1);
            expect(p[0].view).toBe(gpu.canvasView);
            expect(p[0].pipeline).toBe(r._yuvCanvasPipeline);
            expect(r._inputTex).toBe(null);
            expect(r.lastDraw.path).toBe('off+yuv');
        });
    }
});
