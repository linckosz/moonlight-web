/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

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
import { WebGlRenderer, WEBGL_ALGOS } from './WebGlRenderer.js';
import { VideoElementRenderer } from './VideoElementRenderer.js';

/**
 * Enhancer choices that run WITHOUT WebGPU: the three WebGL2 shaders and
 * 'smooth2d' (Canvas2D drawing at the display size with its best resampling
 * filter). The caller keeps opts.webgpu false for all of them; anything not in
 * this list ('sgsr' / 'nis' / 'fsr1' / 'off') is handled by the WebGPU or
 * Canvas2D path.
 */
export const NO_WEBGPU_ALGOS = ['smooth2d', ...WEBGL_ALGOS];

export async function createVideoRenderer(canvas, opts) {
    // HDR: route decoded frames to a <video> element via MediaStreamTrackGenerator.
    // The canvas paths (WebGPU/Canvas2D) tone-map HDR away (importExternalTexture /
    // drawImage only output SDR color spaces) — <video> presents HDR natively. Needs
    // a DOM <video> (opts.videoEl), so it is main-thread only (not the worker path).
    if (opts.hdr && opts.videoEl && typeof MediaStreamTrackGenerator !== 'undefined') {
        try {
            return await VideoElementRenderer.create(canvas, opts);
        } catch (e) {
            console.warn(
                '[Renderer] VideoElement (HDR) init failed, falling back to canvas: ' + e.message,
            );
        }
    }
    if (opts.webgpu && navigator.gpu) {
        try {
            return await WebGpuRenderer.create(canvas, opts);
        } catch (e) {
            console.warn('[Renderer] WebGPU init failed, falling back to Canvas2D: ' + e.message);
        }
    }
    // WebGL enhancers. A failure here — no WebGL2, a shader the driver
    // rejects — is safe to fall back from: getContext('webgl2') returning null
    // leaves the canvas free, and a compile error happens before any draw. A
    // context that was obtained is committed, though, so the renderer only
    // throws before asking for one or after losing it for good.
    if (WEBGL_ALGOS.includes(opts.algo)) {
        try {
            return await WebGlRenderer.create(canvas, opts);
        } catch (e) {
            console.warn('[Renderer] WebGL init failed, falling back to Canvas2D: ' + e.message);
        }
    }
    return await Canvas2DRenderer.create(canvas, {
        ...opts,
        // 'smooth2d': back the canvas at the display size and let the 2D
        // context resample with imageSmoothingQuality 'high' instead of
        // leaving the stretch to CSS. Everything else keeps the frame-sized
        // backing and the CSS stretch.
        scaleToOutput: opts.algo === 'smooth2d',
        smoothingQuality: opts.algo === 'smooth2d' ? 'high' : null,
    });
}
