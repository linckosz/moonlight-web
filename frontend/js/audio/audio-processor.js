/**
 * AudioWorkletProcessor for Moonlight-Web audio pipeline.
 *
 * Runs on the audio render thread (real-time priority).  Receives raw PCM16
 * stereo interleaved buffers from the main thread via port.onmessage, converts
 * them to Float32, and writes them to the output bus in process().
 *
 * Underrun: fills with silence (zeros).
 * Overrun: drops oldest chunks if queue exceeds MAX_QUEUED_CHUNKS.
 *
 * @implements {AudioWorkletProcessor}
 */
class AudioProcessor extends AudioWorkletProcessor {
    constructor() {
        super();

        /** @type {Int16Array[]} Queue of PCM16 stereo interleaved chunks. */
        this._queue = [];

        /** @type {number} Read offset (in int16 elements) into the current chunk. */
        this._readOffset = 0;

        /** @type {number} Total stereo-frames consumed (for diagnostics). */
        this._consumedFrames = 0;

        /** @type {number} Consecutive underrun frames in current process() call. */
        this._underrunFrames = 0;

        /** Max chunks to buffer before dropping oldest (approx 6 frames of 10ms). */
        this.MAX_QUEUED_CHUNKS = 12;

        this.port.onmessage = (evt) => {
            // evt.data is a transferred ArrayBuffer containing raw PCM16 data
            const pcm16 = new Int16Array(evt.data);

            // Drop if queue is too deep (overrun protection)
            if (this._queue.length >= this.MAX_QUEUED_CHUNKS) {
                // Drop half the queue to recover quickly
                const dropCount = Math.floor(this.MAX_QUEUED_CHUNKS / 2);
                this._queue.splice(0, dropCount);
            }

            this._queue.push(pcm16);
        };
    }

    /**
     * AudioWorklet process callback (called by the audio thread).
     *
     * @param {Float32Array[][]} inputs  - Not used (no input connections).
     * @param {Float32Array[][]} outputs - Stereo output: outputs[0][0] = left, outputs[0][1] = right.
     * @param {object}           params  - Not used.
     * @returns {boolean} true to keep the processor alive.
     */
    process(inputs, outputs) {
        const out = outputs[0];
        if (!out || out.length < 2) return true;

        const left = out[0];
        const right = out[1];
        const numFrames = left.length; // Typically 128 frames per render quantum

        let outIdx = 0;

        // Drain queued chunks
        while (outIdx < numFrames && this._queue.length > 0) {
            const chunk = this._queue[0];
            const availablePairs = (chunk.length - this._readOffset) >> 1; // /2
            const needed = numFrames - outIdx;
            const toCopy = Math.min(availablePairs, needed);

            const start = this._readOffset;

            // Bulk de-interleave + PCM16-to-Float32 conversion
            for (let i = 0; i < toCopy; i++) {
                const si = start + (i << 1); // i * 2
                left[outIdx + i] = chunk[si] * 0.000030517578125;     // / 32768
                right[outIdx + i] = chunk[si + 1] * 0.000030517578125;
            }

            outIdx += toCopy;
            this._readOffset += toCopy << 1; // toCopy * 2
            this._consumedFrames += toCopy;

            // Advance to next chunk if current is exhausted
            if (this._readOffset >= chunk.length) {
                this._queue.shift();
                this._readOffset = 0;
            }
        }

        // Fill remaining frames with silence (underrun)
        if (outIdx < numFrames) {
            this._underrunFrames += numFrames - outIdx;
            while (outIdx < numFrames) {
                left[outIdx] = 0.0;
                right[outIdx] = 0.0;
                outIdx++;
            }
        }

        // Report diagnostics periodically via message
        if (this._consumedFrames % 48000 === 0) {
            // Every ~1 second at 48kHz, report buffer health
            this.port.postMessage({
                type: 'diag',
                queueDepth: this._queue.length,
                underrunFrames: this._underrunFrames,
                consumedFrames: this._consumedFrames
            });
            this._underrunFrames = 0;
        }

        return true; // Keep alive
    }
}

registerProcessor('audio-processor', AudioProcessor);
