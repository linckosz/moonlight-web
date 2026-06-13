/**
 * AudioWorkletProcessor for Moonlight-Web audio pipeline.
 *
 * Runs on the audio render thread (real-time priority). Receives Float32
 * interleaved stereo PCM chunks (already Opus-decoded by the WebCodecs
 * AudioDecoder on the main thread) via port.onmessage and writes them to the
 * output bus in process().
 *
 * Anti-crackle design:
 *   - Adaptive jitter buffer: playback waits until `_target` frames are queued.
 *     The target starts small (~60 ms) and GROWS on every underrun (up to
 *     ~240 ms), so a laggy network self-tunes to a larger cushion; it decays
 *     back toward the base during sustained stable playback.
 *   - De-click: short linear fade-out into silence on underrun and fade-in on
 *     resume, turning sharp clicks into inaudible transitions.
 *   - Overrun: drop the OLDEST chunk(s) to keep latency bounded.
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
        this._readOffset = 0;        // read offset (float elements) into head chunk
        this._queuedFrames = 0;      // stereo-frames currently queued
        this._consumedFrames = 0;    // total stereo-frames played (diagnostics)
        this._underrunFrames = 0;    // underrun frames since last diag report
        this._underrunEvents = 0;    // underrun episodes since last diag report
        this._playing = false;       // true once buffer filled and draining

        // Adaptive jitter-buffer targets (stereo-frames at context sample rate).
        this._baseTarget = Math.round(sampleRate * 0.06);   // 60 ms steady state
        this._maxTarget = Math.round(sampleRate * 0.24);    // 240 ms ceiling
        this._target = this._baseTarget;
        this.GROW_FRAMES = Math.round(sampleRate * 0.03);   // +30 ms per underrun
        this.MAX_BUFFER_FRAMES = Math.round(sampleRate * 0.50); // 500 ms hard cap
        this.DECAY_INTERVAL = Math.round(sampleRate * 5);   // shrink after 5 s stable

        // De-click fade length (samples). ~1.3 ms at 48 kHz.
        this.FADE_SAMPLES = 64;
        this._fadeInRemaining = 0;
        this._lastL = 0;
        this._lastR = 0;
        this._framesSinceUnderrun = 0;

        this.port.onmessage = (evt) => {
            // Allow the main thread to override the base latency target.
            if (evt.data && evt.data.type === 'config') {
                if (typeof evt.data.baseLatencyMs === 'number') {
                    this._baseTarget = Math.round(sampleRate * evt.data.baseLatencyMs / 1000);
                    this._target = Math.max(this._target, this._baseTarget);
                }
                return;
            }

            // Float32 interleaved stereo PCM (transferred ArrayBuffer).
            const chunk = new Float32Array(evt.data);
            if (chunk.length === 0) return;

            this._queue.push(chunk);
            this._queuedFrames += chunk.length >> 1; // /2 channels

            // Overrun protection: drop oldest chunks until under the cap.
            while (this._queuedFrames > this.MAX_BUFFER_FRAMES && this._queue.length > 1) {
                const dropped = this._queue.shift();
                this._queuedFrames -= (dropped.length - this._readOffset) >> 1;
                this._readOffset = 0;
            }
        };
    }

    process(inputs, outputs) {
        const out = outputs[0];
        if (!out || out.length < 2) return true;

        const left = out[0];
        const right = out[1];
        const numFrames = left.length; // usually 128

        // Gate playback until the buffer reaches the (adaptive) target.
        if (!this._playing) {
            if (this._queuedFrames >= this._target) {
                this._playing = true;
                this._fadeInRemaining = this.FADE_SAMPLES; // fade in on resume
                this.port.postMessage({
                    type: 'started',
                    outChannels: out.length,
                    queuedFrames: this._queuedFrames,
                    targetMs: Math.round(this._target / sampleRate * 1000)
                });
            } else {
                left.fill(0);
                right.fill(0);
                return true;
            }
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
                    const g = 1 - (this._fadeInRemaining / this.FADE_SAMPLES);
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
                const g = i < fade ? (1 - i / fade) : 0;
                left[outIdx + i] = this._lastL * g;
                right[outIdx + i] = this._lastR * g;
            }

            // Adapt: grow the target so the rebuilt buffer rides out the next lag,
            // and re-arm buffering (queue is already empty here).
            this._target = Math.min(this._maxTarget, this._target + this.GROW_FRAMES);
            this._playing = false;
            this._readOffset = 0;
            this._queue.length = 0;
            this._queuedFrames = 0;
            this._lastL = 0;
            this._lastR = 0;
            this._framesSinceUnderrun = 0;
        } else {
            // Stable playback — slowly decay the target back toward the base.
            this._framesSinceUnderrun += numFrames;
            if (this._framesSinceUnderrun > this.DECAY_INTERVAL &&
                this._target > this._baseTarget) {
                this._target = Math.max(this._baseTarget, this._target - this.GROW_FRAMES);
                this._framesSinceUnderrun = 0;
            }
        }

        // Diagnostics ~once per second.
        if (this._consumedFrames % 48000 < numFrames) {
            this.port.postMessage({
                type: 'diag',
                queueDepth: this._queue.length,
                queuedFrames: this._queuedFrames,
                targetMs: Math.round(this._target / sampleRate * 1000),
                underrunFrames: this._underrunFrames,
                underrunEvents: this._underrunEvents,
                consumedFrames: this._consumedFrames
            });
            this._underrunFrames = 0;
            this._underrunEvents = 0;
        }

        return true;
    }
}

registerProcessor('audio-processor', AudioProcessor);
