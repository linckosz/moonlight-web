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
 * What this browser can do, printed once as `[Caps]` lines, and the page's
 * life (`[Life]`) from then on — only in debug mode, where util/ClientLog.js
 * carries the console to the host's "Download logs" archive.
 *
 * Written for browsers nobody here can sit in front of: the Tesla's (issue
 * #48), a TV's, a friend's. One debug session from such a browser should say
 * which decoders, presenters, audio and input paths it has, without a round
 * trip per question.
 *
 * Nothing that names a device or a person: no GPU string, no device model,
 * no gamepad name, no language — the kind of a thing, never which one. The
 * user agent is already in every batch.
 *
 * Asks, never uses: no decoder is configured, no AudioContext is made (iOS
 * counts them, and a suspended one says nothing about autoplay), the one
 * WebGL context is lost again at once. Every probe has its own try and its
 * own deadline, so a browser that hangs on one still reports the others.
 */

/** A probe that has not answered by then is reported as such. */
const PROBE_MS = 3000;

/** The codecs Auto and the settings can ask the host for, as WebCodecs names them. */
const VIDEO_CODECS = [
    ['h264-baseline', 'avc1.42E01F'],
    ['h264-high', 'avc1.640033'],
    ['hevc-main', 'hvc1.1.6.L153.B0'],
    ['hevc-main10', 'hvc1.2.4.L153.B0'],
    ['av1-8bit', 'av01.0.08M.08'],
    ['av1-10bit', 'av01.0.08M.10'],
];

/** The sizes the host is likely to send. */
const VIDEO_SIZES = [
    [1920, 1080],
    [3840, 2160],
];

let printed = false;
let watching = false;

/**
 * @template T
 * @param {Promise<T>} p
 * @returns {Promise<T | 'timeout'>}
 */
function withDeadline(p, ms) {
    return Promise.race([p, new Promise((r) => setTimeout(() => r('timeout'), ms))]);
}

/**
 * The probe's answer, or what stopped it.
 * @param {() => any} fn
 * @param {number} [ms] a group of probes is given room for its own deadlines
 */
async function ask(fn, ms = PROBE_MS) {
    try {
        return await withDeadline(Promise.resolve().then(fn), ms);
    } catch (e) {
        return `error: ${(e && e.message) || e}`;
    }
}

function line(group, value) {
    let text;
    try {
        text = JSON.stringify(value);
    } catch {
        text = String(value);
    }
    console.log(`[Caps] ${group} ${text}`);
}

function mq(query) {
    try {
        return matchMedia(query).matches;
    } catch {
        return null;
    }
}

async function browser() {
    const out = {
        ua: navigator.userAgent,
        tesla: /\bTesla\//.test(navigator.userAgent || ''),
        cores: navigator.hardwareConcurrency ?? null,
        memoryGb: /** @type {any} */ (navigator).deviceMemory ?? null,
    };
    const uad = /** @type {any} */ (navigator).userAgentData;
    if (uad) {
        out.brands = (uad.brands || []).map((b) => `${b.brand}/${b.version}`);
        out.platform = uad.platform;
        out.mobile = uad.mobile;
        const high = await ask(() => uad.getHighEntropyValues(['fullVersionList', 'architecture']));
        if (high && typeof high === 'object') {
            out.arch = high.architecture;
            out.fullVersions = (high.fullVersionList || []).map((b) => `${b.brand}/${b.version}`);
        } else {
            out.highEntropy = high;
        }
    }
    return out;
}

function display() {
    return {
        screen: [screen.width, screen.height],
        avail: [screen.availWidth, screen.availHeight],
        window: [innerWidth, innerHeight],
        dpr: devicePixelRatio,
        colorDepth: screen.colorDepth,
        hdr: mq('(dynamic-range: high)'),
        p3: mq('(color-gamut: p3)'),
        orientation: (screen.orientation && screen.orientation.type) || null,
        standalone: mq('(display-mode: standalone)') || mq('(display-mode: fullscreen)'),
        fullscreenEnabled: !!(
            document.fullscreenEnabled || /** @type {any} */ (document).webkitFullscreenEnabled
        ),
    };
}

function page() {
    const conn = /** @type {any} */ (navigator).connection;
    return {
        secure: globalThis.isSecureContext ?? null,
        crossOriginIsolated:
            typeof crossOriginIsolated !== 'undefined' ? crossOriginIsolated : null,
        serviceWorker: 'serviceWorker' in navigator,
        swControlled: !!(navigator.serviceWorker && navigator.serviceWorker.controller),
        webSocket: typeof WebSocket !== 'undefined',
        rtcPeerConnection: typeof RTCPeerConnection !== 'undefined',
        rtcDataChannel: typeof RTCDataChannel !== 'undefined',
        encodedTransform:
            typeof RTCRtpScriptTransform !== 'undefined' ||
            (typeof RTCRtpSender !== 'undefined' &&
                'createEncodedStreams' in RTCRtpSender.prototype),
        workers: typeof Worker !== 'undefined',
        offscreenCanvas: typeof OffscreenCanvas !== 'undefined',
        connection: conn
            ? {
                  type: conn.type ?? null,
                  effectiveType: conn.effectiveType ?? null,
                  downlinkMbps: conn.downlink ?? null,
                  rttMs: conn.rtt ?? null,
                  saveData: conn.saveData ?? null,
              }
            : null,
        visibility: document.visibilityState,
    };
}

async function webcodecs() {
    const out = {
        videoDecoder: typeof VideoDecoder !== 'undefined',
        videoFrame: typeof VideoFrame !== 'undefined',
        audioDecoder: typeof AudioDecoder !== 'undefined',
        createImageBitmap: typeof createImageBitmap !== 'undefined',
    };
    if (out.videoDecoder) {
        // All at once: one by one, a slow browser would spend the group's
        // deadline on the first few.
        const asks = [];
        for (const [name, codec] of VIDEO_CODECS) {
            for (const [w, h] of VIDEO_SIZES) {
                for (const accel of ['prefer-hardware', 'no-preference']) {
                    const key = `${name}@${h}p${accel === 'prefer-hardware' ? '-hw' : ''}`;
                    const config = {
                        codec,
                        codedWidth: w,
                        codedHeight: h,
                        hardwareAcceleration: /** @type {any} */ (accel),
                        optimizeForLatency: true,
                    };
                    asks.push(
                        ask(() => VideoDecoder.isConfigSupported(config)).then((r) => [
                            key,
                            r && typeof r === 'object' ? !!r.supported : r,
                        ]),
                    );
                }
            }
        }
        out.video = Object.fromEntries(await Promise.all(asks));
    }
    if (out.audioDecoder) {
        const r = await ask(() =>
            AudioDecoder.isConfigSupported({
                codec: 'opus',
                sampleRate: 48000,
                numberOfChannels: 2,
            }),
        );
        out.opus = r && typeof r === 'object' ? !!r.supported : r;
    }
    return out;
}

function rtpCodecs() {
    if (typeof RTCRtpReceiver === 'undefined' || !RTCRtpReceiver.getCapabilities) return null;
    const kinds = {};
    for (const kind of ['video', 'audio']) {
        try {
            const caps = RTCRtpReceiver.getCapabilities(kind);
            kinds[kind] = caps
                ? [...new Set(caps.codecs.map((c) => c.mimeType.replace(/^(video|audio)\//, '')))]
                : null;
        } catch (e) {
            kinds[kind] = `error: ${e.message}`;
        }
    }
    return kinds;
}

function webgl() {
    let gl = null;
    try {
        const canvas = document.createElement('canvas');
        gl = canvas.getContext('webgl2');
        if (!gl) return { webgl2: false, webgl1: !!canvas.getContext('webgl') };
        return { webgl2: true, maxTexture: gl.getParameter(gl.MAX_TEXTURE_SIZE) };
    } catch (e) {
        return `error: ${e.message}`;
    } finally {
        try {
            gl?.getExtension('WEBGL_lose_context')?.loseContext();
        } catch {
            // Released with the canvas anyway.
        }
    }
}

async function webgpu() {
    const gpu = /** @type {any} */ (navigator).gpu;
    if (!gpu) return { webgpu: false };
    const adapter = await ask(() => gpu.requestAdapter());
    if (!adapter || typeof adapter !== 'object') return { webgpu: true, adapter: adapter ?? null };
    const info = adapter.info || {};
    return {
        webgpu: true,
        vendor: info.vendor || null,
        fallback: adapter.isFallbackAdapter ?? info.isFallbackAdapter ?? null,
        f16: adapter.features.has('shader-f16'),
        timestamps: adapter.features.has('timestamp-query'),
    };
}

function audio() {
    const nav = /** @type {any} */ (navigator);
    const policy = (kind) => {
        try {
            return nav.getAutoplayPolicy ? nav.getAutoplayPolicy(kind) : null;
        } catch {
            return null;
        }
    };
    return {
        audioContext: typeof AudioContext !== 'undefined' || 'webkitAudioContext' in window,
        audioWorklet: typeof AudioWorkletNode !== 'undefined',
        autoplayMedia: policy('mediaelement'),
        autoplayContext: policy('audiocontext'),
        userActivation: nav.userActivation
            ? { hasBeenActive: nav.userActivation.hasBeenActive }
            : null,
        mediaSession: 'mediaSession' in navigator,
        setSinkId:
            typeof HTMLMediaElement !== 'undefined' && 'setSinkId' in HTMLMediaElement.prototype,
        microphone: !!(navigator.mediaDevices && navigator.mediaDevices.getUserMedia),
    };
}

function input() {
    let pads = null;
    try {
        pads = navigator.getGamepads
            ? [...navigator.getGamepads()].filter(Boolean).map((p) => p.mapping || 'raw')
            : null;
    } catch (e) {
        pads = `error: ${e.message}`;
    }
    const nav = /** @type {any} */ (navigator);
    return {
        touchPoints: navigator.maxTouchPoints ?? null,
        pointerCoarse: mq('(pointer: coarse)'),
        pointerFine: mq('(any-pointer: fine)'),
        hover: mq('(any-hover: hover)'),
        pointerLock: 'requestPointerLock' in Element.prototype,
        keyboardLock: !!(nav.keyboard && nav.keyboard.lock),
        gamepadApi: typeof navigator.getGamepads === 'function',
        gamepads: pads,
        vibrate: typeof navigator.vibrate === 'function',
        wakeLock: 'wakeLock' in navigator,
        orientationLock: !!(screen.orientation && /** @type {any} */ (screen.orientation).lock),
        webHid: 'hid' in navigator,
        virtualKeyboard: 'virtualKeyboard' in navigator,
    };
}

/** The page's life from now on: what a browser that pauses or hides pages does to a stream. */
function watchLife() {
    if (watching) return;
    watching = true;
    const say = (what, extra = '') => console.log(`[Life] ${what}${extra ? ' ' + extra : ''}`);
    document.addEventListener('visibilitychange', () =>
        say('visibility', document.visibilityState),
    );
    document.addEventListener('freeze', () => say('freeze'));
    document.addEventListener('resume', () => say('resume'));
    addEventListener('pageshow', (e) => say('pageshow', e.persisted ? 'from bfcache' : ''));
    addEventListener('pagehide', (e) => say('pagehide', e.persisted ? 'to bfcache' : ''));
    addEventListener('online', () => say('online'));
    addEventListener('offline', () => say('offline'));
    addEventListener('gamepadconnected', (e) =>
        say('gamepad connected', e.gamepad.mapping || 'raw'),
    );
    addEventListener('gamepaddisconnected', () => say('gamepad disconnected'));
    document.addEventListener('fullscreenchange', () =>
        say('fullscreen', document.fullscreenElement ? 'on' : 'off'),
    );
    document.addEventListener('fullscreenerror', () => say('fullscreen refused'));
    document.addEventListener('pointerlockchange', () =>
        say('pointer lock', document.pointerLockElement ? 'on' : 'off'),
    );
    document.addEventListener('pointerlockerror', () => say('pointer lock refused'));
    screen.orientation?.addEventListener?.('change', () =>
        say('orientation', screen.orientation.type),
    );
    const conn = /** @type {any} */ (navigator).connection;
    conn?.addEventListener?.('change', () =>
        say(
            'network',
            `${conn.type ?? '?'} ${conn.effectiveType ?? '?'} ${conn.downlink ?? '?'} Mbit/s`,
        ),
    );
}

/**
 * Print what this browser can do, once per page, and follow its life. Called
 * by util/ClientLog.js when debug mode turns the mirror on.
 */
export async function logBrowserCaps() {
    if (printed) return;
    printed = true;
    watchLife();
    line('browser', await ask(browser, 2 * PROBE_MS));
    line('display', await ask(display));
    line('page', await ask(page));
    line('webcodecs', await ask(webcodecs, 3 * PROBE_MS));
    line('rtp', await ask(rtpCodecs));
    line('webgl', await ask(webgl));
    line('webgpu', await ask(webgpu, 2 * PROBE_MS));
    line('audio', await ask(audio));
    line('input', await ask(input));
}
