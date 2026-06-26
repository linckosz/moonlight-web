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
 * iOS audio-session unlock (singleton).
 *
 * On iOS, getting Web Audio out the BUILT-IN SPEAKER (not just Bluetooth) and
 * past the hardware Silent switch requires TWO things, BOTH done inside a user
 * gesture:
 *
 *   a) The AudioContext must be CREATED + resumed inside a gesture, with a tiny
 *      silent buffer played through it — the classic iOS Web Audio "unlock".
 *      A context created outside a gesture (we init audio only AFTER the launch
 *      network round-trip) never acquires the output route.
 *   b) A real looping HTMLMediaElement must be playing — this promotes the
 *      AVAudioSession from "ambient" (silenced by the Silent switch, volume =
 *      ringer) to "playback" (loudspeaker, ignores the switch, volume = media).
 *
 * The only live gesture we have is the launch CLICK, which fires BEFORE the
 * AudioPipeline exists (it awaits a network call first). So we create + unlock
 * the context here, in that gesture (prepareForLaunch), and AudioPipeline later
 * adopts it (adoptContext) instead of creating its own. The looping silent
 * element holds the playback category until release() at teardown.
 *
 * prime() (called at startup) pre-buffers the element and authorizes it on the
 * first interaction, so its launch .play() reliably starts.
 *
 * Off iOS every function is a no-op and adoptContext() returns null.
 */

import { IS_IOS } from '../util/BrowserDetect.js';

/** @type {HTMLAudioElement|null} */
let el = null;
/** @type {string|null} object URL for the silent WAV. */
let url = null;
/** @type {AudioContext|null} Context created in the launch gesture, awaiting adoption. */
let ctx = null;
/** True while we want the silent element to keep playing (between launch and release). */
let holding = false;
/** True once prime() has run. */
let primed = false;

/**
 * Build a short looping silent 16-bit PCM WAV and return an object URL.
 * Generated at runtime to avoid shipping a base64 blob and to guarantee a
 * valid header.
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

/** Play the silent element (promotes the session to "playback"). */
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

/**
 * Pre-buffer the unlock element and authorize it for scripted playback on the
 * first user interaction anywhere in the app. Call once at startup (iOS only).
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
 * Create + unlock the AudioContext and start the silent element, INSIDE the
 * launch-click gesture. AudioPipeline adopts the context shortly after. iOS only.
 * @param {number} [sampleRate=48000]
 */
export function prepareForLaunch(sampleRate = 48000) {
    if (!IS_IOS) return;
    ensureEl();
    playEl();

    if (ctx) return; // already prepared for this launch
    try {
        const Ctor = window.AudioContext || window.webkitAudioContext;
        if (!Ctor) return;
        ctx = new Ctor({ sampleRate });
        // Classic iOS Web Audio unlock: play a 1-sample silent buffer in-gesture.
        const buffer = ctx.createBuffer(1, 1, ctx.sampleRate);
        const src = ctx.createBufferSource();
        src.buffer = buffer;
        src.connect(ctx.destination);
        src.start(0);
        const r = ctx.resume();
        if (r && r.catch) r.catch(() => {});
    } catch (e) {
        ctx = null;
    }
}

/**
 * Hand the gesture-unlocked AudioContext to AudioPipeline (ownership transfers
 * to the caller). Returns null off iOS or if none was prepared.
 * @returns {AudioContext|null}
 */
export function adoptContext() {
    const c = ctx;
    ctx = null;
    return c;
}

/**
 * Promote the iOS audio session on an in-stream gesture (replays the silent
 * element). Does NOT create a context — that only happens at launch. No-op off iOS.
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

/** Stop holding the playback session (stream teardown). Keeps the primed element. */
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
