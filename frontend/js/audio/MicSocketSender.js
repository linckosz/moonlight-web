/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 MoonlightWeb contributors
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
 * The microphone on the WebSocket transport, the one a tunnel without UDP
 * leaves (Cloudflare Tunnel with no port forward): there is no RTP track to put
 * it on, so this page encodes it and sends it on the stream's own socket.
 *
 *   track → AudioWorklet (mic-capture-processor.js, 20 ms frames at 48 kHz)
 *         → WebCodecs AudioEncoder, Opus at the quality's bitrate
 *           (or 16-bit PCM where the browser has no Opus encoder)
 *         → [0x4D 'M'][codec][sequence:2][payload] on the socket
 *
 * The host reads that framing in backend/src/streaming/MicRtp.h. A socket is
 * reliable and ordered, so nothing needs FEC; what it can do is back up, and a
 * voice that arrives late is worse than one that drops a frame — so a frame is
 * skipped while the socket holds more than BACKLOG_BYTES.
 */

export const MIC_FRAME_TAG = 0x4d;
export const MIC_CODEC_OPUS = 1;
export const MIC_CODEC_PCM16 = 2;
const RATE = 48000;
const FRAME_US = 20000;
const BACKLOG_BYTES = 128 * 1024;

/**
 * One microphone message: the header the host parses, then the payload.
 *
 * @param {number} codec
 * @param {number} sequence - wraps at 16 bits
 * @param {Uint8Array} payload
 * @returns {Uint8Array}
 */
export function micFrame(codec, sequence, payload) {
    const out = new Uint8Array(4 + payload.length);
    out[0] = MIC_FRAME_TAG;
    out[1] = codec;
    out[2] = (sequence >> 8) & 0xff;
    out[3] = sequence & 0xff;
    out.set(payload, 4);
    return out;
}

/**
 * Float samples as 16-bit little-endian PCM, clipped.
 *
 * @param {Float32Array} samples
 * @returns {Uint8Array}
 */
export function pcm16(samples) {
    const out = new Uint8Array(samples.length * 2);
    const view = new DataView(out.buffer);
    for (let i = 0; i < samples.length; i++) {
        const s = Math.max(-1, Math.min(1, samples[i]));
        view.setInt16(i * 2, s < 0 ? s * 0x8000 : s * 0x7fff, true);
    }
    return out;
}

/** The Opus encoder this browser has for the quality, or null. */
async function opusConfig(bitrate) {
    if (typeof globalThis.AudioEncoder !== 'function') return null;
    const base = { codec: 'opus', sampleRate: RATE, numberOfChannels: 1, bitrate };
    for (const config of [{ ...base, opus: { frameDuration: FRAME_US } }, base]) {
        try {
            const { supported } = await globalThis.AudioEncoder.isConfigSupported(config);
            if (supported) return config;
        } catch (_e) {
            /* an older encoder refusing the opus dictionary: try without it */
        }
    }
    return null;
}

export class MicSocketSender {
    /**
     * @param {MediaStreamTrack} track
     * @param {(bytes: Uint8Array) => boolean} send - false when the socket is
     *   too backed up to take it (the frame is then dropped)
     * @param {{bitrate: number}} quality
     */
    constructor(track, send, quality) {
        this._track = track;
        this._send = send;
        this._bitrate = quality.bitrate;
        this._seq = 0;
        this._ts = 0;
        this._ctx = null;
        this._node = null;
        this._encoder = null;
        this._stopped = false;
        /** 'opus' | 'pcm' once started */
        this.codec = '';
    }

    /** Starts capturing. Throws when the page cannot capture at all. */
    async start() {
        const AC = globalThis.AudioContext || globalThis.webkitAudioContext;
        const ctx = new AC({ sampleRate: RATE, latencyHint: 'interactive' });
        this._ctx = ctx;
        if (ctx.sampleRate !== RATE) {
            // The host plays 48 kHz; a context that ignored the request is rare
            // (old Safari), and would be heard at the wrong pitch.
            throw new Error('audio context runs at ' + ctx.sampleRate + ' Hz, not 48 kHz');
        }
        await ctx.audioWorklet.addModule('/js/audio/mic-capture-processor.js');
        if (this._stopped) return;

        const config = await opusConfig(this._bitrate);
        if (config) {
            this._encoder = new globalThis.AudioEncoder({
                output: (chunk) => {
                    const bytes = new Uint8Array(chunk.byteLength);
                    chunk.copyTo(bytes);
                    this._emit(MIC_CODEC_OPUS, bytes);
                },
                error: (e) => console.warn('[Mic] Opus encoder failed:', e),
            });
            this._encoder.configure(config);
            this.codec = 'opus';
        } else {
            this.codec = 'pcm';
        }

        const source = ctx.createMediaStreamSource(new MediaStream([this._track]));
        this._node = new AudioWorkletNode(ctx, 'mw-mic-capture', {
            numberOfInputs: 1,
            numberOfOutputs: 1,
            channelCount: 1,
        });
        this._node.port.onmessage = (e) => this._onFrame(e.data);
        source.connect(this._node);
        // Silent (the processor writes nothing), but pulled: a node nobody
        // listens to is never run.
        this._node.connect(ctx.destination);
        if (ctx.state !== 'running') await ctx.resume().catch(() => {});
        console.info(
            '[Mic] sending on the WebSocket as ' +
                (this.codec === 'opus'
                    ? 'Opus at ' + this._bitrate / 1000 + ' kbit/s'
                    : '16-bit PCM'),
        );
    }

    stop() {
        this._stopped = true;
        try {
            if (this._node) this._node.port.postMessage('stop');
            if (this._encoder && this._encoder.state !== 'closed') this._encoder.close();
            if (this._ctx && this._ctx.state !== 'closed') this._ctx.close().catch(() => {});
        } catch (_e) {
            /* already torn down */
        }
        this._node = null;
        this._encoder = null;
        this._ctx = null;
    }

    /** @param {Float32Array} samples - one 20 ms frame */
    _onFrame(samples) {
        if (this._stopped) return;
        if (!this._encoder) {
            this._emit(MIC_CODEC_PCM16, pcm16(samples));
            return;
        }
        // An encoder that falls behind (a throttled tab) would only add delay.
        if (this._encoder.encodeQueueSize > 4) return;
        const data = new globalThis.AudioData({
            format: 'f32-planar',
            sampleRate: RATE,
            numberOfFrames: samples.length,
            numberOfChannels: 1,
            timestamp: this._ts,
            data: samples,
        });
        this._ts += FRAME_US;
        this._encoder.encode(data);
        data.close();
    }

    _emit(codec, payload) {
        const frame = micFrame(codec, this._seq, payload);
        // The sequence moves on either way: a dropped frame is a gap the host
        // conceals, never two frames under one number.
        this._seq = (this._seq + 1) & 0xffff;
        this._send(frame);
    }
}

/** How much the socket may hold before a microphone frame is dropped. */
export const MIC_SOCKET_BACKLOG = BACKLOG_BYTES;
