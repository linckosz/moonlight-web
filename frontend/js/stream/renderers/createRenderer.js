/**
 * createVideoRenderer — picks the output renderer for the canvas path. WebGPU is
 * the preferred renderer on all devices (opts.webgpu defaults on), with a
 * transparent fallback to Canvas2D when WebGPU is unavailable or init fails.
 * Kept in its own module to avoid a circular import (the subclasses import VideoRenderer).
 *
 * The WebGPU attempt is safe to fall back from: WebGpuRenderer.create runs
 * requestAdapter/requestDevice BEFORE getContext('webgpu'), so a failure there
 * leaves the canvas clean for Canvas2D.
 */
import { Canvas2DRenderer } from './Canvas2DRenderer.js';
import { WebGpuRenderer } from './WebGpuRenderer.js';

export async function createVideoRenderer(canvas, opts) {
    if (opts.webgpu && navigator.gpu) {
        try {
            return await WebGpuRenderer.create(canvas, opts);
        } catch (e) {
            console.warn('[Renderer] WebGPU init failed, falling back to Canvas2D: ' + e.message);
        }
    }
    return await Canvas2DRenderer.create(canvas, opts);
}
