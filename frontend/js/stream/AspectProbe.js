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

import { parseAspect, resolveMeasuredAspect } from '../util/AspectRatio.js';

/**
 * Work out the host's real screen format from the black bars Sunshine encodes.
 *
 * Sunshine does not resize its desktop to the resolution we ask for: it scales
 * its capture into that frame and PADS the remainder with black. Those bars are
 * the only trace of the host's true ratio that reaches us — serverinfo carries
 * no DisplayMode and no REST route exposes the display. Measuring them lets the
 * stream ask for a frame the host fills edge to edge, which removes the bars
 * and stops paying bitrate for them.
 *
 * Nothing in the pixels distinguishes padding from black that belongs to the
 * picture — a pillarboxed 4:3 video looks exactly like a 4:3 host. One property
 * separates them: padding is there from the very first frame and never moves,
 * while content arrives later. So the probe only ever looks at the first few
 * seconds, demands that the bars be identical throughout, and then stops for
 * good. A video started afterwards can no longer reshape the stream.
 *
 * There is one case where nothing has to be inferred at all: a host that does
 * not honour the frame we asked for — a virtual display snapping to a supported
 * mode (Wolf, MultiSeat/Apollo), a display-managed backend imposing its own —
 * encodes its own shape, and the decoded frame states it outright. That short
 * circuit runs first, and it is also the ONLY thing still watched once the bar
 * window has closed: the host changing resolution mid-session is the one event
 * no picture can imitate, so it may reshape a running stream where a bar
 * measurement never may.
 */

// Sampling window. Short on purpose: it must close before the user has had time
// to open anything, and every sample has to agree with the first one.
const SAMPLE_INTERVAL_MS = 250;
const WINDOW_MS = 3000;
const MIN_SAMPLES = 4;

// A pixel counts as black below this on every channel. Limited-range black
// decodes to ~16, so the threshold sits just above it.
const BLACK_LEVEL = 20;

// Bars thinner than this are encoder ringing, not padding.
const MIN_BAR_FRACTION = 0.02;

// Two samples describe the same bars when every edge agrees within this.
const EDGE_TOLERANCE_PX = 2;

// How far the decoded shape has to be from the requested one to mean the host
// refused the request. WebGpuRenderer rounds its backing size to whole pixels,
// which moves the ratio by a small fraction of this.
const FRAME_ASPECT_TOLERANCE = 0.01;

// Watch cadence once the verdict is in: two attribute reads a second, no pixel
// work at all. Two readings must agree before it speaks, and it stays quiet for
// a while after any verdict — the startup one included, which covers the
// seconds during which the correction that verdict triggered is still being
// applied and the old stream is still the one on screen.
const WATCH_INTERVAL_MS = 1000;
const WATCH_CONFIRMATIONS = 2;
const MIN_WATCH_REPORT_MS = 10000;

// Probe lines: three columns (and three rows) at 25/50/75%. A bar has to be
// black on all three, so a dark strip in the picture cannot pass for padding.
const PROBE_FRACTIONS = [0.25, 0.5, 0.75];

/**
 * Bar thicknesses from already-sampled edge pixels. Pure — the canvas work
 * lives in measureBars() — so the arithmetic can be exercised without a DOM.
 *
 * @param {Uint8ClampedArray} cols RGBA of an n×h strip: the n probe columns.
 * @param {Uint8ClampedArray} rows RGBA of a w×n strip: the n probe rows.
 * @returns {{top:number,bottom:number,left:number,right:number,uniform:boolean}}
 */
export function scanBars(cols, rows, w, h, n) {
    const rowIsBlack = (y) => {
        for (let i = 0; i < n; i++) {
            const p = (y * n + i) * 4;
            if (cols[p] > BLACK_LEVEL || cols[p + 1] > BLACK_LEVEL || cols[p + 2] > BLACK_LEVEL)
                return false;
        }
        return true;
    };
    let top = 0;
    while (top < h && rowIsBlack(top)) top++;
    // Every sampled row is black: the picture carries no signal at this instant
    // (host screen off, black desktop). Undecidable, not "all padding".
    if (top >= h) return { top: 0, bottom: 0, left: 0, right: 0, uniform: true };
    let bottom = 0;
    while (bottom < h - top && rowIsBlack(h - 1 - bottom)) bottom++;

    const colIsBlack = (x) => {
        for (let i = 0; i < n; i++) {
            const p = (i * w + x) * 4;
            if (rows[p] > BLACK_LEVEL || rows[p + 1] > BLACK_LEVEL || rows[p + 2] > BLACK_LEVEL)
                return false;
        }
        return true;
    };
    let left = 0;
    while (left < w && colIsBlack(left)) left++;
    if (left >= w) return { top: 0, bottom: 0, left: 0, right: 0, uniform: true };
    let right = 0;
    while (right < w - left && colIsBlack(w - 1 - right)) right++;

    return { top, bottom, left, right, uniform: false };
}

/**
 * Sample the edges of one frame: three columns and three rows at 25/50/75%,
 * copied into a strip small enough to read back cheaply.
 * @returns {{top:number,bottom:number,left:number,right:number,uniform:boolean}}
 */
function measureBars(ctx, el, w, h) {
    const canvas = ctx.canvas;
    const n = PROBE_FRACTIONS.length;

    canvas.width = n;
    canvas.height = h;
    for (let i = 0; i < n; i++) {
        const x = Math.min(w - 1, Math.max(0, Math.round(w * PROBE_FRACTIONS[i])));
        ctx.drawImage(el, x, 0, 1, h, i, 0, 1, h);
    }
    const cols = ctx.getImageData(0, 0, n, h).data;

    canvas.width = w;
    canvas.height = n;
    for (let i = 0; i < n; i++) {
        const y = Math.min(h - 1, Math.max(0, Math.round(h * PROBE_FRACTIONS[i])));
        ctx.drawImage(el, 0, y, w, 1, 0, i, w, 1);
    }
    const rows = ctx.getImageData(0, 0, w, n).data;

    return scanBars(cols, rows, w, h, n);
}

/** Same bars, within the per-edge tolerance. */
function sameBars(a, b) {
    return (
        Math.abs(a.top - b.top) <= EDGE_TOLERANCE_PX &&
        Math.abs(a.bottom - b.bottom) <= EDGE_TOLERANCE_PX &&
        Math.abs(a.left - b.left) <= EDGE_TOLERANCE_PX &&
        Math.abs(a.right - b.right) <= EDGE_TOLERANCE_PX
    );
}

/**
 * Turn a stable measurement into the aspect to request. Pure, and exported
 * alongside scanBars() so the whole verdict path is testable without a DOM.
 * @returns {{aspect: string|null, reason: string}}
 */
export function decideAspect(bars, w, h) {
    if (bars.uniform) return { aspect: null, reason: 'uniform' };

    const minV = h * MIN_BAR_FRACTION;
    const minH = w * MIN_BAR_FRACTION;
    const hasV = bars.top >= minV || bars.bottom >= minV;
    const hasH = bars.left >= minH || bars.right >= minH;

    // No bar worth the name: the frame we asked for is already the host's own
    // format. Nothing to correct.
    if (!hasV && !hasH) return { aspect: null, reason: 'no-bars' };
    // A scaler pads on ONE axis. Bars on both is picture, not padding.
    if (hasV && hasH) return { aspect: null, reason: 'both-axes' };

    if (hasV && Math.abs(bars.top - bars.bottom) > EDGE_TOLERANCE_PX)
        return { aspect: null, reason: 'asymmetric' };
    if (hasH && Math.abs(bars.left - bars.right) > EDGE_TOLERANCE_PX)
        return { aspect: null, reason: 'asymmetric' };

    const contentW = w - bars.left - bars.right;
    const contentH = h - bars.top - bars.bottom;
    const aspect = resolveMeasuredAspect(contentW, contentH);
    // Off the white list of real screen formats → almost certainly picture.
    return { aspect, reason: aspect ? 'measured' : 'implausible' };
}

/**
 * The decoded frame's own shape, when it contradicts the one we asked for.
 *
 * Nothing is being inferred here: the host encoded that shape because it would
 * not give us ours. Unlike a bar measurement there is no content that could be
 * faking it, so a ratio the white list does not know is still the host's answer
 * and is passed on as measured. Pure, and exported for the tests.
 *
 * @returns {string|null} the aspect to request, or null when the host honoured
 *          the request (or when there is nothing to compare against).
 */
export function frameAspect(width, height, requested) {
    const want = parseAspect(requested);
    if (!want || !(width > 0) || !(height > 0)) return null;
    const got = width / height;
    if (Math.abs(got - want) / want <= FRAME_ASPECT_TOLERANCE) return null;
    return resolveMeasuredAspect(width, height) || Math.round(width) + ':' + Math.round(height);
}

/** A <canvas> that has never been presented to still reports the HTML default,
 *  whose 2:1 would read as a deliberate host format. */
function isBlankCanvas(surface) {
    return surface.width === 300 && surface.height === 150;
}

/**
 * Start a one-shot probe on the live stream surface.
 *
 * @param {object} opts
 * @param {() => ({el: HTMLVideoElement|HTMLCanvasElement, width: number,
 *                height: number}|null)} opts.getSurface current display surface
 *        and its INTRINSIC size (never the CSS box — object-fit: contain adds
 *        bars of its own on the client side).
 * @param {() => string} [opts.getRequestedAspect] aspect the live stream was
 *        launched with, re-read on every tick: the correction this probe
 *        triggers changes it, and comparing against a frozen value would make
 *        the probe answer itself.
 * @param {(aspect: string|null, reason: string) => void} opts.onResult fires
 *        once with the startup verdict — the aspect to request, or null when
 *        undecided — and again only if the host later changes the shape it
 *        encodes.
 * @returns {() => void} stop, safe to call at any time (onResult never fires
 *          afterwards).
 */
export function startAspectProbe({ getSurface, getRequestedAspect, onResult }) {
    let ctx = null;
    let reference = null;
    let samples = 0;
    let startedAt = 0;
    let timer = null;
    let stopped = false; // stop() called: nothing fires again
    let measured = false; // the bar window is closed, the watch has taken over

    let watchTimer = null;
    let watchPending = null;
    let watchStable = 0;
    let lastReport = 0;

    const requested = () => (getRequestedAspect ? getRequestedAspect() : null);

    const stop = () => {
        stopped = true;
        if (timer) clearInterval(timer);
        timer = null;
        if (watchTimer) clearInterval(watchTimer);
        watchTimer = null;
        ctx = null;
    };

    const report = (aspect, reason) => {
        lastReport = performance.now();
        console.log('[AspectProbe] ' + reason + (aspect ? ' → ' + aspect : ''));
        try {
            onResult(aspect, reason);
        } catch (e) {
            console.warn('[AspectProbe] result handler failed:', e);
        }
    };

    /**
     * The verdict is in, but the host can still change shape under us — someone
     * changing its resolution mid-session. Only its own encoded shape may
     * reopen the question: the bars never speak again, so a film letterboxed
     * for two hours cannot reshape a running stream.
     */
    const watchTick = () => {
        if (stopped) return;
        const surface = getSurface && getSurface();
        if (!surface || !(surface.width > 0) || !(surface.height > 0) || isBlankCanvas(surface)) {
            watchPending = null;
            watchStable = 0;
            return;
        }
        const stated = frameAspect(surface.width, surface.height, requested());
        if (!stated) {
            watchPending = null;
            watchStable = 0;
            return;
        }
        // Two readings in a row: mid-transition the previous stream is still on
        // screen while the settings already carry the new aspect.
        if (stated === watchPending) watchStable++;
        else {
            watchPending = stated;
            watchStable = 1;
        }
        if (watchStable < WATCH_CONFIRMATIONS) return;
        // The application may decline to act (a relaunch in flight, a share
        // pinning the stream). The condition still holds, so ask again later
        // rather than hammering it.
        if (performance.now() - lastReport < MIN_WATCH_REPORT_MS) return;
        watchPending = null;
        watchStable = 0;
        report(stated, 'frame-aspect');
    };

    const finish = (aspect, reason) => {
        if (stopped || measured) return;
        measured = true;
        if (timer) clearInterval(timer);
        timer = null;
        ctx = null;
        report(aspect, reason);
        if (!stopped && !watchTimer) watchTimer = setInterval(watchTick, WATCH_INTERVAL_MS);
    };

    const tick = () => {
        if (stopped || measured) return;
        const surface = getSurface && getSurface();
        if (!surface || !surface.el || !(surface.width > 0) || !(surface.height > 0)) return;
        if (isBlankCanvas(surface)) return;
        // A <video> with no decoded data yet cannot be drawn (a <canvas> has no
        // readyState and is always drawable).
        if ('readyState' in surface.el && surface.el.readyState < 2) return;

        // The host may have refused the frame we asked for, in which case it
        // has already told us its shape and there is nothing to measure.
        const stated = frameAspect(surface.width, surface.height, requested());
        if (stated) {
            finish(stated, 'frame-aspect');
            return;
        }

        let bars;
        try {
            if (!ctx) {
                // willReadFrequently: this canvas exists only to be read back.
                ctx = document
                    .createElement('canvas')
                    .getContext('2d', { willReadFrequently: true });
                if (!ctx) {
                    finish(null, 'no-2d-context');
                    return;
                }
            }
            bars = measureBars(ctx, surface.el, surface.width, surface.height);
        } catch (e) {
            // Tainted canvas, or a GPU surface that refuses to be read back.
            finish(null, 'unreadable: ' + (e && e.message ? e.message : e));
            return;
        }

        if (!reference) {
            reference = bars;
            startedAt = performance.now();
            samples = 1;
            return;
        }
        // The padding never moves. Anything that does is picture, and the whole
        // measurement is void — no second chance this session.
        if (!sameBars(reference, bars)) {
            finish(null, 'unstable');
            return;
        }
        samples++;

        if (performance.now() - startedAt >= WINDOW_MS && samples >= MIN_SAMPLES) {
            const verdict = decideAspect(reference, surface.width, surface.height);
            finish(verdict.aspect, verdict.reason);
        }
    };

    timer = setInterval(tick, SAMPLE_INTERVAL_MS);
    tick();

    return stop;
}
