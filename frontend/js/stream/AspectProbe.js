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

import { resolveMeasuredAspect } from '../util/AspectRatio.js';

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
 * Start a one-shot probe on the live stream surface.
 *
 * @param {object} opts
 * @param {() => ({el: HTMLVideoElement|HTMLCanvasElement, width: number,
 *                height: number}|null)} opts.getSurface current display surface
 *        and its INTRINSIC size (never the CSS box — object-fit: contain adds
 *        bars of its own on the client side).
 * @param {(aspect: string|null, reason: string) => void} opts.onResult fires
 *        exactly once, with the aspect to request or null when undecided.
 * @returns {() => void} stop, safe to call at any time (onResult never fires
 *          afterwards).
 */
export function startAspectProbe({ getSurface, onResult }) {
    let ctx = null;
    let reference = null;
    let samples = 0;
    let startedAt = 0;
    let timer = null;
    let finished = false;

    const stop = () => {
        finished = true;
        if (timer) clearInterval(timer);
        timer = null;
        ctx = null;
    };

    const finish = (aspect, reason) => {
        if (finished) return;
        stop();
        console.log('[AspectProbe] ' + reason + (aspect ? ' → ' + aspect : ''));
        try {
            onResult(aspect, reason);
        } catch (e) {
            console.warn('[AspectProbe] result handler failed:', e);
        }
    };

    const tick = () => {
        if (finished) return;
        const surface = getSurface && getSurface();
        if (!surface || !surface.el || !(surface.width > 0) || !(surface.height > 0)) return;
        // A <video> with no decoded data yet cannot be drawn (a <canvas> has no
        // readyState and is always drawable).
        if ('readyState' in surface.el && surface.el.readyState < 2) return;

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
