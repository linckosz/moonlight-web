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
 * WebGpuRenderer — the frame's way in when the browser refuses
 * importExternalTexture (Chrome on a machine whose decoded frames cannot be
 * shared with WebGPU, 21/10/2026: "Failed to import texture from video" on
 * every frame, a black canvas and a console line per frame), the two copies
 * that take over (an ImageBitmap, then the readback), and the renderer giving
 * up when no frame reaches its canvas any more. The renderer is driven against
 * a fake device: what matters is which path draw() takes, not what a GPU would
 * draw.
 */
/* global console, createImageBitmap, GPUTextureUsage */
import { describe, it, expect, vi, beforeEach, afterEach } from 'vitest';
import { WebGpuRenderer } from '../js/stream/renderers/WebGpuRenderer.js';

/** The renderer's LOST_FRAMES_LIMIT. */
const LOST_LIMIT = 30;

function fakeGpu({ importThrows = false, bitmapThrows = false } = {}) {
    const pass = { setPipeline: vi.fn(), setBindGroup: vi.fn(), draw: vi.fn(), end: vi.fn() };
    const encoder = {
        beginRenderPass: vi.fn(() => pass),
        finish: () => ({}),
        copyTextureToBuffer: vi.fn(),
    };
    const textures = [];
    const devices = [];
    // Each device its own `lost`, settled by gpu.lose(i, reason).
    const mkDevice = () => {
        let lose;
        const device = {
            features: new Set(),
            limits: {},
            lost: new Promise((res) => {
                lose = res;
            }),
            addEventListener() {},
            destroy() {},
            createSampler: () => ({}),
            createBindGroupLayout: () => ({}),
            createPipelineLayout: () => ({}),
            createShaderModule: () => ({ getCompilationInfo: async () => ({ messages: [] }) }),
            pushErrorScope() {},
            popErrorScope: async () => null,
            createRenderPipeline: vi.fn(() => ({})),
            createComputePipeline: () => ({}),
            // A missing layout throws, as in a browser ('off' has none of SGSR's).
            createBindGroup: vi.fn((desc) => {
                if (!desc || !desc.layout)
                    throw new TypeError('required member layout is undefined');
                return {};
            }),
            createBuffer: () => ({
                destroy() {},
                mapAsync: async () => {},
                getMappedRange: () => new ArrayBuffer(256),
                unmap() {},
            }),
            createTexture: vi.fn((desc) => {
                const t = { desc, createView: () => ({}), destroy() {} };
                textures.push(t);
                return t;
            }),
            createCommandEncoder: vi.fn(() => encoder),
            importExternalTexture: vi.fn(() => {
                if (importThrows) throw new Error('Failed to import texture from video');
                return {};
            }),
            queue: {
                submit: vi.fn(),
                writeBuffer: vi.fn(),
                writeTexture: vi.fn(),
                onSubmittedWorkDone: vi.fn(async () => {}),
                copyExternalImageToTexture: vi.fn(() => {
                    if (bitmapThrows)
                        throw new Error('Failed to copy content from external image.');
                }),
            },
        };
        device.lose = lose;
        devices.push(device);
        return device;
    };
    const device = mkDevice();
    const adapter = {
        info: { vendor: 'test' },
        features: new Set(),
        limits: {},
        // The first device, then a new one at every recovery.
        requestDevice: vi.fn(async () =>
            adapter.requestDevice.mock.calls.length === 1 ? device : mkDevice(),
        ),
    };
    const ctx = {
        configure: vi.fn(),
        unconfigure: vi.fn(),
        getCurrentTexture: () => ({ createView: () => ({}) }),
    };
    const canvas = {
        width: 0,
        height: 0,
        getContext: (kind) => (kind === 'webgpu' ? ctx : null),
    };
    return { device, devices, adapter, encoder, pass, textures, canvas, ctx };
}

/**
 * A 640×360 frame. `copyTo` reads it back as RGBA, the way Chrome converts;
 * `converts: false` is a browser that does not know copyTo's `format` member
 * (it never reads it, and would hand back the frame's own planes).
 */
function frame({ converts = true, copyThrows = false } = {}) {
    const f = {
        displayWidth: 640,
        displayHeight: 360,
        codedWidth: 640,
        codedHeight: 360,
        visibleRect: { x: 0, y: 0, width: 640, height: 360 },
        timestamp: 0,
        format: 'NV12',
        close: vi.fn(),
        allocationSize: vi.fn((opts) => {
            if (converts && opts && opts.format === 'RGBA') return 640 * 360 * 4;
            return (640 * 360 * 3) / 2;
        }),
        copyTo: vi.fn(async (buf, opts) => {
            if (copyThrows) throw new Error('copyTo refused');
            if (converts && opts && opts.format === 'RGBA') return [{ offset: 0, stride: 2560 }];
            return [
                { offset: 0, stride: 640 },
                { offset: 640 * 360, stride: 640 },
            ];
        }),
    };
    return f;
}

async function renderer(gpu, algo) {
    Object.defineProperty(globalThis.navigator, 'gpu', {
        value: {
            requestAdapter: vi.fn(async () => gpu.adapter),
            getPreferredCanvasFormat: () => 'bgra8unorm',
        },
        configurable: true,
    });
    const r = await WebGpuRenderer.create(gpu.canvas, { algo, videoCodec: 'h264' });
    r.setOutputSize(1280, 720);
    return r;
}

/** Let the microtasks of a settled `lost` (and a recovery) run. */
const settle = () => new Promise((res) => setTimeout(res, 0));

const warned = (text) => console.warn.mock.calls.filter((c) => String(c[0]).includes(text));

describe('WebGpuRenderer — a refused importExternalTexture', () => {
    let bitmaps;
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
        vi.stubGlobal('GPUMapMode', { READ: 1, WRITE: 2 });
        bitmaps = [];
        vi.stubGlobal(
            'createImageBitmap',
            vi.fn(async (f) => {
                const b = { width: f.displayWidth, height: f.displayHeight, close: vi.fn() };
                bitmaps.push(b);
                return b;
            }),
        );
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
    });
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
        delete globalThis.navigator.gpu;
    });

    it('imports the frame when the browser allows it, nothing copied', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, 'off');
        const f = frame();
        await r.draw(f);
        expect(gpu.device.importExternalTexture).toHaveBeenCalledTimes(1);
        expect(createImageBitmap).not.toHaveBeenCalled();
        expect(gpu.device.queue.copyExternalImageToTexture).not.toHaveBeenCalled();
        expect(f.copyTo).not.toHaveBeenCalled();
        expect(r.lastDraw.path).toBe('off');
        expect(f.close).toHaveBeenCalledTimes(1);
        expect(r.serialDrawsOnly).toBe(false);
        expect(r.failed).toBe(false);
    });

    it('copies the frame through an ImageBitmap into inputTex when the import throws, and warns once', async () => {
        const gpu = fakeGpu({ importThrows: true });
        const r = await renderer(gpu, 'off');
        const f1 = frame();
        await r.draw(f1);
        // the copy: the bitmap of the frame, into the input texture, at the frame's size
        expect(createImageBitmap).toHaveBeenCalledWith(f1);
        const copy = gpu.device.queue.copyExternalImageToTexture;
        expect(copy).toHaveBeenCalledTimes(1);
        const [src, dst, size] = copy.mock.calls[0];
        expect(src.source).toBe(bitmaps[0]);
        expect(dst.texture.desc.size).toEqual({ width: 640, height: 360 });
        expect(dst.texture.desc.usage & GPUTextureUsage.COPY_DST).toBeTruthy();
        expect(size).toEqual({ width: 640, height: 360 });
        expect(bitmaps[0].close).toHaveBeenCalledTimes(1);
        expect(f1.close).toHaveBeenCalledTimes(1);
        expect(f1.copyTo).not.toHaveBeenCalled();
        // the picture still goes to the canvas, by the texture copy
        expect(gpu.pass.draw).toHaveBeenCalledTimes(1);
        expect(gpu.device.queue.submit).toHaveBeenCalledTimes(1);
        expect(r.lastDraw.path).toBe('off+bitmap');
        // one line, not one per frame; the import is not asked again at once
        expect(console.warn).toHaveBeenCalledTimes(1);
        expect(console.error).not.toHaveBeenCalled();
        await r.draw(frame());
        await r.draw(frame());
        expect(gpu.device.importExternalTexture).toHaveBeenCalledTimes(1);
        expect(copy).toHaveBeenCalledTimes(3);
        expect(console.warn).toHaveBeenCalledTimes(1);
        expect(r.failed).toBe(false);
    });

    it('tries the import again later, and leaves the bitmap path when it works', async () => {
        const gpu = fakeGpu({ importThrows: true });
        const r = await renderer(gpu, 'off');
        await r.draw(frame());
        expect(r.lastDraw.path).toBe('off+bitmap');
        // long after the refusal, the import is asked again — and now works
        r._drawCount += 300;
        gpu.device.importExternalTexture.mockImplementation(() => ({}));
        await r.draw(frame());
        expect(gpu.device.importExternalTexture).toHaveBeenCalledTimes(2);
        expect(r.lastDraw.path).toBe('off');
        await r.draw(frame());
        expect(gpu.device.importExternalTexture).toHaveBeenCalledTimes(3);
        expect(gpu.device.queue.copyExternalImageToTexture).toHaveBeenCalledTimes(1);
    });

    it('skips pass 0 on the upscaler path when the copy already filled inputTex', async () => {
        const ok = fakeGpu();
        const r1 = await renderer(ok, 'sgsr');
        await r1.draw(frame());
        const passesWithImport = ok.encoder.beginRenderPass.mock.calls.length;
        expect(passesWithImport).toBe(2); // pass 0 then the kernel

        const refused = fakeGpu({ importThrows: true });
        const r2 = await renderer(refused, 'sgsr');
        await r2.draw(frame());
        expect(refused.encoder.beginRenderPass).toHaveBeenCalledTimes(1); // the kernel alone
        expect(refused.device.queue.copyExternalImageToTexture).toHaveBeenCalledTimes(1);
        expect(r2.lastDraw.path).toBe('sgsr+bitmap');
    });

    it('logs a failing draw once per message, not once per frame', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, 'off');
        gpu.device.createCommandEncoder.mockImplementation(() => {
            throw new Error('boom');
        });
        await r.draw(frame());
        await r.draw(frame());
        await r.draw(frame());
        expect(console.error).toHaveBeenCalledTimes(1);
        expect(String(console.error.mock.calls[0][0])).toContain('boom');
        gpu.device.createCommandEncoder.mockImplementation(() => {
            throw new Error('other');
        });
        await r.draw(frame());
        expect(console.error).toHaveBeenCalledTimes(2);
    });

    it('draws one frame at a time while the frames are copied in, and again two once the import works', async () => {
        const gpu = fakeGpu({ importThrows: true });
        const r = await renderer(gpu, 'off');
        expect(r.serialDrawsOnly).toBe(false);
        await r.draw(frame());
        // a copied frame awaits before its draw: two in flight could land out of order
        expect(r.serialDrawsOnly).toBe(true);
        r._drawCount += 300;
        gpu.device.importExternalTexture.mockImplementation(() => ({}));
        await r.draw(frame());
        expect(r.serialDrawsOnly).toBe(false);
    });
});

describe('WebGpuRenderer — the readback, when the bitmap copy fails too', () => {
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
        vi.stubGlobal('GPUMapMode', { READ: 1, WRITE: 2 });
        vi.stubGlobal(
            'createImageBitmap',
            vi.fn(async (f) => ({
                width: f.displayWidth,
                height: f.displayHeight,
                close: vi.fn(),
            })),
        );
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
    });
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
        delete globalThis.navigator.gpu;
    });

    it('reads the frame back as RGBA and writes it into a texture of its own, one warning a step', async () => {
        const gpu = fakeGpu({ importThrows: true, bitmapThrows: true });
        const r = await renderer(gpu, 'off');
        const f = frame();
        await r.draw(f);
        // the readback: copyTo converting to RGBA, into a buffer of the right size
        expect(f.copyTo).toHaveBeenCalledTimes(1);
        const [buf, opts] = f.copyTo.mock.calls[0];
        expect(opts).toEqual({ format: 'RGBA' });
        expect(buf.byteLength).toBe(640 * 360 * 4);
        // …written into an rgba8unorm texture at the frame's visible size
        const write = gpu.device.queue.writeTexture;
        expect(write).toHaveBeenCalledTimes(1);
        const [dst, data, layout, size] = write.mock.calls[0];
        expect(dst.texture.desc.format).toBe('rgba8unorm');
        expect(dst.texture.desc.size).toEqual({ width: 640, height: 360 });
        expect(dst.texture.desc.usage & GPUTextureUsage.COPY_DST).toBeTruthy();
        expect(data).toBe(buf);
        expect(layout).toEqual({ offset: 0, bytesPerRow: 2560, rowsPerImage: 360 });
        expect(size).toEqual({ width: 640, height: 360 });
        // …and the plain copy reads it to the canvas
        expect(gpu.pass.draw).toHaveBeenCalledTimes(1);
        expect(gpu.device.queue.submit).toHaveBeenCalledTimes(1);
        expect(r.lastDraw.path).toBe('off+readback');
        expect(f.close).toHaveBeenCalledTimes(1);
        expect(warned('importExternalTexture refused')).toHaveLength(1);
        expect(warned('ImageBitmap copy failed too')).toHaveLength(1);
        expect(console.error).not.toHaveBeenCalled();
        // the next frames go straight to the readback: no bitmap, the same buffer
        const f2 = frame();
        await r.draw(f2);
        await r.draw(frame());
        expect(gpu.device.queue.copyExternalImageToTexture).toHaveBeenCalledTimes(1);
        expect(f2.copyTo.mock.calls[0][0]).toBe(buf);
        expect(write).toHaveBeenCalledTimes(3);
        expect(console.warn).toHaveBeenCalledTimes(2);
        expect(r.failed).toBe(false);
    });

    it('runs pass 0 from the read-back texture on the upscaler path', async () => {
        const gpu = fakeGpu({ importThrows: true, bitmapThrows: true });
        const r = await renderer(gpu, 'sgsr');
        await r.draw(frame());
        expect(gpu.encoder.beginRenderPass).toHaveBeenCalledTimes(2); // pass 0, the kernel
        expect(r.lastDraw.path).toBe('sgsr+readback');
        // pass 0 targets inputTex's format: a pipeline of its own, made once
        const pipelines = gpu.device.createRenderPipeline.mock.calls.length;
        await r.draw(frame());
        expect(gpu.device.createRenderPipeline.mock.calls.length).toBe(pipelines);
    });

    it('asks for the bitmap again when it asks for the import again', async () => {
        const gpu = fakeGpu({ importThrows: true, bitmapThrows: true });
        const r = await renderer(gpu, 'off');
        await r.draw(frame());
        await r.draw(frame());
        expect(gpu.device.queue.copyExternalImageToTexture).toHaveBeenCalledTimes(1);
        r._drawCount += 300;
        gpu.device.queue.copyExternalImageToTexture.mockImplementation(() => {});
        await r.draw(frame());
        expect(gpu.device.importExternalTexture).toHaveBeenCalledTimes(2);
        expect(gpu.device.queue.copyExternalImageToTexture).toHaveBeenCalledTimes(2);
        expect(r.lastDraw.path).toBe('off+bitmap');
    });

    it('does not read back where copyTo cannot convert to RGBA', async () => {
        const gpu = fakeGpu({ importThrows: true, bitmapThrows: true });
        const r = await renderer(gpu, 'off');
        const f = frame({ converts: false });
        await r.draw(f);
        // the frame's own planes would have shown as colours: no copy, no write
        expect(f.copyTo).not.toHaveBeenCalled();
        expect(gpu.device.queue.writeTexture).not.toHaveBeenCalled();
        expect(gpu.device.queue.submit).not.toHaveBeenCalled();
        expect(r.lastDraw.path).toBe('off+lost');
        expect(f.close).toHaveBeenCalledTimes(1);
        expect(warned('readback failed too')).toHaveLength(1);
    });
});

describe('WebGpuRenderer — giving up', () => {
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
        vi.stubGlobal('GPUMapMode', { READ: 1, WRITE: 2 });
        vi.stubGlobal(
            'createImageBitmap',
            vi.fn(async (f) => ({
                width: f.displayWidth,
                height: f.displayHeight,
                close: vi.fn(),
            })),
        );
        vi.spyOn(console, 'log').mockImplementation(() => {});
        vi.spyOn(console, 'warn').mockImplementation(() => {});
        vi.spyOn(console, 'error').mockImplementation(() => {});
    });
    afterEach(() => {
        vi.unstubAllGlobals();
        vi.restoreAllMocks();
        delete globalThis.navigator.gpu;
    });

    it('gives up when no way takes a frame in, LOST_FRAMES_LIMIT frames in a row', async () => {
        const gpu = fakeGpu({ importThrows: true, bitmapThrows: true });
        const r = await renderer(gpu, 'mwsr');
        const frames = [];
        for (let i = 0; i < LOST_LIMIT - 1; i++) {
            const f = frame({ copyThrows: true });
            frames.push(f);
            await r.draw(f);
        }
        expect(r.failed).toBe(false);
        // nothing went to the GPU, and every frame was closed
        expect(gpu.device.queue.submit).not.toHaveBeenCalled();
        expect(frames.every((f) => f.close.mock.calls.length === 1)).toBe(true);
        // after a frame lost by every way, the import is asked again at once
        expect(gpu.device.importExternalTexture).toHaveBeenCalledTimes(LOST_LIMIT - 1);
        await r.draw(frame({ copyThrows: true }));
        expect(r.failed).toBe(true);
        expect(warned('giving up')).toHaveLength(1);
        expect(String(warned('giving up')[0][0])).toContain('no way in');
        // said once, however long it lasts
        await r.draw(frame({ copyThrows: true }));
        expect(warned('giving up')).toHaveLength(1);
    });

    it('starts the count again at a frame drawn', async () => {
        const gpu = fakeGpu({ importThrows: true, bitmapThrows: true });
        const r = await renderer(gpu, 'off');
        for (let i = 0; i < LOST_LIMIT - 1; i++) await r.draw(frame({ copyThrows: true }));
        await r.draw(frame());
        expect(r.lastDraw.path).toBe('off+readback');
        for (let i = 0; i < LOST_LIMIT - 1; i++) await r.draw(frame({ copyThrows: true }));
        expect(r.failed).toBe(false);
    });

    it('counts a frame whose work never completes as lost', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, 'sgsr');
        await r.draw(frame());
        gpu.device.queue.onSubmittedWorkDone.mockImplementation(async () => {
            throw new Error('A valid external Instance reference no longer exists.');
        });
        for (let i = 0; i < LOST_LIMIT; i++) await r.draw(frame());
        expect(gpu.device.queue.submit).toHaveBeenCalledTimes(LOST_LIMIT + 1);
        expect(r.failed).toBe(true);
        expect(String(warned('giving up')[0][0])).toContain('did not complete');
    });

    it('gives up when the device is destroyed before it finished a frame, without asking for another', async () => {
        // Linux on the software adapter (22/10/2026): the canvas finds no
        // texture to present, Chrome destroys the device, and every new one.
        const gpu = fakeGpu();
        const r = await renderer(gpu, 'mwsr');
        gpu.device.queue.onSubmittedWorkDone.mockImplementation(async () => {
            gpu.device.lose({ reason: 'destroyed', message: 'Device was destroyed.' });
            await settle();
            throw new Error('A valid external Instance reference no longer exists.');
        });
        await r.draw(frame());
        await settle();
        expect(r.failed).toBe(true);
        expect(String(warned('giving up')[0][0])).toContain('before it finished a frame');
        expect(navigator.gpu.requestAdapter).toHaveBeenCalledTimes(1); // create() alone
        // the next frames are dropped at the door, the renderer being down
        const f = frame();
        await r.draw(f);
        expect(f.close).toHaveBeenCalledTimes(1);
        expect(gpu.device.queue.submit).toHaveBeenCalledTimes(1);
    });

    it('recovers a device lost after it drew, and gives up if the new one is lost before a frame', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, 'off');
        await r.draw(frame());
        gpu.device.lose({ reason: 'unknown', message: 'GPU reset' });
        await settle();
        expect(navigator.gpu.requestAdapter).toHaveBeenCalledTimes(2);
        expect(r.failed).toBe(false);
        expect(r.ready).toBe(true);
        expect(r._device).toBe(gpu.devices[1]);
        // the new device draws…
        await r.draw(frame());
        expect(gpu.devices[1].queue.submit).toHaveBeenCalledTimes(1);
        // …and one lost again after that is recovered again
        gpu.devices[1].lose({ reason: 'unknown', message: 'GPU reset' });
        await settle();
        expect(r._device).toBe(gpu.devices[2]);
        // a device lost before it drew anything is the end
        gpu.devices[2].lose({ reason: 'unknown', message: 'GPU reset' });
        await settle();
        expect(r.failed).toBe(true);
        expect(navigator.gpu.requestAdapter).toHaveBeenCalledTimes(3);
    });

    it('gives up when the device cannot be recovered', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, 'off');
        await r.draw(frame());
        navigator.gpu.requestAdapter.mockImplementation(async () => null);
        gpu.device.lose({ reason: 'unknown', message: 'GPU reset' });
        await settle();
        expect(r.failed).toBe(true);
        expect(String(warned('giving up')[0][0])).toContain('could not be recovered');
    });

    it('is never failed on its own disposal', async () => {
        const gpu = fakeGpu();
        const r = await renderer(gpu, 'off');
        r.dispose();
        gpu.device.lose({ reason: 'destroyed', message: 'Device was destroyed.' });
        await settle();
        expect(r.failed).toBe(false);
    });
});
