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
 * LatencyProbe — click-to-photon measurement, browser side.
 *
 * The host (backend/src/LatencyFlag.cpp, debug builds on Windows) raises a
 * French flag at the top of its screen for 100 ms whenever a click is injected
 * into it. This side sends a click, stamps the moment, and watches the picture
 * it presents until the flag shows up. The difference is the whole loop the
 * player feels — input encoding, network, injection, capture, encode, network,
 * decode, presentation — which no software timestamp in the pipeline covers.
 *
 * Costs nothing while idle: no timer, no per-frame work. The hooks the stream
 * calls (`onFramePresented`) return on a single null check. During a
 * measurement (at most 200 ms per click) each presented frame costs one tiny
 * drawImage of the output surface into a 100×20 canvas and a 3-pixel read.
 *
 * Usage, from the console of a stream whose launch reply carried
 * `latency_flag: true`:
 *
 *     await mwLatency.run()          // 3 clicks, 2 s apart, prints a summary
 *     await mwLatency.run(10, 1500)  // 10 clicks, 1.5 s apart
 *     mwLatencyResults               // every entry ever measured, in order
 *
 * Each entry: { ts, latencyMs, fromMarkMs, ok, reason }
 *   ts         — click moment, microseconds since the Unix epoch (integer)
 *   latencyMs  — click → flag presented, or null when the flag never showed
 *   fromMarkMs — same, counted from the frame the grey mark was painted in
 *                (what a camera filming the client screen would see)
 *   ok         — false when the sample must be discarded (timeout, flag
 *                already up before the click, no picture to sample)
 *
 * The grey mark: a small circle at the pointer, painted for exactly one frame
 * at the click, so a slow-motion camera can pair the client's click with the
 * flag on either screen.
 */

/**
 * Flag rectangle as fractions of the host's primary screen — mirrors
 * LatencyFlag::kLeft..kBottom. The picture is assumed to show the whole
 * screen (Sunshine, Apollo and the native host all capture full-screen).
 */
export const FLAG_REGION = Object.freeze({ left: 0.44, right: 0.56, top: 0.0, bottom: 0.05 });

/** Past this, the flag is assumed lost (network hiccup) and the sample dropped. */
export const FLAG_TIMEOUT_MS = 200;

/** Output surface is scaled into this many columns × rows for sampling. */
const SAMPLE_W = 100;
const SAMPLE_H = 20;

/**
 * Sample columns, one per band, at the centre of each third of the flag.
 * Nearest-neighbour scaling maps column c to source x = (c + 0.5) / SAMPLE_W:
 * 46.5 %, 50.5 %, 54.5 % of the width — inside blue, white and red. Row 0
 * maps to y = 2.5 %, the middle of the flag's 5 %.
 */
const BAND_COLUMNS = [46, 50, 54];

/**
 * Whether three RGB(A) pixels read as blue, white and red — loose enough to
 * survive 4:2:0 chroma, a lossy encode and a downscale, strict enough that a
 * desktop or a game does not fake it by accident (a pure-blue band next to a
 * pure-white one next to a pure-red one is not a common sight).
 * @param {Uint8ClampedArray|number[]} px — 12 values: RGBA × 3, blue/white/red order
 * @returns {boolean}
 */
export function looksLikeFlag(px) {
    if (!px || px.length < 12) return false;
    const r0 = px[0],
        g0 = px[1],
        b0 = px[2];
    const r1 = px[4],
        g1 = px[5],
        b1 = px[6];
    const r2 = px[8],
        g2 = px[9],
        b2 = px[10];
    const blue = b0 >= 120 && r0 <= 110 && g0 <= 110 && b0 - r0 >= 60 && b0 - g0 >= 60;
    const white = r1 >= 170 && g1 >= 170 && b1 >= 170;
    const red = r2 >= 120 && g2 <= 110 && b2 <= 110 && r2 - g2 >= 60 && r2 - b2 >= 60;
    return blue && white && red;
}

/** Median and 90th percentile of a list of numbers (empty → nulls). */
export function summarize(values) {
    const v = values.filter((x) => typeof x === 'number' && isFinite(x)).sort((a, b) => a - b);
    if (v.length === 0) return { n: 0, median: null, p90: null, min: null, max: null };
    const at = (p) => v[Math.min(v.length - 1, Math.max(0, Math.ceil(p * v.length) - 1))];
    return { n: v.length, median: at(0.5), p90: at(0.9), min: v[0], max: v[v.length - 1] };
}

export class LatencyProbe {
    /**
     * @param {object} deps
     * @param {() => (CanvasImageSource|null)} deps.source — the element that
     *        shows the stream right now (the canvas, or the <video> on the
     *        media-track path). Read at every sample: it can switch mid-session.
     * @param {() => void} deps.sendClick — send a left button press + release
     *        to the host, exactly as a real click would go.
     * @param {(() => void)|null} [deps.showMark] paint the grey circle for
     *        one frame. Optional: a headless test has no DOM.
     * @param {((ms: number) => void)|null} [deps.requestFrameEvents] ask the
     *        video worker to report each presented frame for the next `ms`.
     *        The main-thread paths call onFramePresented themselves.
     * @param {any[]} [deps.results] the array results are appended to;
     *        exposed as window.mwLatencyResults by the caller.
     */
    constructor({ source, sendClick, showMark = null, requestFrameEvents = null, results = [] }) {
        this._source = source;
        this._sendClick = sendClick;
        this._showMark = showMark;
        this._requestFrameEvents = requestFrameEvents;
        this.results = results;
        /** @type {null | {t0: number, ts: number, tMark: number, resolve: Function, timer: any, raf: number}} */
        this._pending = null;
        this._running = false;
        this._canvas = null;
        this._ctx = null;
    }

    /** True while a click is waiting for its flag. */
    get measuring() {
        return this._pending !== null;
    }

    /**
     * Run a trial: `clicks` clicks, `spacingMs` apart, each measured. Resolves
     * with the entries of this run (they are also appended to `results`).
     * A run already in progress is left alone — its promise is returned.
     */
    async run(clicks = 3, spacingMs = 2000) {
        if (this._running) {
            console.warn('[LatencyProbe] a run is already in progress');
            return this._runPromise;
        }
        this._running = true;
        this._stopped = false;
        this._runPromise = this._runInner(clicks, spacingMs);
        try {
            return await this._runPromise;
        } finally {
            this._running = false;
            this._runPromise = null;
        }
    }

    async _runInner(clicks, spacingMs) {
        const entries = [];
        for (let i = 0; i < clicks; i++) {
            if (i > 0) await new Promise((r) => setTimeout(r, spacingMs));
            if (this._stopped) break;
            entries.push(await this.measureOnce());
        }
        const s = summarize(entries.filter((e) => e.ok).map((e) => e.latencyMs));
        const dropped = entries.length - s.n;
        console.log(
            `[LatencyProbe] ${s.n}/${entries.length} click(s) measured` +
                (s.n
                    ? ` · median ${s.median.toFixed(1)} ms · p90 ${s.p90.toFixed(1)} ms · ` +
                      `min ${s.min.toFixed(1)} · max ${s.max.toFixed(1)}`
                    : '') +
                (dropped ? ` · ${dropped} dropped` : '') +
                ' — see mwLatencyResults',
        );
        if (typeof console.table === 'function' && entries.length) console.table(entries);
        return entries;
    }

    /**
     * One click, one measurement. Resolves in at most FLAG_TIMEOUT_MS (+ the
     * wait for a stale flag to clear).
     */
    async measureOnce() {
        if (this._pending) throw new Error('LatencyProbe: a measurement is already pending');

        // The flag must be down before the click, or the first sample would
        // "detect" the previous click's flag. Give a lingering one time to go.
        const first = this._sample();
        if (first === null) return this._record(null, null, null, false, 'no picture to sample');
        if (first) {
            const cleared = await this._waitUntil(() => this._sample() === false, 300);
            if (!cleared) return this._record(null, null, null, false, 'flag already up');
        }

        return new Promise((resolve) => {
            const t0 = performance.now();
            const ts = Math.round((performance.timeOrigin + t0) * 1000);
            this._pending = { t0, ts, tMark: t0, resolve, timer: null, raf: 0 };
            this._sendClick();
            if (this._showMark) this._showMark();
            // The mark is painted in the next frame: stamp it from there so
            // fromMarkMs is what a camera on the client screen would count.
            requestAnimationFrame(() => {
                if (this._pending && this._pending.ts === ts)
                    this._pending.tMark = performance.now();
            });
            if (this._requestFrameEvents) this._requestFrameEvents(FLAG_TIMEOUT_MS + 50);
            // rAF polling is the floor: the presented-frame hooks sample sooner
            // (right after a draw, or a <video> frame callback) when they fire.
            const tick = () => {
                if (!this._pending) return;
                this._checkNow();
                if (this._pending) this._pending.raf = requestAnimationFrame(tick);
            };
            this._pending.raf = requestAnimationFrame(tick);
            this._pending.timer = setTimeout(() => this._finish(null, 'timeout'), FLAG_TIMEOUT_MS);
            this._armVideoCallback();
        });
    }

    /**
     * Called by the stream each time a frame has been presented (main-thread
     * draw resolved, worker reported a draw, <video> frame callback). One null
     * check when nothing is being measured.
     */
    onFramePresented() {
        if (!this._pending) return;
        this._checkNow();
    }

    /** Abandon any pending measurement; the probe stays usable. */
    stop() {
        this._stopped = true;
        if (this._pending) this._finish(null, 'stopped');
    }

    _checkNow() {
        const p = this._pending;
        if (!p) return;
        const now = performance.now();
        const hit = this._sample();
        if (hit) {
            this._finish(now, null);
        } else if (now - p.t0 > FLAG_TIMEOUT_MS) {
            this._finish(null, 'timeout');
        }
    }

    _finish(tHit, reason) {
        const p = this._pending;
        if (!p) return;
        this._pending = null;
        if (p.timer) clearTimeout(p.timer);
        if (p.raf) cancelAnimationFrame(p.raf);
        const ok = tHit !== null;
        p.resolve(
            this._record(p.ts, ok ? tHit - p.t0 : null, ok ? tHit - p.tMark : null, ok, reason),
        );
    }

    _record(ts, latencyMs, fromMarkMs, ok, reason) {
        const entry = {
            ts: ts ?? Math.round((performance.timeOrigin + performance.now()) * 1000),
            latencyMs: latencyMs === null ? null : Math.round(latencyMs * 1000) / 1000,
            fromMarkMs: fromMarkMs === null ? null : Math.round(fromMarkMs * 1000) / 1000,
            ok,
            reason: reason || null,
        };
        this.results.push(entry);
        return entry;
    }

    /**
     * Read the three band pixels off the output surface.
     * @returns {boolean|null} true = flag visible, false = not, null = nothing
     *          to sample (no element, zero-sized, or a readback that throws).
     */
    _sample() {
        const el = this._source();
        if (!el) return null;
        try {
            if (!this._ctx) {
                const c = document.createElement('canvas');
                c.width = SAMPLE_W;
                c.height = SAMPLE_H;
                const ctx = c.getContext('2d', { willReadFrequently: true });
                if (!ctx) return null;
                // Nearest neighbour: each sample is one source pixel at a known
                // spot, not an average that could smear a neighbouring band in.
                ctx.imageSmoothingEnabled = false;
                this._canvas = c;
                this._ctx = ctx;
            }
            const ctx = this._ctx;
            ctx.clearRect(0, 0, SAMPLE_W, SAMPLE_H);
            ctx.drawImage(el, 0, 0, SAMPLE_W, SAMPLE_H);
            const px = new Uint8ClampedArray(12);
            let any = false;
            for (let i = 0; i < 3; i++) {
                const d = ctx.getImageData(BAND_COLUMNS[i], 0, 1, 1).data;
                px.set(d, i * 4);
                if (d[3] !== 0) any = true;
            }
            // Fully transparent everywhere: the surface gave nothing back
            // (a context that cannot be read after present) — not "no flag".
            if (!any) return null;
            return looksLikeFlag(px);
        } catch (e) {
            return null;
        }
    }

    /** <video> path: its frame callback fires per presented frame. */
    _armVideoCallback() {
        const el = this._source();
        if (!el || typeof (/** @type {any} */ (el).requestVideoFrameCallback) !== 'function')
            return;
        const video = /** @type {any} */ (el);
        const cb = () => {
            if (!this._pending) return;
            this._checkNow();
            if (this._pending) video.requestVideoFrameCallback(cb);
        };
        video.requestVideoFrameCallback(cb);
    }

    _waitUntil(pred, timeoutMs) {
        return new Promise((resolve) => {
            const start = performance.now();
            const tick = () => {
                if (pred()) return resolve(true);
                if (performance.now() - start > timeoutMs) return resolve(false);
                requestAnimationFrame(tick);
            };
            tick();
        });
    }
}
