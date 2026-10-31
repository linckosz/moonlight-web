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
 * The viewer's microphone, sent to the host.
 *
 * Settings → "Send my microphone" asks for it at launch (`mic_enabled`). The
 * host grants it only when the microphone can land where the game is — the
 * host is the machine MoonlightWeb runs on, the viewer is its owner — and says
 * so (/start's `mic: true`, or `mic_why`). Then, by transport:
 *   - WebRTC: the host offers the audio section sendrecv instead of sendonly;
 *     this side answers it sendrecv and puts its getUserMedia track on the
 *     sender, at the quality's bitrate. No second track, no renegotiation, and
 *     the stream's own sound keeps its native player;
 *   - WebSocket (a tunnel with no UDP): MicSocketSender encodes the track and
 *     sends it on the stream's socket.
 *
 * Settings → "Microphone sound" picks the quality (MIC_QUALITY): the call-style
 * cleanup at 64 kbit/s, or none at all at 128 for a headset or a good mic.
 *
 * The microphone is opened only once the host said yes, so a viewer is never
 * asked for a permission that would lead nowhere. A standby view (seamless
 * quality switch) holds its microphone back until it is promoted, or the host
 * would hear the voice twice for as long as both legs run.
 */

import { MicSocketSender } from './MicSocketSender.js';

/**
 * Whether this browser can capture a microphone at all: a secure context
 * (HTTPS, which every MoonlightWeb page is) and mediaDevices.getUserMedia.
 *
 * @param {any} [nav]
 * @param {boolean} [secure]
 * @returns {boolean}
 */
export function micSupported(nav = globalThis.navigator, secure = globalThis.isSecureContext) {
    return secure !== false && typeof nav?.mediaDevices?.getUserMedia === 'function';
}

/**
 * Whether the host's offer asks to receive on the stream's audio section
 * (mid `audio`, a=sendrecv): that is how it says it took the microphone.
 *
 * @param {string} sdp
 * @returns {boolean}
 */
export function offerTakesMic(sdp) {
    if (typeof sdp !== 'string') return false;
    return sdp
        .split(/(?=^m=)/m)
        .some((section) => /^a=mid:audio\r?$/m.test(section) && /^a=sendrecv\r?$/m.test(section));
}

/**
 * The two ways to sound. `voice` cleans up the way a call does — echo
 * cancellation (the stream's own sound comes out of this device's speakers),
 * noise suppression, gain control — which also takes the life out of a voice;
 * `clean` leaves it as the microphone heard it, at twice the bitrate, for a
 * headset or a microphone that needs no help. Opus is transparent for a voice
 * well before 128 kbit/s; what was holding it back was the browser's default of
 * 32–64 and the processing.
 */
export const MIC_QUALITY = Object.freeze({
    voice: Object.freeze({ processing: true, bitrate: 64000 }),
    clean: Object.freeze({ processing: false, bitrate: 128000 }),
});

/** @param {string} [name] @returns {{processing: boolean, bitrate: number}} */
export function micQuality(name) {
    return MIC_QUALITY[name] || MIC_QUALITY.voice;
}

/**
 * What getUserMedia is asked for, at a quality.
 *
 * @param {{processing: boolean}} quality
 * @returns {MediaStreamConstraints}
 */
export function micConstraints(quality) {
    const p = quality.processing;
    return {
        audio: {
            echoCancellation: p,
            noiseSuppression: p,
            autoGainControl: p,
            channelCount: 1,
            sampleRate: 48000,
        },
        video: false,
    };
}

/**
 * Muted or not, carried from a retiring view to the one replacing it, so a
 * quality switch never unmutes a viewer who muted. A new stream starts live.
 */
let carriedMute = false;

/** The audio transceiver of the stream (mid `audio`), or null. */
function soundTransceiver(pc) {
    if (!pc || typeof pc.getTransceivers !== 'function') return null;
    return pc.getTransceivers().find((t) => t && t.mid === 'audio') || null;
}

export class MicUplink {
    /**
     * @param {object} [opts]
     * @param {boolean} [opts.held] - a standby view's: nothing is sent until release().
     * @param {string} [opts.quality] - a MIC_QUALITY name
     * @param {(c: MediaStreamConstraints) => Promise<MediaStream>} [opts.getUserMedia]
     * @param {typeof MicSocketSender} [opts.SocketSender] - for tests
     */
    constructor(opts = {}) {
        this._held = opts.held === true;
        this._quality = micQuality(opts.quality);
        this._SocketSender = opts.SocketSender || MicSocketSender;
        /** @type {((bytes: Uint8Array) => boolean)|null} the WebSocket path's sender */
        this._socketSend = null;
        /** @type {MicSocketSender|null} */
        this._socket = null;
        this._getUserMedia =
            opts.getUserMedia || ((c) => globalThis.navigator.mediaDevices.getUserMedia(c));
        // A standby takes over the live view's mute; a new stream starts live.
        if (!this._held) carriedMute = false;
        this._muted = carriedMute;
        /** @type {RTCPeerConnection|null} */
        this._pc = null;
        /** @type {MediaStreamTrack|null} */
        this._track = null;
        /** @type {Promise<MediaStreamTrack|null>|null} */
        this._acquiring = null;
        this._closed = false;
        /** 'idle' | 'live' | 'muted' | 'denied' | 'error' */
        this.state = 'idle';
        /** @type {((state: string) => void)|null} */
        this.onStateChange = null;
    }

    get muted() {
        return this._muted;
    }

    /**
     * After setRemoteDescription, before createAnswer: answer the audio
     * section sendrecv, when the host's offer asked for it.
     *
     * @param {RTCPeerConnection} pc
     * @param {string} offerSdp
     * @returns {boolean} whether the answer will send on it
     */
    prepare(pc, offerSdp) {
        if (this._closed || !offerTakesMic(offerSdp)) return false;
        const t = soundTransceiver(pc);
        if (!t) return false;
        try {
            t.direction = 'sendrecv';
        } catch (e) {
            console.warn('[Mic] could not answer the audio section sendrecv:', e);
            return false;
        }
        this._pc = pc;
        return true;
    }

    /**
     * After setLocalDescription: the microphone on the sender. Opens it on
     * the first call (the permission prompt, if any). Never throws.
     *
     * @returns {Promise<boolean>} whether a track is on the sender
     */
    async attach() {
        if (this._closed || this._held || !this._pc) return false;
        const track = await this._acquire();
        const t = soundTransceiver(this._pc);
        if (!track || !t || this._closed) return false;
        try {
            if (t.sender.track !== track) await t.sender.replaceTrack(track);
            track.enabled = !this._muted;
            await this._applyBitrate(t.sender);
            this._setState(this._muted ? 'muted' : 'live');
            console.info(
                '[Mic] sending "' +
                    (track.label || 'microphone') +
                    '" to the host, Opus at ' +
                    this._quality.bitrate / 1000 +
                    ' kbit/s',
            );
            return true;
        } catch (e) {
            console.warn('[Mic] could not put the microphone on the stream:', e);
            this._setState('error');
            return false;
        }
    }

    /**
     * The WebSocket transport's open socket: the microphone goes on it as
     * frames (MicSocketSender). Opens the microphone on the first call. Never
     * throws.
     *
     * @param {(bytes: Uint8Array) => boolean} send
     * @returns {Promise<boolean>} whether it is sending
     */
    async attachSocket(send) {
        this._socketSend = send;
        if (this._closed || this._held || this._socket) return false;
        const track = await this._acquire();
        if (!track || this._closed) return false;
        track.enabled = !this._muted;
        const sender = new this._SocketSender(track, send, this._quality);
        this._socket = sender;
        try {
            await sender.start();
            if (this._closed) return false;
            this._setState(this._muted ? 'muted' : 'live');
            return true;
        } catch (e) {
            console.warn('[Mic] could not send the microphone on the WebSocket:', e);
            sender.stop();
            this._socket = null;
            this._setState('error');
            return false;
        }
    }

    /** A standby view promoted to the live one: start sending. */
    release() {
        if (!this._held) return Promise.resolve(false);
        this._held = false;
        this._muted = carriedMute;
        return this._socketSend ? this.attachSocket(this._socketSend) : this.attach();
    }

    /** The quality's bitrate on the RTP sender; a browser without it keeps the offer's. */
    async _applyBitrate(sender) {
        if (typeof sender.getParameters !== 'function') return;
        try {
            const params = sender.getParameters();
            if (!params.encodings || params.encodings.length === 0) params.encodings = [{}];
            params.encodings[0].maxBitrate = this._quality.bitrate;
            await sender.setParameters(params);
        } catch (e) {
            console.info('[Mic] bitrate left to the browser:', e && e.message);
        }
    }

    /** @param {boolean} muted */
    setMuted(muted) {
        this._muted = muted === true;
        carriedMute = this._muted;
        if (this._track) this._track.enabled = !this._muted;
        if (this.state === 'live' || this.state === 'muted')
            this._setState(this._muted ? 'muted' : 'live');
    }

    toggleMuted() {
        this.setMuted(!this._muted);
        return this._muted;
    }

    /** The stream ended: the microphone is closed, its indicator goes out. */
    close() {
        this._closed = true;
        if (this._socket) this._socket.stop();
        this._socket = null;
        if (this._track) {
            try {
                this._track.stop();
            } catch (e) {
                /* already ended */
            }
        }
        this._track = null;
        this._pc = null;
    }

    _acquire() {
        if (this._track && this._track.readyState !== 'ended') return Promise.resolve(this._track);
        if (this._acquiring) return this._acquiring;
        this._acquiring = this._getUserMedia(micConstraints(this._quality))
            .then((stream) => {
                const track = stream.getAudioTracks()[0] || null;
                if (this._closed) {
                    stream.getTracks().forEach((x) => x.stop());
                    return null;
                }
                this._track = track;
                if (!track) this._setState('error');
                return track;
            })
            .catch((e) => {
                const denied = e && (e.name === 'NotAllowedError' || e.name === 'SecurityError');
                console.warn('[Mic] getUserMedia refused:', e && e.name, e && e.message);
                this._setState(denied ? 'denied' : 'error');
                return null;
            })
            .finally(() => {
                this._acquiring = null;
            });
        return this._acquiring;
    }

    _setState(state) {
        if (this.state === state) return;
        this.state = state;
        if (this.onStateChange) {
            try {
                this.onStateChange(state);
            } catch (e) {
                console.warn('[Mic] state listener failed:', e);
            }
        }
    }
}
