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
 * Dedicated Opus decode worker.
 *
 * Decoding the audio off the main thread keeps it independent from the video
 * pipeline: when the main thread janks (heavy video decode/render), audio decode
 * and delivery are unaffected, so the sound keeps flowing while the video
 * stutters. The decoded PCM is posted DIRECTLY to the AudioWorklet over a
 * transferred MessagePort (worker → worklet), never touching the main thread.
 *
 * Messages in (from AudioPipeline on the main thread):
 *   { type: 'init', sampleRate, channels }
 *   { type: 'pcm-port', port }   MessagePort to the AudioWorklet (transferred)
 *   { type: 'opus', data }       one raw Opus packet (ArrayBuffer, transferred)
 *   { type: 'close' }
 *
 * Messages out (to AudioPipeline):
 *   { type: 'ready', mode }      'webcodecs' | 'wasm'
 *   { type: 'fail', message }    no decoder available
 *   { type: 'stat', writtenSamples, decodeErrors }
 */

let decoder = null; // WebCodecs AudioDecoder
let wasmDecoder = null; // WASM Opus decoder (iOS / WebKit fallback)
let mode = null; // 'webcodecs' | 'wasm'
let pcmPort = null; // MessagePort to the AudioWorklet

let sampleRate = 48000;
let channels = 2;
let chunkTs = 0; // monotonic µs fed to EncodedAudioChunk (pass-through only)
let planeScratch = null;

let writtenSamples = 0;
let decodeErrors = 0;

/** Post interleaved Float32 PCM straight to the worklet (transfer the buffer). */
function postPCM(buffer) {
    if (pcmPort) pcmPort.postMessage(buffer, [buffer]);
}

/** WebCodecs decoder output: convert AudioData → interleaved stereo, post it. */
function onDecoded(audioData) {
    try {
        const frames = audioData.numberOfFrames;
        const srcCh = audioData.numberOfChannels;
        const outCh = channels;

        const interleaved = new Float32Array(frames * outCh);
        if (!planeScratch || planeScratch.length < frames) {
            planeScratch = new Float32Array(frames);
        }
        const plane = planeScratch;

        const nativePlanarF32 = (audioData.format || '') === 'f32-planar';
        for (let c = 0; c < outCh; c++) {
            const srcPlane = c < srcCh ? c : 0; // duplicate mono to both sides
            if (nativePlanarF32) {
                audioData.copyTo(plane, { planeIndex: srcPlane });
            } else {
                audioData.copyTo(plane, { planeIndex: srcPlane, format: 'f32-planar' });
            }
            for (let i = 0; i < frames; i++) {
                interleaved[i * outCh + c] = plane[i];
            }
        }
        writtenSamples += frames;
        postPCM(interleaved.buffer);
    } catch (err) {
        onError(err);
    } finally {
        audioData.close();
    }
}

/** WASM decoder path (synchronous): planar Float32 → interleaved, post it. */
function decodeWasm(bytes) {
    let res;
    try {
        res = wasmDecoder.decodeFrame(bytes);
    } catch (err) {
        onError(err);
        return;
    }
    if (!res || !res.channelData || res.samplesDecoded <= 0) return;
    const frames = res.samplesDecoded;
    const planes = res.channelData;
    const outCh = channels;
    const interleaved = new Float32Array(frames * outCh);
    for (let c = 0; c < outCh; c++) {
        const src = planes[c < planes.length ? c : 0];
        for (let i = 0; i < frames; i++) {
            interleaved[i * outCh + c] = src[i];
        }
    }
    writtenSamples += frames;
    postPCM(interleaved.buffer);
}

function onError(err) {
    decodeErrors++;
    if (decodeErrors <= 5 || decodeErrors % 200 === 0) {
        console.warn('[audio-worker] Opus decode error #' + decodeErrors + ': ' + err.message);
    }
}

/** Create the Opus decoder. WebCodecs preferred, WASM fallback (iOS/WebKit). */
async function setupDecoder() {
    if (typeof AudioDecoder !== 'undefined') {
        try {
            decoder = new AudioDecoder({ output: onDecoded, error: onError });
            decoder.configure({ codec: 'opus', sampleRate, numberOfChannels: channels });
            mode = 'webcodecs';
            return true;
        } catch (err) {
            console.warn(
                '[audio-worker] WebCodecs unavailable (' + err.message + ') — WASM fallback',
            );
            decoder = null;
        }
    }
    try {
        const { OpusDecoder } = await import('./opusWasm.js');
        if (!OpusDecoder) throw new Error('opus-decoder bundle missing OpusDecoder');
        wasmDecoder = new OpusDecoder({ channels, sampleRate });
        await wasmDecoder.ready;
        mode = 'wasm';
        return true;
    } catch (err) {
        console.error('[audio-worker] WASM Opus decoder setup failed:', err.message);
        return false;
    }
}

/** Decode one raw Opus packet. */
function decodeOpus(buffer) {
    const bytes = new Uint8Array(buffer);
    if (bytes.byteLength === 0) return;
    if (mode === 'wasm') {
        decodeWasm(bytes);
        return;
    }
    if (!decoder || decoder.state !== 'configured') return;
    try {
        decoder.decode(new EncodedAudioChunk({ type: 'key', timestamp: chunkTs, data: bytes }));
        chunkTs += 5000; // 5 ms nominal spacing (µs); exact value unused downstream
    } catch (err) {
        onError(err);
    }
}

self.onmessage = async (evt) => {
    const msg = evt.data;
    if (!msg) return;

    switch (msg.type) {
        case 'init':
            sampleRate = msg.sampleRate || 48000;
            channels = msg.channels || 2;
            if (await setupDecoder()) {
                self.postMessage({ type: 'ready', mode });
            } else {
                self.postMessage({ type: 'fail', message: 'no Opus decoder available' });
            }
            break;
        case 'pcm-port':
            pcmPort = msg.port;
            break;
        case 'opus':
            decodeOpus(msg.data);
            break;
        case 'close':
            try {
                if (decoder && decoder.state !== 'closed') decoder.close();
            } catch (e) {
                /* ignore */
            }
            try {
                if (wasmDecoder) wasmDecoder.free();
            } catch (e) {
                /* ignore */
            }
            decoder = null;
            wasmDecoder = null;
            self.close();
            break;
        default:
            break;
    }
};

// Lightweight diagnostics for the stats overlay (~1 Hz).
setInterval(() => {
    self.postMessage({ type: 'stat', writtenSamples, decodeErrors });
}, 1000);
