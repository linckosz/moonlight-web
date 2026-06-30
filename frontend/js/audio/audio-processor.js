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
 * AudioWorkletProcessor for MoonlightWeb audio pipeline.
 *
 * Runs on the audio render thread (real-time priority). Receives Float32
 * interleaved stereo PCM chunks (already Opus-decoded by the WebCodecs
 * AudioDecoder on the main thread) via port.onmessage and writes them to the
 * output bus in process().
 *
 * Anti-crackle design:
 *   - Adaptive jitter buffer: playback waits until `_target` frames are queued.
 *     The target starts small (~60 ms) and GROWS up to ~160 ms when the stream
 *     is unstable — on a real underrun AND proactively when the buffer dips near
 *     empty (a near-underrun) — so a laggy network self-tunes to a larger
 *     cushion; it decays slowly back toward the base during sustained stable
 *     playback. The base (steady-state) target is NOT changed, so a healthy
 *     network keeps the same latency — the cushion only grows under instability.
 *   - De-click: short linear fade-out into silence on underrun and fade-in on
 *     resume, turning sharp clicks into inaudible transitions.
 *   - Soft underrun recovery: on underrun we KEEP whatever just arrived and
 *     resume as soon as a small fraction of the target is buffered, instead of
 *     flushing everything and waiting for the full target to refill.
 *   - Time-stretch (WSOLA, optional — `config.timeStretch`): when the buffer
 *     drifts away from the target (clock drift, mild jitter), pitch-preserving
 *     accelerate/expand corrections are applied IN PLACE on the queue head via
 *     correlated overlap-add. This smoothly drains/fills the buffer instead of
 *     dropping chunks (a click) or underrunning (a gap), and adds NO steady
 *     latency: corrections only fire outside a dead-band around the target.
 *     Off → byte-identical to the plain adaptive-buffer behaviour.
 *
 * `sampleRate` is a global in AudioWorkletGlobalScope (context rate).
 *
 * @implements {AudioWorkletProcessor}
 */
class AudioProcessor extends AudioWorkletProcessor {
    constructor() {
        super();

        /** @type {Float32Array[]} Queue of Float32 interleaved stereo chunks. */
        this._queue = [];
        this._readOffset = 0; // read offset (float elements) into head chunk
        this._queuedFrames = 0; // stereo-frames currently queued
        this._consumedFrames = 0; // total stereo-frames played (diagnostics)
        this._underrunFrames = 0; // underrun frames since last diag report
        this._underrunEvents = 0; // underrun episodes since last diag report
        this._playing = false; // true once buffer filled and draining

        // Adaptive jitter-buffer targets (stereo-frames at context sample rate).
        // Base (steady-state) target is unchanged: a healthy network keeps the
        // same latency. Only the ceiling / growth / decay are tuned so a laggy
        // network builds a larger cushion and holds it longer.
        this._baseTarget = Math.round(sampleRate * 0.06); // 60 ms steady state
        this._maxTarget = Math.round(sampleRate * 0.16); // 160 ms ceiling
        this._target = this._baseTarget;
        this.GROW_FRAMES = Math.round(sampleRate * 0.02); // +20 ms per instability event
        this.MAX_BUFFER_FRAMES = Math.round(sampleRate * 0.4); // 400 ms hard cap
        this.DECAY_INTERVAL = Math.round(sampleRate * 8); // shrink after 8 s stable
        this.DECAY_FRAMES = Math.round(sampleRate * 0.01); // -10 ms per decay step

        // Soft underrun recovery: resume playback once this fraction of the
        // current target is re-buffered, instead of waiting for the full target.
        this._resumeFraction = 0.5;
        this._underrunRecovery = false;

        // De-click fade length (samples). ~1.3 ms at 48 kHz.
        this.FADE_SAMPLES = 64;
        this._fadeInRemaining = 0;
        this._lastL = 0;
        this._lastR = 0;
        this._framesSinceUnderrun = 0;

        // ── Time-stretch (WSOLA accelerate/expand) ──────────────────────────
        // Off by default; the main thread enables it via a 'config' message.
        this._timeStretch = false;
        this.TS_PMIN = Math.round(sampleRate / 400); // shortest pitch period (400 Hz)
        this.TS_PMAX = Math.round(sampleRate / 100); // longest pitch period (100 Hz)
        this.TS_DEADBAND = Math.round(sampleRate * 0.02); // ±20 ms around target: no correction
        this.TS_MIN_SPACING = Math.round(sampleRate * 0.03); // ≥30 ms between corrections
        this.TS_MIN_CORR = 0.3; // skip if best correlation is too weak (avoids artefacts)
        this._tsSinceCorrection = this.TS_MIN_SPACING;
        this._tsLastScore = 0;

        // PCM input port: when audio is decoded in a dedicated Worker, PCM is
        // posted straight here over a transferred MessagePort (worker → worklet),
        // bypassing the main thread entirely. In the no-worker fallback, PCM
        // arrives on this.port instead.
        this._pcmPort = null;

        this.port.onmessage = (evt) => {
            const data = evt.data;
            // Config messages from the main thread (plain objects, not buffers).
            if (data && data.type === 'config') {
                if (typeof data.baseLatencyMs === 'number') {
                    this._baseTarget = Math.round((sampleRate * data.baseLatencyMs) / 1000);
                    this._target = Math.max(this._target, this._baseTarget);
                }
                if (typeof data.timeStretch === 'boolean') {
                    this._timeStretch = data.timeStretch;
                }
                return;
            }
            // Direct PCM channel from the decode worker.
            if (data && data.type === 'pcm-port') {
                this._pcmPort = data.port;
                this._pcmPort.onmessage = (e) => this._enqueue(e.data);
                return;
            }
            // Fallback (no worker): PCM (transferred ArrayBuffer) on the node port.
            this._enqueue(data);
        };
    }

    /** Queue one interleaved-stereo Float32 PCM buffer for playback. */
    _enqueue(buffer) {
        const chunk = new Float32Array(buffer);
        if (chunk.length === 0) return;

        this._queue.push(chunk);
        this._queuedFrames += chunk.length >> 1; // /2 channels

        // Overrun protection: drop oldest chunks until under the cap. With
        // time-stretch on this is only a last-resort safety net (accelerate
        // normally drains the buffer smoothly well before this triggers).
        while (this._queuedFrames > this.MAX_BUFFER_FRAMES && this._queue.length > 1) {
            const dropped = this._queue.shift();
            this._queuedFrames -= (dropped.length - this._readOffset) >> 1;
            this._readOffset = 0;
        }
    }

    // ── Queue helpers (used by the time-stretch corrections) ────────────────

    /**
     * Pop exactly `frames` stereo-frames from the FRONT of the queue into a
     * fresh interleaved buffer, spanning chunk boundaries. Caller must ensure
     * `this._queuedFrames >= frames`.
     * @param {number} frames
     * @returns {Float32Array} interleaved stereo, length frames*2.
     */
    _popFrames(frames) {
        const out = new Float32Array(frames << 1);
        let got = 0;
        while (got < frames && this._queue.length > 0) {
            const chunk = this._queue[0];
            const avail = (chunk.length - this._readOffset) >> 1;
            const take = Math.min(avail, frames - got);
            out.set(chunk.subarray(this._readOffset, this._readOffset + (take << 1)), got << 1);
            got += take;
            this._readOffset += take << 1;
            this._queuedFrames -= take;
            if (this._readOffset >= chunk.length) {
                this._queue.shift();
                this._readOffset = 0;
            }
        }
        return out;
    }

    /**
     * Reset the read offset by replacing the head chunk with its unread tail,
     * so the head always starts at offset 0. Needed before unshifting.
     */
    _normalizeHead() {
        if (this._readOffset > 0 && this._queue.length > 0) {
            this._queue[0] = this._queue[0].subarray(this._readOffset);
            this._readOffset = 0;
        }
    }

    /** Push an interleaved buffer back to the FRONT of the queue. */
    _unshiftFrames(buf) {
        this._normalizeHead();
        this._queue.unshift(buf);
        this._queuedFrames += buf.length >> 1;
    }

    /**
     * Estimate the pitch period (stereo-frames) of `seg` by maximising the
     * normalised cross-correlation of the L channel between [0,P) and [P,2P).
     * Stores the best score in `_tsLastScore`. Caller guarantees seg holds at
     * least 2*TS_PMAX frames.
     * @param {Float32Array} seg interleaved stereo
     * @param {number} frames number of stereo-frames in seg
     * @returns {number} best period P
     */
    _findPeriod(seg, frames) {
        const maxP = Math.min(this.TS_PMAX, frames >> 1);
        let bestP = this.TS_PMIN;
        let bestScore = -Infinity;
        for (let P = this.TS_PMIN; P <= maxP; P++) {
            let cc = 0;
            let e0 = 0;
            let e1 = 0;
            // Subsample by 2 (L channel only) to bound CPU on the audio thread.
            for (let i = 0; i < P; i += 2) {
                const a = seg[i << 1];
                const b = seg[(i + P) << 1];
                cc += a * b;
                e0 += a * a;
                e1 += b * b;
            }
            const score = cc / (Math.sqrt(e0 * e1) + 1e-9);
            if (score > bestScore) {
                bestScore = score;
                bestP = P;
            }
        }
        this._tsLastScore = bestScore;
        return bestP;
    }

    /**
     * Accelerate: remove ~one pitch period from the queue head via overlap-add,
     * draining the buffer by P frames without a click or pitch change.
     * @returns {boolean} true if a correction was applied.
     */
    _accelerate() {
        const need = this.TS_PMAX << 1; // 2*PMAX frames
        const seg = this._popFrames(need);
        const P = this._findPeriod(seg, need);
        if (this._tsLastScore < this.TS_MIN_CORR) {
            this._unshiftFrames(seg); // too noisy to merge cleanly — put it back
            return false;
        }
        // out = crossfade(seg[0,P), seg[P,2P)) ++ seg[2P, need)  → length need-P
        const out = new Float32Array((need - P) << 1);
        for (let i = 0; i < P; i++) {
            const w = i / P;
            const oi = i << 1;
            const ai = i << 1;
            const bi = (P + i) << 1;
            out[oi] = seg[ai] * (1 - w) + seg[bi] * w;
            out[oi + 1] = seg[ai + 1] * (1 - w) + seg[bi + 1] * w;
        }
        out.set(seg.subarray((P << 1) << 1), P << 1); // tail seg[2P..need)
        this._unshiftFrames(out);
        return true;
    }

    /**
     * Expand: insert ~one pitch period into the queue head via overlap-add,
     * filling the buffer by P frames to ride out a dip without an underrun.
     * @returns {boolean} true if a correction was applied.
     */
    _expand() {
        const need = this.TS_PMAX << 1; // 2*PMAX frames
        const seg = this._popFrames(need);
        const P = this._findPeriod(seg, need);
        if (this._tsLastScore < this.TS_MIN_CORR) {
            this._unshiftFrames(seg);
            return false;
        }
        // out = seg[0,P) ++ crossfade(seg[P,2P), seg[0,P)) ++ seg[P,need)
        //       → length need+P
        const out = new Float32Array((need + P) << 1);
        out.set(seg.subarray(0, P << 1), 0);
        for (let i = 0; i < P; i++) {
            const w = i / P;
            const oi = (P + i) << 1;
            const ai = (P + i) << 1; // seg[P+i]
            const bi = i << 1; // seg[i]
            out[oi] = seg[ai] * (1 - w) + seg[bi] * w;
            out[oi + 1] = seg[ai + 1] * (1 - w) + seg[bi + 1] * w;
        }
        out.set(seg.subarray(P << 1, need << 1), (P << 1) << 1); // seg[P..need)
        this._unshiftFrames(out);
        return true;
    }

    /**
     * Nudge the buffer toward the target with one pitch-preserving correction
     * per call, only when it has drifted beyond the dead-band. No-op near the
     * target, so a healthy stream is untouched (zero added latency).
     */
    _maintainTimeStretch(numFrames) {
        this._tsSinceCorrection += numFrames;
        if (
            this._tsSinceCorrection < this.TS_MIN_SPACING ||
            this._queuedFrames < this.TS_PMAX << 1
        ) {
            return;
        }
        const err = this._queuedFrames - this._target;
        if (err > this.TS_DEADBAND) {
            if (this._accelerate()) this._tsSinceCorrection = 0;
        } else if (err < -this.TS_DEADBAND) {
            if (this._expand()) this._tsSinceCorrection = 0;
        }
    }

    process(inputs, outputs) {
        const out = outputs[0];
        if (!out || out.length < 2) return true;

        const left = out[0];
        const right = out[1];
        const numFrames = left.length; // usually 128

        // Gate playback until the buffer reaches the (adaptive) target.
        // After an underrun we only wait for a fraction of the target to refill
        // (soft recovery), so the silent gap is a few ms instead of the full
        // re-buffer. The first start still waits for the full target.
        if (!this._playing) {
            const gate = this._underrunRecovery
                ? Math.max(1, Math.round(this._target * this._resumeFraction))
                : this._target;
            if (this._queuedFrames >= gate) {
                this._playing = true;
                this._underrunRecovery = false;
                this._fadeInRemaining = this.FADE_SAMPLES; // fade in on resume
                this.port.postMessage({
                    type: 'started',
                    outChannels: out.length,
                    queuedFrames: this._queuedFrames,
                    targetMs: Math.round((this._target / sampleRate) * 1000),
                });
            } else {
                left.fill(0);
                right.fill(0);
                return true;
            }
        }

        // Pitch-preserving buffer maintenance (drift/jitter) before draining.
        if (this._timeStretch) {
            this._maintainTimeStretch(numFrames);
        }

        let outIdx = 0;

        // Drain queued chunks, applying fade-in gain on the first samples.
        while (outIdx < numFrames && this._queue.length > 0) {
            const chunk = this._queue[0];
            const availablePairs = (chunk.length - this._readOffset) >> 1;
            const needed = numFrames - outIdx;
            const toCopy = Math.min(availablePairs, needed);
            const start = this._readOffset;

            for (let i = 0; i < toCopy; i++) {
                const si = start + (i << 1);
                let l = chunk[si];
                let r = chunk[si + 1];
                if (this._fadeInRemaining > 0) {
                    const g = 1 - this._fadeInRemaining / this.FADE_SAMPLES;
                    l *= g;
                    r *= g;
                    this._fadeInRemaining--;
                }
                left[outIdx + i] = l;
                right[outIdx + i] = r;
            }

            outIdx += toCopy;
            this._readOffset += toCopy << 1;
            this._consumedFrames += toCopy;
            this._queuedFrames -= toCopy;

            if (this._readOffset >= chunk.length) {
                this._queue.shift();
                this._readOffset = 0;
            }
        }

        // Remember the last real sample for the de-click fade-out.
        if (outIdx > 0) {
            this._lastL = left[outIdx - 1];
            this._lastR = right[outIdx - 1];
        }

        if (outIdx < numFrames) {
            // ── Underrun: buffer drained mid-quantum ───────────────────────────
            this._underrunFrames += numFrames - outIdx;
            this._underrunEvents++;

            // De-click: linearly ramp the last sample down to zero, then silence.
            const gap = numFrames - outIdx;
            const fade = Math.min(this.FADE_SAMPLES, gap);
            for (let i = 0; i < gap; i++) {
                const g = i < fade ? 1 - i / fade : 0;
                left[outIdx + i] = this._lastL * g;
                right[outIdx + i] = this._lastR * g;
            }

            // Adapt: grow the target so the rebuilt buffer rides out the next
            // lag, and re-arm buffering in soft-recovery mode. The queue is
            // already drained here (we ran out mid-quantum); whatever arrives
            // next is kept and we resume at a fraction of the target rather than
            // flushing and waiting for a full refill.
            this._target = Math.min(this._maxTarget, this._target + this.GROW_FRAMES);
            this._playing = false;
            this._underrunRecovery = true;
            this._lastL = 0;
            this._lastR = 0;
            this._framesSinceUnderrun = 0;
        } else {
            // Stable playback.
            this._framesSinceUnderrun += numFrames;
            // Instability detection: if the buffer dipped near empty without a
            // full underrun (jitter), grow the target proactively before it
            // becomes an audible gap. Fast up / slow down (AIMD), 160 ms ceiling.
            const lowWatermark = Math.max(numFrames * 2, this._target >> 2); // ~25% of target
            if (this._queuedFrames < lowWatermark && this._target < this._maxTarget) {
                this._target = Math.min(this._maxTarget, this._target + this.GROW_FRAMES);
                this._framesSinceUnderrun = 0;
            } else if (
                this._framesSinceUnderrun > this.DECAY_INTERVAL &&
                this._target > this._baseTarget
            ) {
                // Slowly decay the target back toward the base when sustained-stable.
                this._target = Math.max(this._baseTarget, this._target - this.DECAY_FRAMES);
                this._framesSinceUnderrun = 0;
            }
        }

        // Diagnostics ~once per second.
        if (this._consumedFrames % 48000 < numFrames) {
            this.port.postMessage({
                type: 'diag',
                queueDepth: this._queue.length,
                queuedFrames: this._queuedFrames,
                targetMs: Math.round((this._target / sampleRate) * 1000),
                underrunFrames: this._underrunFrames,
                underrunEvents: this._underrunEvents,
                consumedFrames: this._consumedFrames,
            });
            this._underrunFrames = 0;
            this._underrunEvents = 0;
        }

        return true;
    }
}

registerProcessor('audio-processor', AudioProcessor);
