/**
 * AudioPipeline — manages AudioContext + AudioWorkletNode for streaming audio.
 *
 * Receives raw PCM16 stereo interleaved buffers via write(), transfers them
 * to the AudioWorklet thread for Float32 conversion and real-time playback.
 * Handles lifecycle: init, resume, close.
 *
 * Requirements:
 *   - Browser must support AudioWorklet (Chrome 66+, Firefox 76+, Safari 14.1+).
 *   - AudioContext created at 48000 Hz (native rate may differ; no resampling yet).
 *
 * Usage:
 *   const audio = new AudioPipeline();
 *   await audio.init();               // Load AudioWorklet, create node
 *   audio.write(pcm16Buffer);          // Feed PCM16 data
 *   await audio.resume();             // If context suspended
 *   audio.close();                     // Cleanup
 */
export class AudioPipeline {
    /**
     * @param {object} [options]
     * @param {number} [options.sampleRate=48000] - Target sample rate.
     * @param {string} [options.workletUrl='/js/audio/audio-processor.js'] - URL to the AudioWorklet processor module.
     */
    constructor(options = {}) {
        this.sampleRate = options.sampleRate || 48000;
        this.workletUrl = options.workletUrl || '/js/audio/audio-processor.js';

        /** @type {AudioContext|null} */
        this.context = null;

        /** @type {AudioWorkletNode|null} */
        this.node = null;

        /** @type {boolean} True after init() succeeds and node is ready. */
        this.ready = false;

        /** @type {boolean} True when close() has been called. */
        this._closed = false;

        // Diagnostics
        this._writtenSamples = 0;   // Total stereo-frames written
        this._queueDepth = 0;       // Estimated queue depth inside the worklet
        this._underrunCount = 0;    // Total underrun frames (from worklet diag)

        // Bound handler for worklet messages
        this._onWorkletMessage = (evt) => this._handleWorkletMessage(evt);
    }

    /**
     * Initialise the AudioContext and AudioWorkletNode.
     *
     * Must be called after a user gesture (click/key), because AudioContext
     * creation requires a user activation in most browsers.
     *
     * @returns {Promise<boolean>} true if initialised successfully, false on error.
     */
    async init() {
        if (this._closed) return false;

        try {
            // Create AudioContext at the target sample rate
            this.context = new AudioContext({ sampleRate: this.sampleRate });

            // Check the actual sample rate (might differ)
            if (this.context.sampleRate !== this.sampleRate) {
                console.warn('[AudioPipeline] Sample rate mismatch: requested=' +
                    this.sampleRate + ', actual=' + this.context.sampleRate);

                // TODO: implement resampling later (WSOLA or offline converter)
                // For now the pitch may be slightly off if rates mismatch.
            }

            // Load the AudioWorklet processor module
            await this.context.audioWorklet.addModule(this.workletUrl);

            // Guard: if close() was called during addModule(), abort
            if (this._closed) {
                this.cleanup();
                return false;
            }

            // Create the AudioWorkletNode
            this.node = new AudioWorkletNode(this.context, 'audio-processor');
            this.node.port.onmessage = this._onWorkletMessage;
            this.node.connect(this.context.destination);

            // Guard: if close() was called during node setup, abort
            if (this._closed) {
                this.cleanup();
                return false;
            }

            // Handle context state changes
            this.context.onstatechange = () => {
                console.log('[AudioPipeline] AudioContext state: ' + this.context.state);
                if (this.context.state === 'closed' && !this._closed) {
                    // External close (e.g. browser autoplay policy)
                    this._closed = true;
                }
            };

            this.ready = true;
            console.log('[AudioPipeline] Initialised: rate=' + this.context.sampleRate +
                ', baseLatency=' + (this.context.baseLatency || '?') + 's');
            return true;

        } catch (err) {
            console.error('[AudioPipeline] Initialisation failed:', err.message, err);
            this.cleanup();
            return false;
        }
    }

    /**
     * Write a PCM16 stereo interleaved buffer to the audio pipeline.
     *
     * Extracts the PCM16 payload into a standalone ArrayBuffer and transfers it
     * to the AudioWorklet thread (zero-copy of the extracted portion).
     *
     * NOTE: `sample` is typically a sub-view of a larger DataChannel message
     * buffer (which includes a 13-byte fragment header).  We MUST extract only
     * the PCM16 bytes before transfer to avoid sending header bytes as audio.
     *
     * @param {Uint8Array} sample - PCM16 stereo interleaved data (from WebRTC DataChannel).
     */
    write(sample) {
        if (!this.ready || this._closed) return;

        try {
            // Extract PCM16 bytes into a standalone buffer.
            // sample.slice() copies only the view's bytes, not the parent buffer.
            const pcmBuf = sample.slice();
            this.node.port.postMessage(pcmBuf.buffer, [pcmBuf.buffer]);
            this._writtenSamples += sample.byteLength >> 2; // /4 (2 bytes * 2 channels)
        } catch (err) {
            // If transfer fails (e.g. context closed during write), log once
            if (!this._transferErrorLogged) {
                console.warn('[AudioPipeline] Transfer failed:', err.message);
                this._transferErrorLogged = true;
            }
        }
    }

    /**
     * Resume the AudioContext if suspended (autoplay policy).
     *
     * Must be called from a user gesture context.
     *
     * @returns {Promise<boolean>} true if context is now running.
     */
    async resume() {
        if (!this.context || this._closed) return false;

        if (this.context.state === 'suspended') {
            try {
                await this.context.resume();
                console.log('[AudioPipeline] AudioContext resumed');
            } catch (err) {
                console.warn('[AudioPipeline] Resume failed:', err.message);
                return false;
            }
        }

        return this.context.state === 'running';
    }

    /**
     * Close the AudioContext and release resources.
     */
    close() {
        if (this._closed) return;
        this._closed = true;

        this.ready = false;
        this.cleanup();
    }

    /**
     * Internal cleanup (no guard).
     */
    cleanup() {
        if (this.node) {
            this.node.port.onmessage = null;
            try { this.node.disconnect(); } catch (e) { /* ignore */ }
            this.node = null;
        }
        if (this.context && this.context.state !== 'closed') {
            this.context.onstatechange = null;
            try { this.context.close(); } catch (e) { /* ignore */ }
        }
        this.context = null;
    }

    /**
     * Handle messages from the AudioWorklet thread (diagnostics).
     * @param {MessageEvent} evt
     */
    _handleWorkletMessage(evt) {
        const msg = evt.data;
        if (!msg || !msg.type) return;

        if (msg.type === 'diag') {
            this._queueDepth = msg.queueDepth || 0;
            if (msg.underrunFrames > 0) {
                this._underrunCount += msg.underrunFrames;
                if (this._underrunCount % 4800 === 0) {
                    console.warn('[AudioPipeline] Underrun: ' + msg.underrunFrames +
                        ' frames, queue=' + msg.queueDepth);
                }
            }
        }
    }

    /**
     * Get diagnostic stats.
     * @returns {{ writtenSamples: number, queueDepth: number, underrunCount: number, ready: boolean }}
     */
    getStats() {
        return {
            writtenSamples: this._writtenSamples,
            queueDepth: this._queueDepth,
            underrunCount: this._underrunCount,
            ready: this.ready
        };
    }
}
