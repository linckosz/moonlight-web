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
 * PipelineDiag — observations of the browser-side WebCodecs pipeline.
 *
 * The overlay's three latency legs (decode / render queue / render) say how
 * long each stage took, but not WHY. They all inflate together as soon as the
 * host framerate rises (static desktop → moving mouse or a video playing),
 * which has two very different explanations that the legs alone cannot tell
 * apart:
 *
 *   - the browser is slower per frame (bigger frames to decode, heavier draw), or
 *   - the pipeline is BACK-PRESSURED: the presentation path (canvas swap chain /
 *     GPU) can't retire frames at the incoming rate, the render guard stays busy,
 *     decoded frames pile up, the decoder's output pool starves and its
 *     submit→output latency grows as a consequence.
 *
 * Under back-pressure the decode leg is a symptom, not a cause — and the fps
 * shown is the DECODED rate, so a pipeline presenting only two thirds of what
 * it decodes still reads "60 fps".
 *
 * This class records the quantities that separate the two:
 *   - frame arrival interval (what the host actually sends),
 *   - VideoDecoder.decodeQueueSize at submit (decoder input pressure),
 *   - decoded-frame queue depth at pump (render can't keep up),
 *   - the draw split into submit vs wait (see VideoRenderer.lastDraw): a high
 *     WAIT with a low submit is presentation back-pressure, not draw cost,
 *   - drops by cause, so the presented-vs-decoded gap is attributable.
 *
 * Instantiated once per pipeline: the worker owns one (its own clock, posted in
 * the counters message) and StreamView owns one for the main-thread path. Pure
 * bookkeeping — no DOM, no WebCodecs — so both contexts can import it.
 */

/** Sliding-window samples, mirroring StreamView's SlidingStats (avg + max). */
class DiagWindow {
    constructor(windowMs) {
        this._windowMs = windowMs;
        this._samples = []; // [{ time, value }]
    }

    /**
     * @param {number} value
     * @param {number} [max] Reject above this — the default is the same sanity
     *   bound as the latency legs (a clock jump or a stream-long stall must not
     *   poison a millisecond window forever). Pass Infinity for a magnitude
     *   that legitimately has no bound, like a frame size.
     */
    push(value, max = 5000) {
        if (!(value >= 0) || value > max) return;
        this._samples.push({ time: performance.now(), value });
    }

    _prune() {
        const cutoff = performance.now() - this._windowMs;
        let i = 0;
        while (i < this._samples.length && this._samples[i].time <= cutoff) i++;
        if (i > 0) this._samples.splice(0, i);
    }

    get avg() {
        this._prune();
        if (this._samples.length === 0) return 0;
        let sum = 0;
        for (const s of this._samples) sum += s.value;
        return sum / this._samples.length;
    }

    get max() {
        this._prune();
        let m = 0;
        for (const s of this._samples) {
            if (s.value > m) m = s.value;
        }
        return m;
    }
}

export class PipelineDiag {
    constructor(windowMs = 2000) {
        this._arrival = new DiagWindow(windowMs);
        this._frameBytes = new DiagWindow(windowMs);
        this._decodeQueue = new DiagWindow(windowMs);
        this._frameQueue = new DiagWindow(windowMs);
        this._renderSubmit = new DiagWindow(windowMs);
        this._renderWait = new DiagWindow(windowMs);
        this._drawConcurrency = new DiagWindow(windowMs);
        this._lastArrival = 0;
        this._renderPath = '';
        // Cumulative, by cause — deltas are computed by the reader so the
        // display can show a per-second rate without resetting shared state.
        this._drops = { stale: 0, queueFull: 0, backpressure: 0 };
    }

    /**
     * One encoded frame entered the pipeline: records the arrival interval and,
     * when the size is known, how big the frame is. A still desktop sends tiny
     * deltas while motion sends full-size frames, so the encoded size is the
     * competing explanation for a growing decode leg ("the frames are simply
     * heavier") against back-pressure — the queue depths tell them apart.
     * @param {number} [bytes]
     */
    noteArrival(bytes) {
        const now = performance.now();
        if (this._lastArrival > 0) this._arrival.push(now - this._lastArrival);
        this._lastArrival = now;
        if (bytes !== undefined) this._frameBytes.push(bytes, Infinity);
    }

    /** VideoDecoder.decodeQueueSize, sampled at decode() submit. */
    noteDecodeQueue(size) {
        this._decodeQueue.push(size);
    }

    /** Decoded frames waiting to be drawn, sampled when the pump runs. */
    noteFrameQueue(depth) {
        this._frameQueue.push(depth);
    }

    /**
     * Timing of the draw that just completed, as reported by the renderer.
     * @param {{submitMs?: number, waitMs?: number, path?: string}|null} lastDraw
     * @param {number} [inFlight] Draws running concurrently with this one — a
     *   draw's duration is a LATENCY, and once the pipeline overlaps two of
     *   them the time each frame actually costs the pipeline is that latency
     *   divided by the concurrency (Little's law). Without this, doubling the
     *   in-flight cap would look like the draw got twice as expensive.
     */
    noteDraw(lastDraw, inFlight) {
        if (!lastDraw) return;
        this._renderSubmit.push(lastDraw.submitMs);
        this._renderWait.push(lastDraw.waitMs);
        this._drawConcurrency.push(inFlight === undefined ? 1 : inFlight);
        if (lastDraw.path) this._renderPath = lastDraw.path;
    }

    /**
     * A frame was dropped.
     * @param {'stale'|'queueFull'|'backpressure'} cause
     *   stale        — superseded in the render queue (drop-to-latest)
     *   queueFull    — decoder output arrived with the queue already full
     *   backpressure — never submitted: decoder input queue saturated / no reference
     */
    noteDrop(cause) {
        if (this._drops[cause] !== undefined) this._drops[cause]++;
    }

    /** Flat snapshot — postMessage-safe (structured clone of plain numbers). */
    snapshot() {
        // Per-frame cost of the render stage: the whole draw latency spread
        // over the draws that were overlapping. This — not the raw wait — is
        // what has to fit inside a frame interval, so it is what the enhancer
        // ladder is fed.
        const concurrency = Math.max(1, this._drawConcurrency.avg);
        const drawLatency = this._renderSubmit.avg + this._renderWait.avg;
        return {
            drawConcurrency: concurrency,
            renderServiceMs: drawLatency / concurrency,
            arrivalAvgMs: this._arrival.avg,
            arrivalMaxMs: this._arrival.max,
            frameBytesAvg: this._frameBytes.avg,
            frameBytesMax: this._frameBytes.max,
            decodeQueueAvg: this._decodeQueue.avg,
            decodeQueueMax: this._decodeQueue.max,
            frameQueueAvg: this._frameQueue.avg,
            frameQueueMax: this._frameQueue.max,
            renderSubmitMs: this._renderSubmit.avg,
            renderWaitMs: this._renderWait.avg,
            renderPath: this._renderPath,
            dropStale: this._drops.stale,
            dropQueueFull: this._drops.queueFull,
            dropBackpressure: this._drops.backpressure,
        };
    }
}

/**
 * Render one snapshot as a single compact line, for the overlay's expanded
 * detail and the periodic console trace. `rates` carries the values the
 * snapshot cannot know (they live on the main thread): decoded/presented fps
 * and the drop deltas of the last interval.
 *
 * @param {ReturnType<PipelineDiag['snapshot']>|null} diag
 * @param {{decodedFps?: number, presentedFps?: number, dropsPerSec?: number}} rates
 */
export function formatDiag(diag, rates) {
    if (!diag) return '';
    const r = rates || {};
    const n1 = (v) => (v || 0).toFixed(1);
    return (
        'fps in/out ' +
        Math.round(r.decodedFps || 0) +
        '/' +
        Math.round(r.presentedFps || 0) +
        ' · arrival ' +
        n1(diag.arrivalAvgMs) +
        '/' +
        n1(diag.arrivalMaxMs) +
        'ms · frame ' +
        n1(diag.frameBytesAvg / 1024) +
        '/' +
        n1(diag.frameBytesMax / 1024) +
        'KB · decQ ' +
        n1(diag.decodeQueueAvg) +
        '/' +
        Math.round(diag.decodeQueueMax) +
        ' · frmQ ' +
        n1(diag.frameQueueAvg) +
        '/' +
        Math.round(diag.frameQueueMax) +
        ' · draw ' +
        n1(diag.renderSubmitMs) +
        '+' +
        n1(diag.renderWaitMs) +
        'ms ×' +
        n1(diag.drawConcurrency) +
        '→' +
        n1(diag.renderServiceMs) +
        'ms' +
        (diag.renderPath ? ' [' + diag.renderPath + ']' : '') +
        ' · drop ' +
        n1(r.dropsPerSec) +
        '/s (stale ' +
        diag.dropStale +
        ' full ' +
        diag.dropQueueFull +
        ' bp ' +
        diag.dropBackpressure +
        ')'
    );
}
