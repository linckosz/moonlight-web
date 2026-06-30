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
 * Mobile audio unlock + native-RTP-audio output (singleton).
 *
 * Mobile blocks <audio>.play() outside a user gesture, so the native RTP Opus
 * track (pc.ontrack) cannot start by itself. We bless a single persistent
 * <audio> element inside the launch CLICK gesture (the only live gesture, fired
 * before the audio element exists) and later swap its srcObject to the WebRTC
 * stream — the blessing carries over, so it plays without a tap.
 *
 * On iOS that same element also handles the Silent switch: an unmuted element
 * playing real audio promotes the AVAudioSession from "ambient" (silenced by the
 * switch, ringer volume) to "playback" (loudspeaker, media volume). For the WSS
 * path (no RTP track) the element stays a silent loop and an AudioContext,
 * created + unlocked in the same gesture, is adopted by AudioPipeline.
 *
 * No Web Audio gain stage: routing the remote stream through
 * createMediaStreamSource overloads WebKit's single thread and freezes the
 * WebCodecs video decode on iOS. Volume is the element's native 1.0.
 *
 * Off mobile every function is a no-op (playStream returns false → the caller
 * plays the stream on its own <audio> element); adoptContext returns null.
 */

import { IS_IOS, IS_ANDROID } from '../util/BrowserDetect.js';

// Element blessing (gesture-unlock for scripted playback) is needed on ALL
// mobile — Android blocks <audio>.play() outside a gesture too.
const IS_MOBILE = IS_IOS || IS_ANDROID;

/** @type {HTMLAudioElement|null} The blessed element: silent loop, then the stream. */
let el = null;
/** @type {string|null} object URL for the silent WAV. */
let url = null;
/** @type {AudioContext|null} Context created in the launch gesture, adopted by AudioPipeline (iOS/WSS). */
let ctx = null;
/** True while we want the element to keep playing (between launch and release). */
let holding = false;
/** True once prime() has run. */
let primed = false;

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

/** Lazily create the hidden looping silent <audio> element (mobile only). */
function ensureEl() {
    if (el || !IS_MOBILE) return;
    try {
        url = makeSilentWavUrl();
        const node = document.createElement('audio');
        node.loop = true;
        node.preload = 'auto';
        node.muted = false; // must NOT be muted, or the iOS session stays "ambient"
        node.volume = 1.0;
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

/** Play the element (blesses it / promotes the iOS session to "playback"). */
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

/** Create + unlock an AudioContext inside a gesture (silent-buffer trick). iOS/WSS. */
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
 * Pre-buffer the unlock element and authorize it for scripted playback on the
 * first user interaction anywhere in the app. Call once at startup (mobile).
 */
export function prime() {
    if (!IS_MOBILE || primed) return;
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
 * Inside the launch-click gesture: bless the element (so a later srcObject swap
 * plays without a tap) and, on iOS, unlock the AudioContext that AudioPipeline
 * (WSS path) adopts. No-op off mobile.
 * @param {number} [sampleRate=48000]
 */
export function prepareForLaunch(sampleRate = 48000) {
    if (!IS_MOBILE) return;
    ensureEl();
    playEl();
    if (IS_IOS && !ctx) ctx = makeUnlockedCtx(sampleRate); // adopted by AudioPipeline (WSS)
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

/** One-time gesture retry of the element if mobile autoplay rejected it. */
function armOutputRetry() {
    const events = ['pointerdown', 'touchend', 'mousedown', 'click'];
    const onGesture = () => {
        for (const ev of events) window.removeEventListener(ev, onGesture, true);
        playEl();
    };
    for (const ev of events) window.addEventListener(ev, onGesture, true);
}

/**
 * Play a native-RTP-audio MediaStream through the gesture-blessed element at
 * native volume (1.0). Mobile only; desktop returns false so the caller plays
 * the stream on its own <audio> element (no autoplay restriction there).
 * @param {MediaStream} stream
 * @returns {boolean} true if routed (mobile + element ready); false otherwise.
 */
export function playStream(stream) {
    if (!IS_MOBILE) return false;
    ensureEl();
    if (!el) return false;
    holding = true;
    try {
        el.loop = false;
        el.muted = false;
        el.volume = 1.0;
        el.srcObject = stream;
        const p = el.play();
        if (p && p.catch) p.catch(() => armOutputRetry());
    } catch (e) {
        return false;
    }
    return true;
}

/**
 * Promote the iOS audio session on an in-stream gesture (replays the element).
 * Used by the WSS AudioPipeline path. No-op off iOS.
 */
export function unlock() {
    if (!IS_IOS) return;
    ensureEl();
    playEl();
}

/** True once the element is actually playing (mobile). */
export function isActive() {
    return !!(el && !el.paused);
}

/** Stop holding the session / playback (stream teardown). Keeps the element. */
export function release() {
    holding = false;
    if (el) {
        try {
            el.pause();
            // If el carried the WebRTC stream, restore the silent looping source
            // so the next launch can re-bless this same element.
            if (el.srcObject) {
                el.srcObject = null;
                if (url) {
                    el.src = url;
                    el.loop = true;
                    el.load();
                }
            }
            el.currentTime = 0;
        } catch (e) {
            /* ignore */
        }
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
