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
 * WebGpuRenderer — the precision of MWSR. The kernel runs its f32 text on every
 * GPU: the device is not asked for shader-f16, and no module enables it, on an
 * adapter that offers it, whoever made it, and after a device lost.
 */
/* global setTimeout */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { WebGpuRenderer } from '../js/stream/renderers/WebGpuRenderer.js';

const tick = () => new Promise((res) => setTimeout(res));

/** A device that grants what it is asked for, and an adapter that offers shader-f16. */
function fakeGpu(vendor) {
    const modules = [];
    const pass = { setPipeline: vi.fn(), setBindGroup: vi.fn(), draw: vi.fn(), end: vi.fn() };
    const encoder = {
        beginRenderPass: vi.fn(() => pass),
        beginComputePass: vi.fn(() => pass),
        finish: () => ({}),
        clearBuffer: vi.fn(),
        copyTextureToBuffer: vi.fn(),
    };
    const mkDevice = (desc) => ({
        features: new Set((desc && desc.requiredFeatures) || []),
        limits: {},
        lost: new Promise(() => {}),
        addEventListener() {},
        destroy() {},
        createSampler: () => ({}),
        createBindGroupLayout: () => ({}),
        createPipelineLayout: () => ({}),
        createShaderModule: vi.fn((d) => {
            modules.push(d.code);
            return { getCompilationInfo: async () => ({ messages: [] }) };
        }),
        pushErrorScope: vi.fn(),
        popErrorScope: vi.fn(async () => null),
        createRenderPipeline: vi.fn(() => ({})),
        createComputePipeline: () => ({}),
        createBindGroup: vi.fn(() => ({})),
        createBuffer: () => ({ destroy() {} }),
        createTexture: vi.fn((d) => {
            const t = { desc: d, destroy() {} };
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
    });
    const adapter = {
        info: { vendor },
        features: new Set(['shader-f16', 'timestamp-query']),
        limits: {},
        requestDevice: vi.fn(async (desc) => mkDevice(desc)),
    };
    const ctx = {
        configure: vi.fn((cfg) => void cfg.toneMapping),
        unconfigure: vi.fn(),
        getCurrentTexture: () => ({ createView: () => ({ canvas: true }) }),
    };
    const canvas = { width: 0, height: 0, getContext: (k) => (k === 'webgpu' ? ctx : null) };
    return { adapter, canvas, modules };
}

async function renderer(gpu, opts) {
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
    r.setOutputSize(2560, 1440);
    await tick();
    return r;
}

describe('WebGpuRenderer — MWSR in f32', () => {
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
            INDEX: 16,
            VERTEX: 32,
            UNIFORM: 64,
            STORAGE: 128,
            INDIRECT: 256,
            QUERY_RESOLVE: 512,
        });
    });
    afterEach(() => {
        vi.unstubAllGlobals();
    });

    for (const vendor of ['amd', 'apple', 'nvidia', 'arm', '']) {
        it(`does not ask for shader-f16 on an adapter of « ${vendor} » that offers it`, async () => {
            const gpu = fakeGpu(vendor);
            await renderer(gpu, {});
            expect(gpu.adapter.requestDevice).toHaveBeenCalledTimes(1);
            expect(gpu.adapter.requestDevice.mock.calls[0]).toEqual([]);
            expect(gpu.modules.length).toBeGreaterThan(0);
            expect(gpu.modules.some((code) => code.includes('enable f16'))).toBe(false);
            expect(gpu.modules.some((code) => code.includes('alias hf = f32;'))).toBe(true);
        });
    }

    it('stays in f32 in HDR', async () => {
        const gpu = fakeGpu('amd');
        await renderer(gpu, { hdr: true, hdrLinear: true });
        expect(gpu.modules.some((code) => code.includes('enable f16'))).toBe(false);
    });

    it('stays in f32 on the device it recovers', async () => {
        const gpu = fakeGpu('amd');
        const r = await renderer(gpu, {});
        await r._recover();
        expect(gpu.adapter.requestDevice).toHaveBeenCalledTimes(2);
        expect(gpu.adapter.requestDevice.mock.calls[1]).toEqual([]);
        expect(gpu.modules.some((code) => code.includes('enable f16'))).toBe(false);
    });
});
