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
 * WebGpuRenderer — a pipeline the device refuses. It draws nothing and tells
 * the frame nothing: submit and onSubmittedWorkDone succeed. Its refusal is
 * kept, and the renderer gives up when a frame's pass uses it, for the page to
 * move the picture to WebGL2. Driven against a fake device whose error scopes
 * work as WebGPU's: a refusal lands in the innermost scope of its filter.
 */
/* global console, setTimeout */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { WebGpuRenderer } from '../js/stream/renderers/WebGpuRenderer.js';

const REFUSAL = 'Fragment library could not be created';
const tick = () => new Promise((res) => setTimeout(res));

/** @param {function(object): boolean} [refuses] which render pipelines the device refuses */
function fakeGpu(refuses = () => false) {
    const pass = { setPipeline: vi.fn(), setBindGroup: vi.fn(), draw: vi.fn(), end: vi.fn() };
    const encoder = {
        beginRenderPass: vi.fn(() => pass),
        beginComputePass: vi.fn(() => pass),
        finish: () => ({}),
        clearBuffer: vi.fn(),
        copyTextureToBuffer: vi.fn(),
    };
    const scopes = [];
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
        pushErrorScope: vi.fn((filter) => scopes.push({ filter, error: null })),
        popErrorScope: vi.fn(async () => scopes.pop().error),
        createRenderPipeline: vi.fn((desc) => {
            if (refuses(desc))
                scopes.findLast((s) => s.filter === 'validation').error = { message: REFUSAL };
            return { constants: desc.fragment.constants || {} };
        }),
        createComputePipeline: () => ({}),
        createBindGroup: vi.fn(() => ({})),
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
        configure: vi.fn((cfg) => void cfg.toneMapping),
        unconfigure: vi.fn(),
        getCurrentTexture: () => ({ createView: () => ({ canvas: true }) }),
    };
    const canvas = { width: 0, height: 0, getContext: (k) => (k === 'webgpu' ? ctx : null) };
    return { device, adapter, pass, canvas, scopes };
}

/** A 640×360 HDR frame as a software decoder hands it: I420P10, PQ, BT.2020. */
function frame() {
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
        hdr: true,
        hdrLinear: true,
        ...opts,
    });
    r.setOutputSize(outW, outH);
    await tick();
    return r;
}

/** The one-read reduction of the float canvas, and no other pipeline. */
const isDown1 = (desc) => {
    const c = desc.fragment.constants;
    return !!c && c.LINEAR_LIGHT === 3 && !('R_DOWN' in c);
};

const warned = () => console.warn.mock.calls.map((c) => String(c[0]));

describe('WebGpuRenderer — a pipeline the device refuses', () => {
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

    it('changes nothing while the device refuses nothing', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, {}, 320, 180);
        await r.draw(frame());
        r.setOutputSize(1280, 720);
        await r.draw(frame());
        await tick();
        expect(r.failed).toBe(false);
        expect(r._refused.size).toBe(0);
        expect(warned().filter((w) => /refused|giving up/.test(w))).toEqual([]);
        expect(gpu.scopes).toEqual([]);
        expect(gpu.device.popErrorScope.mock.calls.length).toBe(
            gpu.device.pushErrorScope.mock.calls.length,
        );
    });

    it("gives up when the frame's pass uses the refused pipeline", async () => {
        const gpu = fakeGpu(isDown1);
        const r = await renderer(gpu, {}, 320, 180);
        expect(r.failed).toBe(false);
        await r.draw(frame());
        const calls = gpu.pass.setPipeline.mock.calls;
        expect(calls[calls.length - 1][0]).toBe(r._mwsrPipelines.get('down1lin'));
        expect(r.failed).toBe(true);
        expect(warned()).toContain('[WebGpuRenderer] a pipeline was refused: ' + REFUSAL);
        expect(warned().filter((w) => w.includes('giving up'))).toHaveLength(1);
    });

    it('does nothing about a refused pipeline no frame uses, until one does', async () => {
        const gpu = fakeGpu(isDown1);
        const r = await renderer(gpu, {}, 1280, 720);
        await r.draw(frame());
        expect(r._refused.size).toBe(1);
        expect(r.failed).toBe(false);
        r.setOutputSize(320, 180);
        await r.draw(frame());
        expect(r.failed).toBe(true);
    });

    it('takes the same way on the other passes', async () => {
        for (const algo of ['fsr1', 'off']) {
            const gpu = fakeGpu(() => true);
            const r = await renderer(gpu, { algo }, 1280, 720);
            await r.draw(frame());
            expect(r.failed, algo).toBe(true);
        }
    });

    it('stays quiet when the device goes before it answers', async () => {
        const gpu = fakeGpu();
        gpu.device.popErrorScope = vi.fn(() => Promise.reject(new Error('device lost')));
        const r = await renderer(gpu, {}, 320, 180);
        await r.draw(frame());
        await tick();
        expect(r.failed).toBe(false);
        expect(r._refused.size).toBe(0);
    });

    it('leaves no scope open when the creation throws', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, {}, 320, 180);
        gpu.device.createRenderPipeline.mockImplementationOnce(() => {
            throw new TypeError('required member layout is undefined');
        });
        expect(() => r._renderPipeline({})).toThrow(TypeError);
        await tick();
        expect(gpu.scopes).toEqual([]);
        expect(r._refused.size).toBe(0);
    });
});
