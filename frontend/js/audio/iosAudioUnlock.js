/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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
 * iOS/mobile audio unlock + native-RTP-audio output (singleton).
 *
 * Two responsibilities:
 *
 * 1. iOS session unlock (Silent switch / loudspeaker). Getting Web Audio out the
 *    BUILT-IN SPEAKER past the hardware Silent switch requires, inside a user
 *    gesture: (a) an AudioContext created + resumed with a tiny silent buffer,
 *    and (b) a real looping unmuted HTMLMediaElement playing, which promotes the
 *    AVAudioSession from "ambient" to "playback". The only live gesture is the
 *    launch CLICK (before AudioPipeline exists), so we do both here in
 *    prepareForLaunch; AudioPipeline later adopts the context (adoptContext).
 *
 * 2. Native RTP audio output with a volume boost (playStream). The browser
 *    decodes the incoming Opus track, but an <audio> element caps at volume 1.0.
 *    To go louder WITHOUT quality loss we route the MediaStream through a Web
 *    Audio GainNode (AudioContext -> MediaStreamSource -> GainNode -> output).
 *    A muted <audio> element keeps the remote decoder pumping (some engines
 *    yield silence from createMediaStreamSource on a remote stream unless it is
 *    also attached to a media element) and is the audible fallback.
 *
 * Off mobile, the unlock functions are no-ops; playStream still applies the gain
 * (desktop has no autoplay-after-gesture issue here — the launch click counts).
 */

import { IS_IOS, IS_ANDROID } from '../util/BrowserDetect.js';

const IS_MOBILE = IS_IOS || IS_ANDROID;

// Output gain for the native RTP audio (Web Audio GainNode). >1.0 lifts the
// volume above the <audio> element's 1.0 ceiling; lossless (sample multiply).
// Kept modest to avoid clipping on already-hot game audio.
const AUDIO_GAIN = 1.5;

// ── iOS session-holder element (silent unmuted loop) ───────────────────────
/** @type {HTMLAudioElement|null} */
let el = null;
/** @type {string|null} object URL for the silent WAV. */
let url = null;
/** @type {AudioContext|null} Context created in the launch gesture, awaiting adoption by AudioPipeline (WSS). */
let ctx = null;
/** True while we want the silent element to keep playing (between launch and release). */
let holding = false;
/** True once prime() has run. */
let primed = false;

// ── Native RTP audio gain stage ────────────────────────────────────────────
/** @type {AudioContext|null} Drives the gain stage (unlocked in-gesture on mobile). */
let outCtx = null;
/** @type {MediaStreamAudioSourceNode|null} */
let srcNode = null;
/** @type {GainNode|null} */
let gainNode = null;
/** @type {HTMLAudioElement|null} Muted element pumping the remote decoder + fallback. */
let sinkEl = null;

/**
 * Build a short looping silent 16-bit PCM WAV and return an object URL.
 * @returns {string} object URL for a silent WAV.
 */
function makeSilentWavUrl() {
    const sr = 8000; // tiny — content is silence, rate is irrelevant
    const numSamples = sr >> 1; // 0.5 s
    const blockAlign = 2; // mono, 16-bit
    const dataSize = numSamples * blockAlign;
    const buf = new ArrayBuffer(44 + dataSize);
    const view = new DataView(buf);
    const str = (off, s) => {
        for (let i = 0; i < s.length; i++) view.setUint8(off + i, s.charCodeAt(i));
    };
    str(0, 'RIFF');
    view.setUint32(4, 36 + dataSize, true);
    str(8, 'WAVE');
    str(12, 'fmt ');
    view.setUint32(16, 16, true); // PCM fmt chunk size
    view.setUint16(20, 1, true); // PCM
    view.setUint16(22, 1, true); // mono
    view.setUint32(24, sr, true);
    view.setUint32(28, sr * blockAlign, true); // byte rate
    view.setUint16(32, blockAlign, true);
    view.setUint16(34, 16, true); // bits per sample
    str(36, 'data');
    view.setUint32(40, dataSize, true);
    // PCM samples are already zero-filled (silence).
    return URL.createObjectURL(new Blob([buf], { type: 'audio/wav' }));
}

/** Lazily create the hidden looping silent <audio> element (iOS only). */
function ensureEl() {
    if (el || !IS_IOS) return;
    try {
        url = makeSilentWavUrl();
        const node = document.createElement('audio');
        node.loop = true;
        node.preload = 'auto';
        node.muted = false; // must NOT be muted, or the session stays "ambient"
        node.volume = 1.0; // content is pure silence — volume is irrelevant
        node.setAttribute('playsinline', '');
        node.setAttribute('webkit-playsinline', '');
        node.src = url;
        node.style.display = 'none';
        if (document.body) document.body.appendChild(node);
        node.load();
        el = node;
    } catch (e) {
        el = null;
    }
}

/** Play the silent element (promotes the iOS session to "playback"). */
function playEl() {
    if (!el) return;
    holding = true;
    try {
        const p = el.play();
        if (p && p.catch) p.catch(() => {});
    } catch (e) {
        /* ignore */
    }
}

/** Lazily create the hidden MUTED element that pumps the remote decoder. */
function ensureSinkEl() {
    if (sinkEl) return;
    try {
        const node = document.createElement('audio');
        node.muted = true; // audible output goes through the GainNode, not here
        node.autoplay = true;
        node.setAttribute('playsinline', '');
        node.setAttribute('webkit-playsinline', '');
        node.style.display = 'none';
        if (document.body) document.body.appendChild(node);
        sinkEl = node;
    } catch (e) {
        sinkEl = null;
    }
}

/** Create + unlock an AudioContext inside a gesture (silent-buffer trick). */
function makeUnlockedCtx(sampleRate) {
    try {
        const Ctor = window.AudioContext || window.webkitAudioContext;
        if (!Ctor) return null;
        const c = new Ctor({ sampleRate });
        const b = c.createBuffer(1, 1, c.sampleRate);
        const s = c.createBufferSource();
        s.buffer = b;
        s.connect(c.destination);
        s.start(0);
        const r = c.resume();
        if (r && r.catch) r.catch(() => {});
        return c;
    } catch (e) {
        return null;
    }
}

/**
 * Pre-buffer the iOS unlock element and authorize it on the first user
 * interaction anywhere in the app. Call once at startup. iOS only.
 */
export function prime() {
    if (!IS_IOS || primed) return;
    primed = true;
    ensureEl();
    if (!el) return;

    const events = ['pointerdown', 'touchstart', 'mousedown', 'keydown', 'click'];
    const onFirst = () => {
        cleanup();
        if (!el) return;
        try {
            const p = el.play();
            if (p && p.then) {
                p.then(() => {
                    if (!holding && el) el.pause(); // authorize-only, stay silent
                }).catch(() => {});
            }
        } catch (e) {
            /* ignore */
        }
    };
    const cleanup = () => {
        for (const ev of events) window.removeEventListener(ev, onFirst, true);
    };
    for (const ev of events) window.addEventListener(ev, onFirst, true);
}

/**
 * Inside the launch-click gesture: hold the iOS session and unlock the audio
 * contexts so playback (native gain stage / WSS AudioPipeline) can start later
 * without a tap. iOS does both; Android only unlocks the gain-stage context.
 * @param {number} [sampleRate=48000]
 */
export function prepareForLaunch(sampleRate = 48000) {
    if (!IS_MOBILE) return;
    if (IS_IOS) {
        ensureEl();
        playEl(); // silent unmuted loop -> "playback" session
        if (!ctx) ctx = makeUnlockedCtx(sampleRate); // adopted by AudioPipeline (WSS)
    }
    // All mobile: unlock the context that drives the native RTP gain stage.
    if (!outCtx) outCtx = makeUnlockedCtx(sampleRate);
}

/**
 * Hand the gesture-unlocked AudioContext to AudioPipeline (WSS path, iOS).
 * Returns null off iOS or if none was prepared.
 * @returns {AudioContext|null}
 */
export function adoptContext() {
    const c = ctx;
    ctx = null;
    return c;
}

/** One-time gesture retry of the native audio output (autoplay rejected). */
function armOutputRetry() {
    const events = ['pointerdown', 'touchend', 'mousedown', 'click'];
    const onGesture = () => {
        for (const ev of events) window.removeEventListener(ev, onGesture, true);
        try {
            if (outCtx) {
                const r = outCtx.resume();
                if (r && r.catch) r.catch(() => {});
            }
        } catch (e) {
            /* ignore */
        }
        try {
            if (sinkEl) {
                const p = sinkEl.play();
                if (p && p.catch) p.catch(() => {});
            }
        } catch (e) {
            /* ignore */
        }
        if (IS_IOS) playEl();
    };
    for (const ev of events) window.addEventListener(ev, onGesture, true);
}

/**
 * Output a WebRTC (native RTP Opus) MediaStream with a volume boost. Routes the
 * stream through a Web Audio GainNode (>1.0, lossless) and pumps the remote
 * decoder with a muted element. Falls back to the unmuted element at 1.0 if the
 * gain stage cannot be built.
 * @param {MediaStream} stream
 * @param {number} [gain=AUDIO_GAIN]
 * @returns {boolean} always true (the stream is routed somewhere).
 */
export function playStream(stream, gain = AUDIO_GAIN) {
    ensureSinkEl();

    // Web Audio gain stage — lets us exceed the element's 1.0 volume ceiling.
    let boosted = false;
    try {
        const Ctor = window.AudioContext || window.webkitAudioContext;
        if (Ctor) {
            if (!outCtx) outCtx = new Ctor(); // desktop: no in-gesture unlock needed
            if (srcNode) {
                try {
                    srcNode.disconnect();
                } catch (e) {
                    /* ignore */
                }
            }
            if (gainNode) {
                try {
                    gainNode.disconnect();
                } catch (e) {
                    /* ignore */
                }
            }
            srcNode = outCtx.createMediaStreamSource(stream);
            gainNode = outCtx.createGain();
            gainNode.gain.value = gain;
            srcNode.connect(gainNode);
            gainNode.connect(outCtx.destination);
            const r = outCtx.resume();
            if (r && r.catch) r.catch(() => armOutputRetry());
            boosted = true;
        }
    } catch (e) {
        boosted = false;
    }

    try {
        sinkEl.srcObject = stream;
        sinkEl.muted = boosted; // gain path is audible -> keep the element silent
        sinkEl.volume = 1.0;
        const p = sinkEl.play();
        if (p && p.catch) p.catch(() => armOutputRetry());
    } catch (e) {
        /* ignore */
    }

    // iOS: keep the silent unmuted loop holding the "playback" session.
    if (IS_IOS) {
        ensureEl();
        playEl();
    }
    return true;
}

/**
 * Promote the iOS audio session on an in-stream gesture (replays the silent
 * element). Used by the WSS AudioPipeline path. No-op off iOS.
 */
export function unlock() {
    if (!IS_IOS) return;
    ensureEl();
    playEl();
}

/** True once the silent element is actually playing (iOS). */
export function isActive() {
    return !!(el && !el.paused);
}

/** Stop holding the session + tear down the native gain stage (stream teardown). */
export function release() {
    holding = false;
    if (el) {
        try {
            el.pause();
            el.currentTime = 0;
        } catch (e) {
            /* ignore */
        }
    }
    // Native RTP gain stage teardown.
    if (srcNode) {
        try {
            srcNode.disconnect();
        } catch (e) {
            /* ignore */
        }
        srcNode = null;
    }
    if (gainNode) {
        try {
            gainNode.disconnect();
        } catch (e) {
            /* ignore */
        }
        gainNode = null;
    }
    if (sinkEl) {
        try {
            sinkEl.pause();
            sinkEl.srcObject = null;
        } catch (e) {
            /* ignore */
        }
    }
    if (outCtx) {
        try {
            outCtx.close();
        } catch (e) {
            /* ignore */
        }
        outCtx = null;
    }
    // Close a context that was prepared but never adopted (e.g. launch failed).
    if (ctx) {
        try {
            ctx.close();
        } catch (e) {
            /* ignore */
        }
        ctx = null;
    }
}
