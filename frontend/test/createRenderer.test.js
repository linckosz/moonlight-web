/*
 * Moonlight-Web — TNR suite. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 *
 * The real renderers touch WebGPU/Canvas/<video> and are out of scope; we mock
 * them and assert only the selection/fallback logic of createVideoRenderer.
 */
import { describe, it, expect, beforeEach, afterEach, vi } from 'vitest';

vi.mock('../js/stream/renderers/Canvas2DRenderer.js', () => ({
    Canvas2DRenderer: { create: vi.fn(async () => ({ kind: 'canvas2d' })) },
}));
vi.mock('../js/stream/renderers/WebGpuRenderer.js', () => ({
    WebGpuRenderer: { create: vi.fn(async () => ({ kind: 'webgpu' })) },
}));
vi.mock('../js/stream/renderers/VideoElementRenderer.js', () => ({
    VideoElementRenderer: { create: vi.fn(async () => ({ kind: 'video' })) },
}));

import { createVideoRenderer } from '../js/stream/renderers/createRenderer.js';
import { Canvas2DRenderer } from '../js/stream/renderers/Canvas2DRenderer.js';
import { WebGpuRenderer } from '../js/stream/renderers/WebGpuRenderer.js';
import { VideoElementRenderer } from '../js/stream/renderers/VideoElementRenderer.js';

const canvas = {};

describe('createVideoRenderer', () => {
    beforeEach(() => {
        vi.clearAllMocks();
        WebGpuRenderer.create.mockResolvedValue({ kind: 'webgpu' });
        VideoElementRenderer.create.mockResolvedValue({ kind: 'video' });
        Canvas2DRenderer.create.mockResolvedValue({ kind: 'canvas2d' });
    });
    afterEach(() => vi.unstubAllGlobals());

    it('routes HDR to the <video> renderer when supported', async () => {
        vi.stubGlobal('MediaStreamTrackGenerator', function () {});
        vi.stubGlobal('navigator', { gpu: {} });
        const r = await createVideoRenderer(canvas, { hdr: true, videoEl: {}, webgpu: true });
        expect(r.kind).toBe('video');
        expect(VideoElementRenderer.create).toHaveBeenCalled();
    });

    it('prefers WebGPU when available', async () => {
        vi.stubGlobal('navigator', { gpu: {} });
        const r = await createVideoRenderer(canvas, { webgpu: true });
        expect(r.kind).toBe('webgpu');
    });

    it('falls back to Canvas2D when WebGPU is unavailable', async () => {
        vi.stubGlobal('navigator', {});
        const r = await createVideoRenderer(canvas, { webgpu: true });
        expect(r.kind).toBe('canvas2d');
    });

    it('falls back to Canvas2D when WebGPU init throws', async () => {
        vi.stubGlobal('navigator', { gpu: {} });
        WebGpuRenderer.create.mockRejectedValue(new Error('no device'));
        const r = await createVideoRenderer(canvas, { webgpu: true });
        expect(r.kind).toBe('canvas2d');
    });

    it('falls back from a failing <video> HDR renderer to the canvas path', async () => {
        vi.stubGlobal('MediaStreamTrackGenerator', function () {});
        vi.stubGlobal('navigator', { gpu: {} });
        VideoElementRenderer.create.mockRejectedValue(new Error('no MSTG'));
        const r = await createVideoRenderer(canvas, { hdr: true, videoEl: {}, webgpu: true });
        expect(r.kind).toBe('webgpu'); // HDR failed → WebGPU still preferred
    });
});
