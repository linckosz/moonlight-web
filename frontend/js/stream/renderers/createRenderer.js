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
import { VideoElementRenderer } from './VideoElementRenderer.js';

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
    return await Canvas2DRenderer.create(canvas, opts);
}
