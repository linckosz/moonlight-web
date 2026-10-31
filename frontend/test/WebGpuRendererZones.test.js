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
 * WebGpuRenderer — MWSR's zones from the native host (plan B,
 * docs/design/upscaler-mwsr-b.md §12 c). The page asked for them
 * (opts.zoneMap): the `up` kernel's second text is built with the first, a
 * frame's zones wait by its timestamp, and the draw takes the second text,
 * the zones' drops in a texture, only where they fit the scale it draws at.
 * Driven against a fake device: what matters is the pipeline and the bind
 * group each frame's pass takes, and what goes into the texture.
 */
/* global console */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { VideoRenderer } from '../js/stream/renderers/VideoRenderer.js';
import { WebGpuRenderer } from '../js/stream/renderers/WebGpuRenderer.js';
import { mwsrZoneTexels } from '../js/stream/MwsrZoneBlock.js';

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
        createRenderPipeline: vi.fn((desc) => ({
            code: desc.fragment.module.code,
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
        getCurrentTexture: () => ({ createView: () => ({}) }),
    };
    const canvas = { width: 0, height: 0, getContext: (k) => (k === 'webgpu' ? ctx : null) };
    return { device, adapter, pass, canvas };
}

/** A 640×360 frame, drawn up to 1280×720 (×2) unless the output says otherwise. */
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

/** 32 × 18 zones decided for `scale`, every code `code` or a pattern of them all. */
function zones(scale, code) {
    const codes = Uint8Array.from({ length: 576 }, (_, i) => code ?? i % 12);
    return { cols: 32, rows: 18, scale, codes };
}

/** The pipeline and the bind group of the draw's last render pass. */
function last(gpu) {
    const p = gpu.pass.setPipeline.mock.calls;
    const g = gpu.pass.setBindGroup.mock.calls;
    return { pipeline: p[p.length - 1][0], group: g[g.length - 1][1] };
}

const zoneWrites = (gpu, r) =>
    gpu.device.queue.writeTexture.mock.calls.filter(([dst]) => dst.texture === r._zoneTex);

describe("WebGpuRenderer — mwsr's zones from the native host", () => {
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

    it('builds nothing of them, and keeps none, unless the page asked', async () => {
        expect(new VideoRenderer().wantsFrameZones).toBe(false);
        const r = await renderer(fakeGpu(), { algo: 'mwsr' });
        expect(r.wantsFrameZones).toBe(false);
        expect(r._mwsrPipelines.has('upmap')).toBe(false);
        r.noteFrameZones(100, zones(2));
        expect(r._zonesByTs.size).toBe(0);
        const s = await renderer(fakeGpu(), { algo: 'sgsr', zoneMap: true });
        expect(s.wantsFrameZones).toBe(false);
    });

    it("builds the kernel's second text with the first, which alone reads binding 3", async () => {
        const r = await renderer(fakeGpu(), { algo: 'mwsr', zoneMap: true });
        expect(r.wantsFrameZones).toBe(true);
        expect(r._mwsrPipelines.get('upmap').code).toContain(
            '@group(0) @binding(3) var zoneMap : texture_2d<f32>;',
        );
        expect(r._mwsrPipelines.get('up').code).not.toContain('zoneMap');
    });

    it('draws a frame with its zones by the second text, their drops in an r16float texture', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr', zoneMap: true });
        const z = zones(2);
        r.noteFrameZones(100, z);
        await r.draw(frame(100));
        const { pipeline, group } = last(gpu);
        expect(pipeline).toBe(r._mwsrPipelines.get('upmap'));
        expect(r._zoneTex.desc).toMatchObject({
            size: { width: 32, height: 18 },
            format: 'r16float',
        });
        const writes = zoneWrites(gpu, r);
        expect(writes).toHaveLength(1);
        expect(Array.from(writes[0][1])).toEqual(Array.from(mwsrZoneTexels(z.codes)));
        expect(writes[0][2]).toEqual({ bytesPerRow: 64 });
        expect(writes[0][3]).toEqual({ width: 32, height: 18 });
        expect(group.desc.layout).toBe(r._mwsrMapLayout);
        expect(group.desc.entries[1].resource).toBe(r._inputTexView);
        expect(group.desc.entries[3].resource).toBe(r._zoneTexView);
        expect(r.mwsrZoneFrames).toBe(1);
        expect(console.error).not.toHaveBeenCalled();
    });

    it('writes the texture again only when the codes change', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr', zoneMap: true });
        r.noteFrameZones(1, zones(2, 3));
        r.noteFrameZones(2, zones(2, 3));
        r.noteFrameZones(3, zones(2, 4));
        for (const t of [1, 2, 3]) await r.draw(frame(t));
        expect(zoneWrites(gpu, r)).toHaveLength(2);
        expect(
            gpu.device.createTexture.mock.calls.filter(([d]) => d.format === 'r16float'),
        ).toHaveLength(1);
        expect(r.mwsrZoneFrames).toBe(3);
    });

    it('takes the law for a frame without zones, or whose zones do not fit the scale', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr', zoneMap: true });
        const up = r._mwsrPipelines.get('up');
        await r.draw(frame(5));
        expect(last(gpu).pipeline).toBe(up);
        // decided for ×1.33, drawn at ×2: a zoom, a window of another size
        r.noteFrameZones(6, zones(4 / 3));
        await r.draw(frame(6));
        expect(last(gpu).pipeline).toBe(up);
        expect(r._zonesByTs.size).toBe(0);
        // ×1.25: under the bench's scales, whatever the host decided for
        r.setOutputSize(800, 450);
        r.noteFrameZones(7, zones(1.25));
        await r.draw(frame(7));
        expect(last(gpu).pipeline).toBe(up);
        expect(zoneWrites(gpu, r)).toHaveLength(0);
        expect(r.mwsrZoneFrames).toBe(0);
    });

    it('reads none on a reduction', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, { algo: 'mwsr', zoneMap: true });
        r.setOutputSize(320, 180);
        r.noteFrameZones(8, zones(0.5));
        await r.draw(frame(8));
        expect(last(gpu).pipeline).not.toBe(r._mwsrPipelines.get('upmap'));
        expect(r._zoneTex).toBe(null);
        expect(r._zonesByTs.size).toBe(0);
    });

    it('forgets the zones of frames the decoder dropped, and keeps at most 64', async () => {
        const r = await renderer(fakeGpu(), { algo: 'mwsr', zoneMap: true });
        for (let t = 1; t <= 100; t++) r.noteFrameZones(t, zones(2));
        expect(r._zonesByTs.size).toBe(64);
        await r.draw(frame(90));
        expect([...r._zonesByTs.keys()]).toEqual(Array.from({ length: 10 }, (_, i) => 91 + i));
        // frame 95 takes its own, and 91 to 94 go with it
        await r.draw(frame(95));
        expect([...r._zonesByTs.keys()]).toEqual([96, 97, 98, 99, 100]);
    });

    describe("the eye test's split screen", () => {
        const splitWrites = (gpu, r) =>
            gpu.device.queue.writeBuffer.mock.calls
                .filter(([buf]) => buf === r._splitUniform)
                .map(([, , data]) => Array.from(data));

        it('draws a frame with its zones by both texts, then the two side by side', async () => {
            const gpu = fakeGpu();
            const r = await renderer(gpu, {
                algo: 'mwsr',
                zoneMap: true,
                mwsrSplit: { gridLeft: false, view: 'left-twice' },
            });
            r.noteFrameZones(10, zones(2));
            const passes = gpu.device.createCommandEncoder().beginRenderPass.mock.calls.length;
            await r.draw(frame(10));
            const pipes = gpu.pass.setPipeline.mock.calls.map(([p]) => p).slice(-3);
            expect(pipes[0]).toBe(r._mwsrPipelines.get('upmap'));
            expect(pipes[1]).toBe(r._mwsrPipelines.get('up'));
            expect(pipes[2]).toBe(r._splitPipeline);
            expect(r._splitPipeline.code).toContain('textureLoad(gridTex');
            const calls = gpu.device
                .createCommandEncoder()
                .beginRenderPass.mock.calls.slice(passes);
            const views = calls.slice(-3).map(([d]) => d.colorAttachments[0].view);
            expect(views[0]).toBe(r._splitTex.views[0]);
            expect(views[1]).toBe(r._splitTex.views[1]);
            expect(r._splitTex.tex[0].desc).toMatchObject({
                size: { width: 1280, height: 720 },
                format: 'bgra8unorm',
            });
            // the law's bind group: the first text's, on inputTex
            const groups = gpu.pass.setBindGroup.mock.calls.map(([, g]) => g).slice(-3);
            expect(groups[0].desc.layout).toBe(r._mwsrMapLayout);
            expect(groups[1]).toBe(r._mwsrBindGroup);
            expect(groups[2]).toBe(r._splitGroup);
            // view 1 (the left half twice), the grid on the right, 1280 wide
            expect(splitWrites(gpu, r)).toEqual([[1, 0, 1280, 0]]);
            expect(r.mwsrZoneFrames).toBe(1);
            expect(console.error).not.toHaveBeenCalled();
        });

        it('draws a frame without zones by the law alone, whole', async () => {
            const gpu = fakeGpu();
            const r = await renderer(gpu, {
                algo: 'mwsr',
                zoneMap: true,
                mwsrSplit: { gridLeft: true, view: 'halves' },
            });
            await r.draw(frame(11));
            expect(last(gpu).pipeline).toBe(r._mwsrPipelines.get('up'));
            expect(r._splitPipeline).toBe(null);
        });

        it('changes view live, keeps its textures for one size, and goes off', async () => {
            const gpu = fakeGpu();
            const r = await renderer(gpu, {
                algo: 'mwsr',
                zoneMap: true,
                mwsrSplit: { gridLeft: true, view: 'centre-twice' },
            });
            r.noteFrameZones(1, zones(2));
            await r.draw(frame(1));
            const tex = r._splitTex;
            r.setMwsrSplit({ gridLeft: true, view: 'halves' });
            r.noteFrameZones(2, zones(2));
            await r.draw(frame(2));
            expect(r._splitTex).toBe(tex);
            expect(splitWrites(gpu, r)).toEqual([
                [0, 1, 1280, 0],
                [2, 1, 1280, 0],
            ]);
            r.setMwsrSplit({ gridLeft: true, view: 'nonsense' });
            expect(r._split).toBe(null);
            r.setMwsrSplit(null);
            r.noteFrameZones(3, zones(2));
            await r.draw(frame(3));
            expect(last(gpu).pipeline).toBe(r._mwsrPipelines.get('upmap'));
        });
    });
});
