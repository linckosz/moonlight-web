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
 * Settings → "Microphone" says which one, or none. Unless it is off, the launch
 * asks the host for it (`mic_enabled`). The host grants it only when the
 * microphone can land where the game is — the host is the machine MoonlightWeb
 * runs on, the viewer is its owner — and says so (/start's `mic: true`, or
 * `mic_why`). Then, by transport:
 *   - WebRTC: the host offers the audio section sendrecv instead of sendonly;
 *     this side answers it sendrecv, and the sender carries the getUserMedia
 *     track while the microphone is open, at the quality's bitrate. No second
 *     track, no renegotiation, and the stream's own sound keeps its native
 *     player;
 *   - WebSocket (a tunnel with no UDP): MicSocketSender encodes the track and
 *     sends it on the stream's socket.
 *
 * Settings → "Microphone sound" picks the quality (MIC_QUALITY): the call-style
 * cleanup at 64 kbit/s, or none at all at 128 for a headset or a good mic.
 *
 * Every stream starts with the microphone closed: it opens on the viewer's click
 * or shortcut, and only then is the permission asked. Cutting it stops the track,
 * so the browser's indicator goes out and nothing is captured; the host then
 * receives nothing rather than silence. A standby view (seamless quality switch)
 * holds its microphone back until it is promoted, or the host would hear the
 * voice twice for as long as both legs run.
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
 * Whether the launch should ask the host for the microphone. A viewer who never
 * touched the list has the default, the system's microphone; `mic_enabled` alone
 * cannot say that, as every saved settings object carries it.
 *
 * @param {{mic_chosen?: boolean, mic_enabled?: boolean}} [settings]
 * @returns {boolean}
 */
export function micRequested(settings) {
    return settings?.mic_chosen === true ? settings.mic_enabled === true : true;
}

const DEVICE_KEY = 'mw-mic-device';

/**
 * The microphone picked on this browser, or null for the system's. Its own key,
 * not a field of the streaming settings: those go to the host with every launch,
 * and a device's name has no business there.
 *
 * @param {Pick<Storage, 'getItem'>} [storage]
 * @returns {{id: string, label: string}|null}
 */
export function loadMicDevice(storage) {
    try {
        const d = JSON.parse((storage || globalThis.localStorage).getItem(DEVICE_KEY));
        if (!d || typeof d.id !== 'string' || !d.id) return null;
        return { id: d.id, label: typeof d.label === 'string' ? d.label : '' };
    } catch (_e) {
        return null;
    }
}

/**
 * @param {{id: string, label: string}|null} device
 * @param {Pick<Storage, 'setItem'|'removeItem'>} [storage]
 */
export function saveMicDevice(device, storage) {
    try {
        const s = storage || globalThis.localStorage;
        if (device) s.setItem(DEVICE_KEY, JSON.stringify({ id: device.id, label: device.label }));
        else s.removeItem(DEVICE_KEY);
    } catch (_e) {
        /* a blocked storage: the choice lasts until the page closes */
    }
}

/**
 * The microphones a viewer can name: enumerateDevices without the entries that
 * only stand for the system's choice (Chrome's `default` and `communications`),
 * and without the nameless ones a browser lists before the permission.
 *
 * @param {MediaDeviceInfo[]} devices
 * @returns {{id: string, label: string}[]}
 */
export function namedMicrophones(devices) {
    return (devices || [])
        .filter(
            (d) =>
                d.kind === 'audioinput' &&
                d.label &&
                d.deviceId &&
                d.deviceId !== 'default' &&
                d.deviceId !== 'communications',
        )
        .map((d) => ({ id: d.deviceId, label: d.label }));
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
 * @param {string} [deviceId] - that microphone and no other; none follows the system
 * @returns {MediaStreamConstraints}
 */
export function micConstraints(quality, deviceId) {
    const p = quality.processing;
    /** @type {MediaTrackConstraints} */
    const audio = {
        echoCancellation: p,
        noiseSuppression: p,
        autoGainControl: p,
        channelCount: 1,
        sampleRate: 48000,
    };
    if (deviceId) audio.deviceId = { exact: deviceId };
    return { audio, video: false };
}

/**
 * Muted or not, carried from a retiring view to the one replacing it, so a
 * quality switch neither opens a microphone the viewer cut nor cuts one in use.
 * A new stream starts cut.
 */
let carriedMute = true;

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
     * @param {{id: string, label: string}|null} [opts.device] - null follows the system
     * @param {(c: MediaStreamConstraints) => Promise<MediaStream>} [opts.getUserMedia]
     * @param {() => Promise<MediaDeviceInfo[]>} [opts.enumerateDevices]
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
        /** Where the next socket sender numbers from (see _closeTrack). */
        this._socketSequence = 0;
        this._device = opts.device || null;
        this._getUserMedia =
            opts.getUserMedia || ((c) => globalThis.navigator.mediaDevices.getUserMedia(c));
        this._enumerateDevices =
            opts.enumerateDevices || (() => globalThis.navigator.mediaDevices.enumerateDevices());
        // A standby takes over the live view's mute; a new stream starts cut.
        if (!this._held) carriedMute = true;
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
        /** @type {(() => void)|null} the picked microphone is gone: the system's is used */
        this.onDeviceMissing = null;
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
     * After setLocalDescription, and at every opening: the microphone on the
     * sender, unless the viewer has it cut (the permission prompt, if any, comes
     * with the opening). Never throws.
     *
     * @returns {Promise<boolean>} whether a track is on the sender
     */
    async attach() {
        if (this._closed || this._held || !this._pc) return false;
        if (this._muted) {
            this._setState('muted');
            return false;
        }
        const track = await this._acquire();
        const t = soundTransceiver(this._pc);
        if (!track || !t || this._track !== track) return false;
        try {
            if (t.sender.track !== track) await t.sender.replaceTrack(track);
            // Cut again while that ran.
            if (this._track !== track) return false;
            await this._applyBitrate(t.sender);
            this._setState('live');
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
            this._fail('error');
            return false;
        }
    }

    /**
     * The WebSocket transport's open socket: the microphone goes on it as
     * frames (MicSocketSender), unless the viewer has it cut. Also called at
     * every opening. Never throws.
     *
     * @param {(bytes: Uint8Array) => boolean} send
     * @returns {Promise<boolean>} whether it is sending
     */
    async attachSocket(send) {
        this._socketSend = send;
        if (this._closed || this._held || this._socket) return false;
        if (this._muted) {
            this._setState('muted');
            return false;
        }
        const track = await this._acquire();
        // A second click may have started the sender while the first waited.
        if (!track || this._track !== track || this._socket) return false;
        const sender = new this._SocketSender(track, send, this._quality, this._socketSequence);
        this._socket = sender;
        try {
            await sender.start();
            if (this._socket !== sender) return false;
            this._setState('live');
            return true;
        } catch (e) {
            console.warn('[Mic] could not send the microphone on the WebSocket:', e);
            if (this._socket === sender) this._fail('error');
            else sender.stop();
            return false;
        }
    }

    /** A standby view promoted to the live one: start sending. */
    release() {
        if (!this._held) return Promise.resolve(false);
        this._held = false;
        this._muted = carriedMute;
        return this._send();
    }

    /** The microphone on whichever transport this stream has. */
    _send() {
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

    /**
     * Cut: the track stops, so the browser's indicator goes out. Open: the
     * microphone comes back, a refusal being reported like the first one.
     *
     * @param {boolean} muted
     * @returns {Promise<boolean>} whether it is sending once this settles
     */
    setMuted(muted) {
        this._muted = muted === true;
        carriedMute = this._muted;
        if (this._closed || this._held) return Promise.resolve(false);
        if (this._muted) {
            this._closeTrack();
            this._setState('muted');
            return Promise.resolve(false);
        }
        // Back from a refusal, or a second one would go unreported.
        if (this.state === 'denied' || this.state === 'error') this._setState('muted');
        return this._send();
    }

    /** @returns {Promise<boolean>} whether it is sending once this settles */
    toggleMuted() {
        return this.setMuted(!this._muted);
    }

    /** The stream ended: the microphone is closed, its indicator goes out. */
    close() {
        this._closed = true;
        this._closeTrack();
        this._pc = null;
    }

    /** Nothing captured and nothing sent; the section stays negotiated. */
    _closeTrack() {
        if (this._socket) {
            // Numbered on from here: the host's single socket source would take
            // a sender starting again at 0 for late frames, and drop the voice.
            if (Number.isInteger(this._socket.sequence)) {
                this._socketSequence = this._socket.sequence;
            }
            this._socket.stop();
            this._socket = null;
        }
        const sender = soundTransceiver(this._pc)?.sender;
        if (sender && sender.track) {
            Promise.resolve(sender.replaceTrack(null)).catch((e) => {
                console.info('[Mic] the sender kept its stopped track:', e && e.message);
            });
        }
        if (this._track) {
            try {
                this._track.stop();
            } catch (_e) {
                /* already ended */
            }
        }
        this._track = null;
    }

    /** It could not be opened: back to cut, and said. */
    _fail(state) {
        this._muted = true;
        carriedMute = true;
        this._closeTrack();
        this._setState(state);
    }

    /**
     * The stream for the microphone asked for. An id does not last (a browser may
     * draw new ones, the device may be gone), so a stale one falls back on the
     * name, which only shows once a microphone is open: the system's is opened
     * first, and kept when nothing bears that name.
     */
    async _openStream() {
        const open = (id) => this._getUserMedia(micConstraints(this._quality, id));
        const wanted = this._device;
        if (!wanted) return open();
        try {
            return await open(wanted.id);
        } catch (e) {
            if (e?.name !== 'OverconstrainedError' && e?.name !== 'NotFoundError') throw e;
        }
        const stream = await open();
        const listed = await this._enumerateDevices().catch(() => []);
        const found = namedMicrophones(listed).find((d) => d.label === wanted.label);
        this._device = found || null;
        if (!found) {
            console.info('[Mic] "' + wanted.label + '" is gone: using the system microphone');
            if (this.onDeviceMissing) this.onDeviceMissing();
            return stream;
        }
        if (stream.getAudioTracks()[0]?.label === found.label) return stream;
        stream.getTracks().forEach((x) => x.stop());
        return open(found.id);
    }

    _acquire() {
        if (this._track && this._track.readyState !== 'ended') return Promise.resolve(this._track);
        if (this._acquiring) return this._acquiring;
        this._acquiring = this._openStream()
            .then((stream) => {
                const track = stream.getAudioTracks()[0] || null;
                // Cut, or the stream ended, while the prompt was up.
                if (this._closed || this._muted) {
                    stream.getTracks().forEach((x) => x.stop());
                    return null;
                }
                this._track = track;
                if (!track) this._fail('error');
                return track;
            })
            .catch((e) => {
                const denied = e && (e.name === 'NotAllowedError' || e.name === 'SecurityError');
                console.warn('[Mic] getUserMedia refused:', e && e.name, e && e.message);
                this._fail(denied ? 'denied' : 'error');
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
