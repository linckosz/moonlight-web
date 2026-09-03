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
 * VideoRenderer — common interface for the video output strategies.
 *
 * Two implementations sit behind this contract:
 *   - Canvas2DRenderer : the legacy Canvas 2D path (feature Off / no WebGPU).
 *   - WebGpuRenderer    : upscaling/sharpening via WGSL (added in later commits).
 *
 * The renderer OWNS the canvas context: a canvas can only ever hold one context
 * type ('2d' XOR 'webgpu'), so callers must never call getContext themselves.
 * Both the main thread (StreamView rAF loop) and the OffscreenCanvas worker
 * (VideoDecodeWorker) delegate to a renderer instance.
 *
 * Out of the renderer (kept by callers, unchanged): the serialization guard
 * (_rendering / S.rendering, anti HEVC race), drop-to-latest, and stats counters.
 */
export class VideoRenderer {
    /**
     * Timing of the LAST completed draw(), for the pipeline diagnostics
     * (PipelineDiag.noteDraw). Split deliberately:
     *   submitMs — work this renderer actually does (pixel conversion, command
     *              encoding, the synchronous canvas call).
     *   waitMs   — time spent waiting on something else to be ready: the GPU
     *              queue, an image-bitmap upload, the canvas swap chain. This
     *              is where presentation back-pressure shows up, and it is what
     *              makes the "Render" leg grow with the framerate without the
     *              draw itself getting any more expensive.
     *   path     — which code path produced the frame (renderers have several).
     * @type {{submitMs: number, waitMs: number, path: string}}
     */
    lastDraw = { submitMs: 0, waitMs: 0, path: '' };

    /**
     * Async factory — subclasses implement it: set up the context and resources.
     * @param {HTMLCanvasElement|OffscreenCanvas} canvas
     * @param {object} opts
     * @returns {Promise<VideoRenderer>}
     */
    static async create(canvas, opts) {
        throw new Error('VideoRenderer.create is abstract');
    }

    /**
     * Display (output) size in device pixels. Canvas2D backs the frame
     * resolution and lets CSS stretch, so this is a no-op there; WebGPU scales
     * to this size via the shader.
     */
    setOutputSize(width, height) {}

    /**
     * Whether this renderer must be given one frame at a time (see
     * RenderPacing). False here: WebGPU is safe to pipeline because commands
     * are executed in submission order, so two overlapping draws still present
     * in order. Canvas2D overrides it — a path that awaits before touching the
     * canvas could otherwise let an older frame land last.
     */
    get serialDrawsOnly() {
        return false;
    }

    /**
     * Switch the enhancement pass at runtime. Only the WebGPU path has one;
     * everywhere else this is inert, so the governor can call it blind.
     * @param {string} algo
     * @returns {boolean} true when the renderer switched.
     */
    setAlgo(algo) {
        return false;
    }

    /**
     * Click-to-photon probe support (see stream/LatencyProbe.js). A canvas that
     * presents through a desynchronized swap chain (WebGL2 here) cannot be
     * read back with drawImage once it is on screen — the drawing buffer is
     * gone after present — so the renderer reads the probe's three pixels
     * itself, right after the draw, while `probeActive` is set. `probePixels`
     * is `undefined` when the renderer does not need this (the probe then
     * samples the canvas), `null` while no frame has been drawn since the
     * probe was armed, else the 12 RGBA values in blue/white/red column order.
     */
    set probeActive(on) {}
    get probePixels() {
        return undefined;
    }

    /**
     * Draw one VideoFrame and CLOSE it (always, even on error). Returns a
     * promise that resolves when the frame has been consumed. Does NOT touch
     * the caller's stats counters.
     * @param {VideoFrame} frame
     * @returns {Promise<void>}
     */
    async draw(frame) {
        throw new Error('VideoRenderer.draw is abstract');
    }

    /**
     * Context-loss handling for the main-thread rAF loop. Canvas2D overrides
     * these (its 2D context can be lost and recreated); WebGPU recovers on its
     * own via device.lost, so the defaults are inert.
     */
    isContextLost() {
        return false;
    }
    recreateContext() {}

    /** Release context/GPU resources. */
    dispose() {}

    /** Async-init renderers report readiness here; Canvas2D is always ready. */
    get ready() {
        return true;
    }

    /** Renderer kind for the stats overlay: 'canvas2d' | 'webgpu'. */
    get kind() {
        return 'base';
    }

    /**
     * Whether the renderer is currently presenting an HDR surface. Only the
     * WebGPU and <video> paths can be; Canvas2D never is, so the default
     * completes the contract for it. Callers already coerce with `!!`, so this
     * returns exactly what they observed before (`undefined` → `false`).
     */
    get hdrActive() {
        return false;
    }
}
