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
 * Canvas2DRenderer — the legacy Canvas 2D output path (feature Off / no WebGPU).
 *
 * Faithful extraction of the rendering code previously duplicated between
 * StreamView._drawFrameWithBitmap (main thread) and VideoDecodeWorker.drawFrame
 * (OffscreenCanvas worker). Backing buffer = frame resolution, CSS stretches to
 * the display box (status quo). Behavior is unchanged — every HEVC NV12 fallback
 * is preserved; the serialization guard stays in the caller.
 *
 * Platform-specific workarounds for browser bugs:
 *   Windows Chrome HEVC NV12 : D3D11 compositor ghosting → createImageBitmap
 *     primary + 'copy' composite (replaces all pixels regardless of alpha).
 *   All other cases (H.264, AV1, Safari, Edge) : direct drawImage(VideoFrame)
 *     first (skips a per-frame GPU copy), createImageBitmap as fallback.
 */
import { VideoRenderer } from './VideoRenderer.js';
import { CODEC_HEVC } from '../../util/Mp4Muxer.js';

export class Canvas2DRenderer extends VideoRenderer {
    constructor() {
        super();
        // Declared here so the instance shape is explicit; `create()` is the
        // only construction path and overwrites all of them before returning.
        /** @type {string} 'h264' | 'hevc' | 'av1'. */
        this.videoCodec = '';
        /** @type {boolean} Gate for the NV12 'copy' path. */
        this.isChromeWindowsHevc = false;
        /** @type {number} Frames drawn — drives the one-shot warm-up logging. */
        this._rendered = 0;
        /**
         * Debug 'smooth2d' mode: back the canvas at the display size and draw
         * the frame scaled with `smoothingQuality`, instead of a frame-sized
         * backing stretched by CSS. Off by default (status quo).
         */
        this.scaleToOutput = false;
        /** @type {string|null} imageSmoothingQuality when scaleToOutput. */
        this.smoothingQuality = null;
        this._outW = 0;
        this._outH = 0;
    }

    /**
     * @param {HTMLCanvasElement|OffscreenCanvas} canvas
     * @param {object} opts
     * @param {boolean} opts.desynchronized  Low-latency context (no vsync compose).
     * @param {string}  opts.videoCodec       'h264' | 'hevc' | 'av1'.
     * @param {boolean} opts.isChromeWindowsHevc  Gate for the NV12 'copy' path.
     */
    static async create(canvas, opts) {
        const r = new Canvas2DRenderer();
        r.canvas = canvas;
        r.videoCodec = opts.videoCodec;
        // Mutable so callers can update it if the platform flag is resolved after
        // creation (kept identical to the previous live-read behavior).
        r.isChromeWindowsHevc = !!opts.isChromeWindowsHevc;
        r.scaleToOutput = opts.scaleToOutput === true;
        r.smoothingQuality = r.scaleToOutput ? opts.smoothingQuality || 'high' : null;
        // desynchronized:true bypasses vsync composition (lower latency, possible
        // tearing). The worker always passes true; the main thread gates it on VSync.
        r.ctx = opts.desynchronized
            ? canvas.getContext('2d', { desynchronized: true })
            : canvas.getContext('2d');
        // Initial size; adjusted to match the first frame in draw().
        canvas.width = 1920;
        canvas.height = 1080;
        r._rendered = 0;
        if (r.scaleToOutput)
            console.log('[Canvas2DRenderer] scaling to output, smoothing=' + r.smoothingQuality);
        return r;
    }

    get kind() {
        return 'canvas2d';
    }

    /** Overlay name for the debug 'smooth2d' mode; null in the ordinary mode. */
    get algoName() {
        return this.scaleToOutput ? 'Canvas2D smoothing ' + this.smoothingQuality : null;
    }

    setOutputSize(width, height) {
        // Ordinarily a no-op: the Canvas2D backing follows the frame resolution
        // and the display box scales it via CSS. In 'smooth2d' the backing
        // follows the display size instead and draw() resamples into it.
        if (width > 0 && height > 0) {
            this._outW = width;
            this._outH = height;
        }
    }

    /** Canvas size draw() wants for this frame: display box (smooth2d) or frame. */
    _targetSize(frame) {
        const inW = frame.displayWidth || frame.codedWidth || 0;
        const inH = frame.displayHeight || frame.codedHeight || 0;
        if (!this.scaleToOutput || this._outW <= 0 || this._outH <= 0 || inW <= 0 || inH <= 0)
            return [inW, inH];
        const frameAspect = inW / inH;
        const boxAspect = this._outW / this._outH;
        const cw = frameAspect >= boxAspect ? this._outW : Math.round(this._outH * frameAspect);
        return [cw, Math.round(cw / frameAspect)];
    }

    /**
     * Resizing a canvas resets its 2D context state, imageSmoothingQuality
     * included — so the filter is re-armed after every resize, not once.
     */
    _applySmoothing() {
        if (!this.smoothingQuality || !this.ctx) return;
        try {
            this.ctx.imageSmoothingEnabled = true;
            this.ctx.imageSmoothingQuality = this.smoothingQuality;
        } catch (e) {}
    }

    isContextLost() {
        // Safari/WebKit lacks CanvasRenderingContext2D.isContextLost().
        return this.ctx && typeof this.ctx.isContextLost === 'function'
            ? this.ctx.isContextLost()
            : false;
    }

    recreateContext() {
        // Recovery uses the default (composited) context, matching the original.
        if (this.canvas) {
            this.ctx = this.canvas.getContext('2d');
            this._applySmoothing();
        }
    }

    async draw(frame) {
        if (!this.canvas || !this.ctx) {
            try {
                frame.close();
            } catch (e) {}
            return;
        }

        // Diagnostics: everything awaited is "wait" (GPU upload / readback), the
        // rest is this renderer's own work — which on an accelerated canvas
        // includes the synchronous drawImage blocking on a full swap chain, the
        // signature of presentation back-pressure. See VideoRenderer.lastDraw.
        const drawStart = performance.now();
        let waitMs = 0;

        // Resize the backing buffer to the frame size when needed (or to the
        // display box in 'smooth2d', see _targetSize).
        const [tw, th] = this._targetSize(frame);
        if (tw > 0 && th > 0 && (this.canvas.width !== tw || this.canvas.height !== th)) {
            this.canvas.width = tw;
            this.canvas.height = th;
            this._applySmoothing();
        }

        const isHevcNv12 =
            this.videoCodec === CODEC_HEVC && frame.format === 'NV12' && this.isChromeWindowsHevc;

        if (isHevcNv12) {
            // ── HEVC NV12: createImageBitmap(VideoFrame) PRIMARY + 'copy' ─────
            // Keeps NV12→RGBA conversion on the GPU (alpha=255 everywhere); the
            // await must precede any canvas mutation. 'copy' replaces all pixels.
            //   1. createImageBitmap(VideoFrame) → drawImage(bitmap, 'copy')
            //   2. ctx.drawImage(VideoFrame, 'copy')          (some Safari)
            //   3. copyTo(RGBA) → ImageData → putImageData     (last resort)
            let bitmap = null;
            const bitmapStart = performance.now();
            try {
                bitmap = await createImageBitmap(frame);
            } catch (e) {}
            waitMs += performance.now() - bitmapStart;

            this.ctx.save();
            this.ctx.globalCompositeOperation = 'copy';

            let success = false;
            if (bitmap) {
                this.ctx.drawImage(bitmap, 0, 0, this.canvas.width, this.canvas.height);
                bitmap.close();
                success = true;
            }
            if (!success) {
                try {
                    this.ctx.drawImage(frame, 0, 0, this.canvas.width, this.canvas.height);
                    success = true;
                } catch (e) {
                    console.warn(
                        '[Canvas2DRenderer] ctx.drawImage(VideoFrame) failed: ' + e.message,
                    );
                }
            }
            if (!success) {
                try {
                    const w = frame.displayWidth || frame.codedWidth || 0;
                    const h = frame.displayHeight || frame.codedHeight || 0;
                    if (w > 0 && h > 0) {
                        const size = w * h * 4;
                        const buf = new ArrayBuffer(size);
                        const copyStart = performance.now();
                        await frame.copyTo(buf, { format: 'RGBA' });
                        waitMs += performance.now() - copyStart;
                        const imageData = new ImageData(new Uint8ClampedArray(buf, 0, size), w, h);
                        this.ctx.putImageData(imageData, 0, 0);
                        success = true;
                    }
                } catch (e) {
                    console.error('[Canvas2DRenderer] All HEVC render paths failed:', e.message);
                }
            }

            this.ctx.restore();

            // Force GPU sync on the first frames to flush stale compositor caches;
            // per-frame readback would cost ~1-3ms, so only the first 30.
            if (this._rendered < 30) {
                try {
                    this.ctx.getImageData(0, 0, 1, 1);
                } catch (e) {}
            }

            frame.close();
            this._rendered++;
            this._noteDraw(drawStart, waitMs, bitmap ? 'nv12-bitmap' : 'nv12-fallback');
            return;
        }

        // ── Standard path (H.264 / AV1 / non-NV12 / Safari) ──────────────────
        // Try direct drawImage(VideoFrame) first: it skips the per-frame await
        // createImageBitmap (GPU copy + microtask gap). createImageBitmap is the
        // fallback for browsers where direct VideoFrame drawing fails.
        let rendered = false;
        let path = 'drawImage';
        try {
            this.ctx.drawImage(frame, 0, 0, this.canvas.width, this.canvas.height);
            rendered = true;
        } catch (e) {}

        if (!rendered) {
            path = 'bitmap';
            try {
                const bitmapStart = performance.now();
                const bitmap = await createImageBitmap(frame);
                waitMs += performance.now() - bitmapStart;
                this.ctx.drawImage(bitmap, 0, 0, this.canvas.width, this.canvas.height);
                bitmap.close();
                rendered = true;
            } catch (e) {
                try {
                    this.ctx.drawImage(frame, 0, 0, this.canvas.width, this.canvas.height);
                    rendered = true;
                } catch (e2) {
                    try {
                        path = 'copyTo';
                        const w = frame.displayWidth || frame.codedWidth || 0;
                        const h = frame.displayHeight || frame.codedHeight || 0;
                        if (w > 0 && h > 0) {
                            const size = w * h * 4;
                            const buf = new ArrayBuffer(size);
                            const copyStart = performance.now();
                            await frame.copyTo(buf, { format: 'RGBA' });
                            waitMs += performance.now() - copyStart;
                            const imageData = new ImageData(
                                new Uint8ClampedArray(buf, 0, size),
                                w,
                                h,
                            );
                            this.ctx.putImageData(imageData, 0, 0);
                            rendered = true;
                        }
                    } catch (e3) {
                        console.error('[Canvas2DRenderer] All render paths failed:', e3.message);
                    }
                }
            }
        }

        frame.close();
        this._rendered++;
        this._noteDraw(drawStart, waitMs, rendered ? path : 'failed');
    }

    /**
     * Canvas2D may only be pipelined on its synchronous path: `drawImage` of a
     * VideoFrame mutates the canvas before draw() ever suspends, so two
     * overlapping draws still land in order. Every other path awaits first
     * (createImageBitmap, copyTo) — two of them in flight could resolve out of
     * order and present an older frame last, and on Chrome Windows HEVC NV12
     * they would additionally race on the same VideoFrame.
     *
     * Read from the last draw rather than guessed: which path a browser takes
     * is stable, but only observable once it has run. Before the first draw the
     * answer is "serial", which is the safe default.
     */
    get serialDrawsOnly() {
        return this.lastDraw.path !== 'drawImage';
    }

    /** Record the draw split for PipelineDiag (see VideoRenderer.lastDraw). */
    _noteDraw(startMs, waitMs, path) {
        const total = performance.now() - startMs;
        this.lastDraw.submitMs = Math.max(0, total - waitMs);
        this.lastDraw.waitMs = waitMs;
        this.lastDraw.path = path;
    }

    dispose() {
        this.ctx = null;
        this.canvas = null;
    }
}
