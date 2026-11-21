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
import { videoSinkCtor } from './videoSink.js';

/**
 * Enhancer choices that run WITHOUT WebGPU: the WebGL2 shaders (WEBGL_ALGOS:
 * the three ports, and 'gl-mwsr' and 'gl-mwsr-q', MoonlightWeb's own in its
 * two modes) and 'smooth2d' (Canvas2D drawing at the display size with its
 * best resampling filter). The caller keeps opts.webgpu false for all of
 * them; anything not in this list ('sgsr' / 'nis' / 'fsr1' / 'mwsr' /
 * 'mwsr-q' / 'off') is handled by the WebGPU or Canvas2D path.
 */
export const NO_WEBGPU_ALGOS = ['smooth2d', ...WEBGL_ALGOS];

/**
 * Firefox presents the plain SDR stream on WebGL2, not on Canvas2D.
 *
 * Gecko's drawImage(VideoFrame) on a 2D canvas is a synchronous copy on the
 * main thread. Measured on the N95 (Firefox 157, UHD Graphics, hardware H.264,
 * 12/10/2026): 38 ms a 720p frame and 55 ms a 1080p one, where texImage2D on
 * WebGL2 took 2 and 4 ms with a readback to wait on it. At 60 fps the decoder's
 * outputs then queue behind the draws, the decode queue overflows, and every
 * keyframe asked for to clear it overflows again: an IDR every 300 ms, for
 * good. Chrome draws the same frame in 0.5 ms and keeps Canvas2D (the
 * 03/09/2026 defaults, measured on Mac and Windows).
 *
 * Only the plain path moves (plainPicture).
 * @param {{gecko: boolean, hdr: boolean, algo: string, webgpu: boolean,
 *          sink: boolean, forceCanvas2d?: boolean, devCanvas2d?: boolean}} s
 */
export function geckoPresentsOnPlainWebGl(s) {
    return s.gecko === true && plainPicture(s);
}

/**
 * iOS and iPadOS present the plain SDR stream on WebGL2 too, for the pinch zoom.
 *
 * WebKit shows a 2D canvas at its box's size on the glass whatever its backing
 * holds: zoomed by the CSS transform, a 1440p frame on a phone was reduced to
 * the ~1179 px of the box first and that reduction magnified, so text went to
 * mush at 2× (iPhone, 1440p stream, 20/10/2026) where the enhancer, on WebGL,
 * stayed sharp. A WebGL canvas is shown from its own backing, and the renderer
 * grows that backing with the zoom step (StreamView._outputZoomScale).
 * @param {{ios: boolean, hdr: boolean, algo: string, webgpu: boolean,
 *          sink: boolean, forceCanvas2d?: boolean, devCanvas2d?: boolean}} s
 */
export function iosPresentsOnPlainWebGl(s) {
    return s.ios === true && plainPicture(s);
}

/**
 * The plain picture: the one stream that may go on either Canvas2D or WebGL2
 * with nothing to lose (geckoPresentsOnPlainWebGl, RendererFallback). An
 * enhancer already runs on WebGL or WebGPU, HDR has its own routing, and the
 * debug menu's Canvas2D stays Canvas2D.
 * @param {{hdr: boolean, algo: string, webgpu: boolean, sink: boolean,
 *          forceCanvas2d?: boolean, devCanvas2d?: boolean}} s
 */
export function plainPicture(s) {
    return !s.hdr && s.algo === 'off' && !s.webgpu && !s.sink && !s.forceCanvas2d && !s.devCanvas2d;
}

/**
 * Where the picture goes when its renderer gives up (VideoRenderer.failed):
 * the pass it ran, on its WebGL2 twin, under the setting as the ladder's
 * ceiling; the plain copy where the setting has no pass, on the presenter the
 * page would use for one (plainWebGl). createVideoRenderer answers with
 * Canvas2D where WebGL2 refuses (Bruno, 22/10/2026: `gl-<algo>`, then
 * Canvas2D, rather than black).
 * @param {{ceiling: string, pass: string, plainWebGl?: boolean}} s
 *   ceiling the setting and pass the rung the renderer was on, as rungs
 *   ('fsr1', 'nis', 'sgsr', 'mwsr' or 'off'; see EnhancerGovernor.ladderRung)
 * @returns {{algo: string, ceiling: string, plainWebGl: boolean}}
 *   the options of createVideoRenderer that change
 */
export function rendererAfterFailure(s) {
    const twin = 'gl-' + s.ceiling;
    if (!WEBGL_ALGOS.includes(twin))
        return { algo: 'off', ceiling: 'off', plainWebGl: s.plainWebGl === true };
    const pass = 'gl-' + s.pass;
    return { algo: WEBGL_ALGOS.includes(pass) ? pass : 'off', ceiling: twin, plainWebGl: false };
}

/**
 * Whether this view's frames wait for the screen's vsync: what the native host
 * is told (`client_vsync` at /start, then `clientrefresh`), to run the stream
 * at a divisor of this screen's refresh and to open its window after a press
 * only for a client that tears (bench §8w.9).
 *
 * "Allow tearing" is a wish, and /start can only say the wish: the renderer is
 * made later. Only a canvas that skips the compositor makes it real, Canvas2D
 * or WebGL2 with the desynchronized hint. WebGPU has no such hint, and a
 * <video> element (the HDR sink, a TV, the media transport) is the compositor's
 * to show: both paint at the vsync whatever the setting. Every HDR stream lands
 * on one of the two.
 * @param {{tearing: boolean, transport: string, kind: ?string}} s
 *        `kind` the renderer's (VideoRenderer.kind), null before it is made:
 *        the setting is then all there is to go on
 * @returns {boolean}
 */
export function presentsOnVsync(s) {
    if (!s.tearing || s.transport === 'webrtc-media') return true;
    if (!s.kind) return false;
    return s.kind !== 'canvas2d' && s.kind !== 'webgl';
}

export async function createVideoRenderer(canvas, opts) {
    // The product's sessions draw mwsr by its modes (MwsrShader's MWSR_MODES):
    // the pass 'mwsr' is `performance`, the kernel pushed, and 'mwsr-q' is
    // `quality`. The laboratory makes its renderers itself, without this
    // option, and keeps the law alone under 'mwsr'.
    opts = { mwsrMode: 'performance', ...opts };
    // HDR: route decoded frames to a <video> element via the track generator.
    // The canvas paths (WebGPU/Canvas2D) tone-map HDR away (importExternalTexture /
    // drawImage only output SDR color spaces) — <video> presents HDR natively. Needs
    // a DOM <video> (opts.videoEl), so it is main-thread only (not the worker path).
    // opts.forceVideo: the same sink on purpose, HDR or not (the debug menu's
    // "Video element" entry, to measure it against the canvas presenters).
    if ((opts.hdr || opts.forceVideo) && opts.videoEl && videoSinkCtor()) {
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
    // throws before asking for one or after losing it for good. The setting
    // (`ceiling`) picks the renderer, not the pass it starts with: a session
    // the enhancer ladder starts on its plain rung stays on WebGL, so it can
    // climb back.
    if (WEBGL_ALGOS.includes(opts.ceiling || opts.algo)) {
        try {
            return await WebGlRenderer.create(canvas, opts);
        } catch (e) {
            console.warn('[Renderer] WebGL init failed, falling back to Canvas2D: ' + e.message);
        }
    }
    // The plain copy of Firefox and iOS (geckoPresentsOnPlainWebGl,
    // iosPresentsOnPlainWebGl): the same WebGL renderer with no pass, and the
    // same safe fallback.
    if (opts.plainWebGl) {
        try {
            return await WebGlRenderer.create(canvas, { ...opts, algo: 'off' });
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
