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

import { IS_IOS } from '../util/BrowserDetect.js';
import * as iosAudioUnlock from './iosAudioUnlock.js';

/**
 * AudioPipeline — manages AudioContext + AudioWorkletNode for streaming audio.
 *
 * Receives raw Opus packets via write(), decodes them with the WebCodecs
 * AudioDecoder (native browser Opus decoder), then transfers the resulting
 * Float32 interleaved PCM to the AudioWorklet thread for real-time playback.
 * Handles lifecycle: init, resume, close.
 *
 * IMPORTANT: moonlight-common-c delivers ENCODED Opus packets (see Limelight.h
 * "decodeAndPlaySample provides Opus audio data"), NOT PCM. The backend forwards
 * those Opus bytes verbatim over every transport (DataChannel / media-DC / WSS),
 * so decoding must happen here, client-side.
 *
 * Requirements:
 *   - Browser must support AudioWorklet (Chrome 66+, Firefox 76+, Safari 14.1+).
 *   - Browser must support WebCodecs AudioDecoder (Chrome/Edge 94+, Firefox 130+,
 *     Safari 16.4+). Without it audio stays silent (logged once).
 *   - AudioContext created at 48000 Hz (Opus native rate).
 *
 * Usage:
 *   const audio = new AudioPipeline();
 *   await audio.init();               // Load AudioWorklet, create node + decoder
 *   audio.write(opusPacket);           // Feed one Opus packet
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
        this.channels = options.channels || 2;
        this.workletUrl = options.workletUrl || '/js/audio/audio-processor.js';
        // Pitch-preserving time-stretch (WSOLA) in the worklet. On by default;
        // disabled via the server kill switch (settings.json audio_time_stretch).
        this.timeStretch = options.timeStretch !== false;

        /** @type {Worker|null} Dedicated Opus decode worker (off the main thread). */
        this._worker = null;
        /** @type {boolean} True when audio is decoded in the worker. */
        this._useWorker = false;

        /** @type {AudioContext|null} */
        this.context = null;

        /** @type {AudioWorkletNode|null} */
        this.node = null;

        /** @type {AudioDecoder|null} WebCodecs Opus decoder. */
        this.decoder = null;

        /** @type {object|null} WASM Opus decoder (iOS fallback). */
        this._wasmDecoder = null;

        /** @type {'webcodecs'|'wasm'|null} Active decode path. */
        this._mode = null;

        /** @type {boolean} True after init() succeeds and node is ready. */
        this.ready = false;

        /** @type {boolean} True when close() has been called. */
        this._closed = false;

        // Monotonic timestamp (µs) fed to EncodedAudioChunk. Opus packets are
        // ~5 ms; exact value is irrelevant to decoding (passed through only).
        this._chunkTs = 0;

        // Reusable planar scratch buffer for AudioData->interleaved conversion.
        this._planeScratch = null;

        // Diagnostics
        this._writtenSamples = 0; // Total stereo-frames decoded
        this._queueDepth = 0; // Estimated queue depth inside the worklet
        this._underrunCount = 0; // Total underrun frames (from worklet diag)
        this._decodeErrors = 0; // Opus decode errors

        // Bound handlers
        this._onWorkletMessage = (evt) => this._handleWorkletMessage(evt);
        this._onDecodedAudio = (audioData) => this._handleDecodedAudio(audioData);
        this._onDecoderError = (err) => this._handleDecoderError(err);
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
            // iOS: adopt the AudioContext created + unlocked inside the launch
            // gesture (see iosAudioUnlock.prepareForLaunch). A context created
            // here, outside any gesture, never acquires the loudspeaker route on
            // iOS. Off iOS (or if none prepared) create our own as before.
            const adopted = iosAudioUnlock.adoptContext();
            this.context = adopted || new AudioContext({ sampleRate: this.sampleRate });

            // Check the actual sample rate (might differ)
            if (this.context.sampleRate !== this.sampleRate) {
                console.warn(
                    '[AudioPipeline] Sample rate mismatch: requested=' +
                        this.sampleRate +
                        ', actual=' +
                        this.context.sampleRate,
                );

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

            // Create the AudioWorkletNode — force a stereo output bus, otherwise
            // the output may default to mono and the processor (which writes
            // out[0]/out[1]) would emit pure silence.
            this.node = new AudioWorkletNode(this.context, 'audio-processor', {
                outputChannelCount: [2],
            });
            this.node.port.onmessage = this._onWorkletMessage;
            this.node.connect(this.context.destination);

            // Push runtime config to the worklet (time-stretch kill switch).
            this.node.port.postMessage({ type: 'config', timeStretch: this.timeStretch });

            // Guard: if close() was called during node setup, abort
            if (this._closed) {
                this.cleanup();
                return false;
            }

            // Prefer decoding in a dedicated worker: keeps audio decode + delivery
            // independent from main-thread video jank, so sound keeps flowing while
            // the video stutters. Fall back to main-thread decode if unavailable.
            this._useWorker = await this._setupWorker();
            if (this._closed) {
                this.cleanup();
                return false;
            }
            if (!this._useWorker) {
                console.warn('[AudioPipeline] Decode worker unavailable — main-thread decode');
                if (!(await this._setupDecoder())) {
                    console.error(
                        '[AudioPipeline] No Opus decoder available — audio will be silent',
                    );
                    this.cleanup();
                    return false;
                }
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
            console.log(
                '[AudioPipeline] Initialised: rate=' +
                    this.context.sampleRate +
                    ', state=' +
                    this.context.state +
                    ', baseLatency=' +
                    (this.context.baseLatency || '?') +
                    's',
            );

            // Autoplay safety net: if the context starts suspended (init ran
            // outside the user-gesture stack), resume it on the first user
            // interaction with the page — which a stream session always has.
            // On iOS we arm it unconditionally so the unmute element is started
            // by a gesture even when the context is already running (else it
            // would never play and audio stays on the "ambient" session).
            if (this.context.state === 'suspended' || IS_IOS) {
                this._armGestureResume();
            }
            return true;
        } catch (err) {
            console.error('[AudioPipeline] Initialisation failed:', err.message, err);
            this.cleanup();
            return false;
        }
    }

    /**
     * Create the dedicated Opus decode worker and wire a direct PCM channel
     * worker → AudioWorklet (bypassing the main thread). Waits for the worker to
     * confirm a usable decoder.
     * @returns {Promise<boolean>} true if the worker path is active.
     */
    async _setupWorker() {
        if (typeof Worker === 'undefined' || typeof MessageChannel === 'undefined') return false;
        let worker;
        try {
            worker = new Worker(new URL('./audio-decode-worker.js', import.meta.url), {
                type: 'module',
            });
        } catch (err) {
            console.warn('[AudioPipeline] Decode worker construction failed:', err.message);
            return false;
        }

        // Direct PCM channel: worker → worklet (never touches the main thread).
        const channel = new MessageChannel();
        this.node.port.postMessage({ type: 'pcm-port', port: channel.port1 }, [channel.port1]);
        worker.postMessage({ type: 'pcm-port', port: channel.port2 }, [channel.port2]);

        // Wait for the worker to confirm a working decoder (or fail / time out).
        const ready = new Promise((resolve) => {
            const timer = setTimeout(() => resolve(false), 3000);
            worker.onmessage = (e) => {
                const m = e.data;
                if (!m) return;
                if (m.type === 'ready') {
                    clearTimeout(timer);
                    console.log('[AudioPipeline] Decode worker ready (' + m.mode + ')');
                    resolve(true);
                } else if (m.type === 'fail') {
                    clearTimeout(timer);
                    resolve(false);
                } else if (m.type === 'stat') {
                    // Diagnostics for the stats overlay.
                    this._writtenSamples = m.writtenSamples || this._writtenSamples;
                    this._decodeErrors = m.decodeErrors || this._decodeErrors;
                }
            };
            worker.onerror = (err) => {
                clearTimeout(timer);
                console.warn('[AudioPipeline] Decode worker error:', err.message || err);
                resolve(false);
            };
        });
        worker.postMessage({ type: 'init', sampleRate: this.sampleRate, channels: this.channels });

        if (!(await ready)) {
            try {
                worker.terminate();
            } catch (e) {
                /* ignore */
            }
            return false;
        }
        this._worker = worker;
        return true;
    }

    /**
     * Create the Opus decoder. Prefers the native WebCodecs AudioDecoder;
     * falls back to a WASM decoder (iOS / WebKit, where WebCodecs Opus is
     * unavailable). Sets this._mode to 'webcodecs' or 'wasm'.
     * @returns {Promise<boolean>} true if a decoder was created.
     */
    async _setupDecoder() {
        // --- Native WebCodecs path ---
        if (typeof AudioDecoder !== 'undefined') {
            try {
                this.decoder = new AudioDecoder({
                    output: this._onDecodedAudio,
                    error: this._onDecoderError,
                });
                // Plain Opus packets (no Ogg container) — no description needed for
                // mono/stereo. Sunshine streams 48 kHz stereo by default.
                this.decoder.configure({
                    codec: 'opus',
                    sampleRate: this.sampleRate,
                    numberOfChannels: this.channels,
                });
                this._mode = 'webcodecs';
                console.log(
                    '[AudioPipeline] Opus AudioDecoder (WebCodecs) configured: ' +
                        this.sampleRate +
                        'Hz, ' +
                        this.channels +
                        'ch',
                );
                return true;
            } catch (err) {
                console.warn(
                    '[AudioPipeline] WebCodecs AudioDecoder unavailable (' +
                        err.message +
                        ') — trying WASM fallback',
                );
                this.decoder = null;
            }
        }

        // --- WASM fallback path (iOS / WebKit) ---
        try {
            const { OpusDecoder } = await import('./opusWasm.js');
            if (!OpusDecoder) throw new Error('opus-decoder bundle missing OpusDecoder');
            if (this._closed) return false;

            this._wasmDecoder = new OpusDecoder({
                channels: this.channels,
                sampleRate: this.sampleRate,
            });
            await this._wasmDecoder.ready;
            if (this._closed) {
                this._freeWasmDecoder();
                return false;
            }

            this._mode = 'wasm';
            console.log(
                '[AudioPipeline] Opus WASM decoder ready: ' +
                    this.sampleRate +
                    'Hz, ' +
                    this.channels +
                    'ch',
            );
            return true;
        } catch (err) {
            console.error('[AudioPipeline] WASM Opus decoder setup failed:', err.message, err);
            this._freeWasmDecoder();
            return false;
        }
    }

    /** Free the WASM decoder if present. */
    _freeWasmDecoder() {
        if (this._wasmDecoder) {
            try {
                this._wasmDecoder.free();
            } catch (e) {
                /* ignore */
            }
            this._wasmDecoder = null;
        }
    }

    /**
     * Decode one Opus packet. The resulting PCM is delivered asynchronously to
     * the AudioWorklet via the decoder output callback.
     *
     * NOTE: `sample` is typically a sub-view of a larger transport buffer; we
     * copy only the packet's bytes (slice) before handing them to the decoder.
     *
     * @param {Uint8Array} sample - One raw Opus packet.
     */
    write(sample) {
        if (!this.ready || this._closed) return;
        if (!sample || sample.byteLength === 0) return;

        // Worker path: transfer a standalone copy of just this packet. Decode
        // happens off the main thread; PCM goes straight to the worklet.
        if (this._useWorker) {
            const buf = sample.slice().buffer;
            this._worker.postMessage({ type: 'opus', data: buf }, [buf]);
            return;
        }

        if (this._mode === 'wasm') {
            this._writeWasm(sample);
            return;
        }

        // --- WebCodecs path (async output via callback) ---
        if (!this.decoder || this.decoder.state !== 'configured') return;
        try {
            const chunk = new EncodedAudioChunk({
                type: 'key', // every Opus packet is independently decodable
                timestamp: this._chunkTs,
                data: sample.slice(), // standalone copy of just this packet
            });
            // 5 ms nominal packet spacing (µs); exact value unused downstream.
            this._chunkTs += 5000;
            this.decoder.decode(chunk);
        } catch (err) {
            if (!this._decodeSubmitErrorLogged) {
                console.warn('[AudioPipeline] decode() submit failed:', err.message);
                this._decodeSubmitErrorLogged = true;
            }
        }
    }

    /**
     * WASM decode path (synchronous). Decodes one Opus packet to planar Float32
     * and posts interleaved PCM to the worklet.
     * @param {Uint8Array} sample - One raw Opus packet.
     */
    _writeWasm(sample) {
        let res;
        try {
            // decodeFrame wants a standalone Uint8Array of just this packet.
            res = this._wasmDecoder.decodeFrame(sample.slice());
        } catch (err) {
            this._handleDecoderError(err);
            return;
        }
        if (!res || !res.channelData || res.samplesDecoded <= 0) return;

        if (!this._firstDecodeLogged) {
            console.log(
                '[AudioPipeline] First decoded (WASM): channels=' +
                    res.channelData.length +
                    ' frames=' +
                    res.samplesDecoded +
                    ' rate=' +
                    res.sampleRate,
            );
            this._firstDecodeLogged = true;
        }
        this._postPlanar(res.channelData, res.samplesDecoded);
    }

    /**
     * Interleave planar Float32 channels and transfer them to the worklet.
     * Duplicates a mono source across both output channels.
     * @param {Float32Array[]} planes - Per-channel Float32 sample arrays.
     * @param {number} frames - Number of stereo-frames (samples per channel).
     */
    _postPlanar(planes, frames) {
        if (this._closed || !this.node || !planes.length) return;
        const outCh = this.channels;
        const interleaved = new Float32Array(frames * outCh);
        for (let c = 0; c < outCh; c++) {
            const src = planes[c < planes.length ? c : 0];
            for (let i = 0; i < frames; i++) {
                interleaved[i * outCh + c] = src[i];
            }
        }
        this._writtenSamples += frames;
        if (!this._firstPostLogged) {
            console.log('[AudioPipeline] First PCM posted to worklet (' + frames + ' frames)');
            this._firstPostLogged = true;
        }
        this.node.port.postMessage(interleaved.buffer, [interleaved.buffer]);
    }

    /**
     * Decoder output callback: convert AudioData to Float32 interleaved stereo
     * and transfer it to the AudioWorklet thread.
     * @param {AudioData} audioData
     */
    _handleDecodedAudio(audioData) {
        if (this._closed || !this.node) {
            audioData.close();
            return;
        }

        try {
            const frames = audioData.numberOfFrames;
            const srcCh = audioData.numberOfChannels;
            const outCh = this.channels; // worklet expects 2 (stereo)

            if (!this._firstDecodeLogged) {
                console.log(
                    '[AudioPipeline] First decoded AudioData: format=' +
                        audioData.format +
                        ' channels=' +
                        srcCh +
                        ' frames=' +
                        frames +
                        ' rate=' +
                        audioData.sampleRate,
                );
                this._firstDecodeLogged = true;
            }

            // Interleaved Float32 buffer for the worklet.
            const interleaved = new Float32Array(frames * outCh);

            if (!this._planeScratch || this._planeScratch.length < frames) {
                this._planeScratch = new Float32Array(frames);
            }
            const plane = this._planeScratch;

            // Decoder native format. Opus in Chrome yields 'f32-planar'; copy
            // those planes directly (no conversion). Otherwise ask copyTo to
            // convert to f32-planar (s16, interleaved, etc.).
            const fmt = audioData.format || '';
            const nativePlanarF32 = fmt === 'f32-planar';

            for (let c = 0; c < outCh; c++) {
                // Map output channel -> source plane (duplicate mono to both sides).
                const srcPlane = c < srcCh ? c : 0;
                if (nativePlanarF32) {
                    audioData.copyTo(plane, { planeIndex: srcPlane });
                } else {
                    audioData.copyTo(plane, { planeIndex: srcPlane, format: 'f32-planar' });
                }
                for (let i = 0; i < frames; i++) {
                    interleaved[i * outCh + c] = plane[i];
                }
            }

            this._writtenSamples += frames;
            if (!this._firstPostLogged) {
                console.log('[AudioPipeline] First PCM posted to worklet (' + frames + ' frames)');
                this._firstPostLogged = true;
            }
            this.node.port.postMessage(interleaved.buffer, [interleaved.buffer]);
        } catch (err) {
            if (!this._convertErrorLogged) {
                console.warn('[AudioPipeline] AudioData conversion failed:', err.message);
                this._convertErrorLogged = true;
            }
        } finally {
            audioData.close();
        }
    }

    /**
     * Decoder error callback. Opus is resilient; log sparingly and keep going.
     * @param {DOMException} err
     */
    _handleDecoderError(err) {
        this._decodeErrors++;
        if (this._decodeErrors <= 5 || this._decodeErrors % 200 === 0) {
            console.warn(
                '[AudioPipeline] Opus decode error #' + this._decodeErrors + ': ' + err.message,
            );
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

        // Promote the iOS audio session on this same user activation (no-op off
        // iOS). The launch click already unlocks it; this is a belt-and-braces
        // retry whenever resume() runs in-gesture.
        iosAudioUnlock.unlock();

        return this.context.state === 'running';
    }

    /**
     * Register one-time listeners that resume the AudioContext on the first
     * user gesture. Removes itself once the context is running.
     */
    _armGestureResume() {
        if (this._gestureResumeArmed) return;
        this._gestureResumeArmed = true;

        const events = ['pointerdown', 'mousedown', 'keydown', 'touchstart', 'click'];
        const tryResume = async () => {
            if (this._closed || !this.context) {
                cleanup();
                return;
            }
            try {
                await this.context.resume();
            } catch (e) {
                /* ignore — may need another gesture */
            }
            // Promote the iOS audio session on the same gesture.
            iosAudioUnlock.unlock();
            // Stay armed until the context runs AND (on iOS) the unlock element
            // is actually playing — a first .play() may be rejected and need
            // another gesture.
            const unmuteOk = !IS_IOS || iosAudioUnlock.isActive();
            if (this.context && this.context.state === 'running' && unmuteOk) {
                console.log('[AudioPipeline] AudioContext resumed via user gesture');
                cleanup();
            }
        };
        const cleanup = () => {
            for (const ev of events) {
                window.removeEventListener(ev, tryResume, true);
            }
            this._gestureResumeCleanup = null;
        };
        this._gestureResumeCleanup = cleanup;
        for (const ev of events) {
            window.addEventListener(ev, tryResume, true);
        }
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
        if (this._gestureResumeCleanup) {
            try {
                this._gestureResumeCleanup();
            } catch (e) {
                /* ignore */
            }
        }
        // NOTE: the iOS playback-session hold (iosAudioUnlock) is intentionally
        // NOT released here. cleanup() also runs on a transport/codec fallback
        // relaunch teardown, which re-launches WITHOUT a user gesture — releasing
        // would drop the session back to "ambient" with no way to re-unlock until
        // the user taps. The hold is released on the real streaming exit, in
        // MoonlightApp._onStreamingQuit().
        if (this._worker) {
            try {
                this._worker.postMessage({ type: 'close' });
            } catch (e) {
                /* ignore */
            }
            try {
                this._worker.terminate();
            } catch (e) {
                /* ignore */
            }
            this._worker = null;
        }
        this._useWorker = false;
        if (this.decoder) {
            try {
                if (this.decoder.state !== 'closed') this.decoder.close();
            } catch (e) {
                /* ignore */
            }
            this.decoder = null;
        }
        this._freeWasmDecoder();
        if (this.node) {
            this.node.port.onmessage = null;
            try {
                this.node.disconnect();
            } catch (e) {
                /* ignore */
            }
            this.node = null;
        }
        if (this.context && this.context.state !== 'closed') {
            this.context.onstatechange = null;
            try {
                this.context.close();
            } catch (e) {
                /* ignore */
            }
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

        if (msg.type === 'started') {
            console.log(
                '[AudioPipeline] Worklet playback started: outChannels=' +
                    msg.outChannels +
                    ' queuedFrames=' +
                    msg.queuedFrames +
                    ' ctxState=' +
                    (this.context ? this.context.state : '?'),
            );
            return;
        }

        if (msg.type === 'diag') {
            this._queueDepth = msg.queueDepth || 0;
            this._bufferTargetMs = msg.targetMs || 0;
            if (msg.underrunEvents > 0) {
                this._underrunCount += msg.underrunFrames || 0;
                console.warn(
                    '[AudioPipeline] Audio underruns: ' +
                        msg.underrunEvents +
                        ' episode(s) in last ~1s, buffer target now ' +
                        msg.targetMs +
                        'ms (queue=' +
                        msg.queuedFrames +
                        ' frames)',
                );
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
            ready: this.ready,
        };
    }
}
