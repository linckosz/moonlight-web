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
        // desynchronized:true bypasses vsync composition (lower latency, possible
        // tearing). The worker always passes true; the main thread gates it on VSync.
        r.ctx = opts.desynchronized
            ? canvas.getContext('2d', { desynchronized: true })
            : canvas.getContext('2d');
        // Initial size; adjusted to match the first frame in draw().
        canvas.width = 1920;
        canvas.height = 1080;
        r._rendered = 0;
        return r;
    }

    get kind() { return 'canvas2d'; }

    setOutputSize(width, height) {
        // No-op: the Canvas2D backing follows the frame resolution; the display
        // box scales it via CSS.
    }

    isContextLost() {
        // Safari/WebKit lacks CanvasRenderingContext2D.isContextLost().
        return (this.ctx && typeof this.ctx.isContextLost === 'function')
            ? this.ctx.isContextLost()
            : false;
    }

    recreateContext() {
        // Recovery uses the default (composited) context, matching the original.
        if (this.canvas) this.ctx = this.canvas.getContext('2d');
    }

    async draw(frame) {
        if (!this.canvas || !this.ctx) { try { frame.close(); } catch (e) {} return; }

        // Resize the backing buffer to the frame size when needed.
        if (frame.displayWidth && frame.displayHeight &&
            (this.canvas.width !== frame.displayWidth ||
             this.canvas.height !== frame.displayHeight)) {
            this.canvas.width = frame.displayWidth;
            this.canvas.height = frame.displayHeight;
        }

        const isHevcNv12 = this.videoCodec === CODEC_HEVC && frame.format === 'NV12' &&
            this.isChromeWindowsHevc;

        if (isHevcNv12) {
            // ── HEVC NV12: createImageBitmap(VideoFrame) PRIMARY + 'copy' ─────
            // Keeps NV12→RGBA conversion on the GPU (alpha=255 everywhere); the
            // await must precede any canvas mutation. 'copy' replaces all pixels.
            //   1. createImageBitmap(VideoFrame) → drawImage(bitmap, 'copy')
            //   2. ctx.drawImage(VideoFrame, 'copy')          (some Safari)
            //   3. copyTo(RGBA) → ImageData → putImageData     (last resort)
            let bitmap = null;
            try { bitmap = await createImageBitmap(frame); } catch (e) {}

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
                    console.warn('[Canvas2DRenderer] ctx.drawImage(VideoFrame) failed: ' + e.message);
                }
            }
            if (!success) {
                try {
                    const w = frame.displayWidth || frame.codedWidth || 0;
                    const h = frame.displayHeight || frame.codedHeight || 0;
                    if (w > 0 && h > 0) {
                        const size = w * h * 4;
                        const buf = new ArrayBuffer(size);
                        await frame.copyTo(buf, { format: 'RGBA' });
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
            if (this._rendered < 30) { try { this.ctx.getImageData(0, 0, 1, 1); } catch (e) {} }

            frame.close();
            this._rendered++;
            return;
        }

        // ── Standard path (H.264 / AV1 / non-NV12 / Safari) ──────────────────
        // Try direct drawImage(VideoFrame) first: it skips the per-frame await
        // createImageBitmap (GPU copy + microtask gap). createImageBitmap is the
        // fallback for browsers where direct VideoFrame drawing fails.
        let rendered = false;
        try {
            this.ctx.drawImage(frame, 0, 0, this.canvas.width, this.canvas.height);
            rendered = true;
        } catch (e) {}

        if (!rendered) {
            try {
                const bitmap = await createImageBitmap(frame);
                this.ctx.drawImage(bitmap, 0, 0, this.canvas.width, this.canvas.height);
                bitmap.close();
                rendered = true;
            } catch (e) {
                try {
                    this.ctx.drawImage(frame, 0, 0, this.canvas.width, this.canvas.height);
                    rendered = true;
                } catch (e2) {
                    try {
                        const w = frame.displayWidth || frame.codedWidth || 0;
                        const h = frame.displayHeight || frame.codedHeight || 0;
                        if (w > 0 && h > 0) {
                            const size = w * h * 4;
                            const buf = new ArrayBuffer(size);
                            await frame.copyTo(buf, { format: 'RGBA' });
                            const imageData = new ImageData(new Uint8ClampedArray(buf, 0, size), w, h);
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
    }

    dispose() {
        this.ctx = null;
        this.canvas = null;
    }
}
