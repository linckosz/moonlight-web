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
    isContextLost() { return false; }
    recreateContext() {}

    /** Release context/GPU resources. */
    dispose() {}

    /** Async-init renderers report readiness here; Canvas2D is always ready. */
    get ready() { return true; }

    /** Renderer kind for the stats overlay: 'canvas2d' | 'webgpu'. */
    get kind() { return 'base'; }
}
