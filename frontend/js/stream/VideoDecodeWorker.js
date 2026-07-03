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
 * VideoDecodeWorker — OffscreenCanvas decode + render worker (opt-in).
 *
 * Runs the WebCodecs VideoDecoder and canvas rendering off the main UI thread.
 * The main thread keeps frame ingest, stale/gap detection, stats and the overlay
 * (StreamView.handleVideoFrame); it forwards each accepted frame here and we do:
 * NAL parse → decoder configure → decode → render to the transferred OffscreenCanvas.
 *
 * Workers have no requestAnimationFrame, so rendering is driven by decoder output
 * (render-on-decode with a "render in flight" guard + drop-to-latest), which keeps
 * latency low and serializes GPU access (needed by the Chrome Windows HEVC path).
 *
 * This is a faithful port of StreamView's decode/render pipeline; the codec/NAL
 * helpers are imported from the same shared modules, so there is no divergence in
 * codec handling. Render quirks (green tint, stride) are preserved and must be
 * re-validated per browser/codec when this path is enabled.
 *
 * Messages in:  init | frame | frameloss | stop
 * Messages out: requestidr | codecfallback | firstframe | status | counters | fatal
 */
import {
    NalParser,
    buildDescription,
    getCodecString,
    toAvcc,
    H264_FALLBACK_CODEC_STRINGS,
    HEVC_FALLBACK_CODEC_STRINGS,
    HEVC_ANNEXB_CODEC_STRINGS,
    CODEC_HEVC,
    isHevcHdrProfile,
} from '../util/Mp4Muxer.js';
import {
    findSequenceHeader,
    buildAv1DecoderConfigs,
    stripNonEssentialObus,
    isAv1HdrProfile,
    CODEC_AV1,
} from '../util/Av1Utils.js';
import { createVideoRenderer } from './renderers/createRenderer.js';

// ── Worker-local pipeline state (mirrors the StreamView fields) ──────────────
const S = {
    canvas: null,
    renderer: null,
    outW: 0,
    outH: 0,
    videoCodec: 'h264',
    isChromeWindowsHevc: false,
    transport: 'webrtc',

    decoder: null,
    decoderConfigured: false,
    decoderConfiguring: false,
    nalParser: null,

    pendingFrames: [],
    frameQueue: [],
    rendering: false,

    _noDescription: false,
    _referenceValid: true,
    _recoveryAttempts: 0,
    _decoderRecovering: false,
    _fatalDecodeError: false,
    _codecFallbackRequested: false,
    _proactiveIdrScheduled: false,
    _firstFrameReported: false,
    _firstDecoderOutputLogged: false,
    _lastResolution: '',

    frameCount: 0,
    _lastChunkTs: 0,
    _queueStallStart: 0,
    _idrRequested: false,
    _lastIdrRequestMs: 0,

    // latency: worker-local steady→perf offset (perf.now origin differs from main,
    // but the delta is self-consistent within the worker)
    _steadyToPerfOffset: null,
    _offsetFromStats: false,

    stopped: false,

    // tunables (match StreamView)
    DECODE_QUEUE_MAX: 8,
    QUEUE_STALL_MS: 200,
    QUEUE_RESET_MS: 1000,
    MAX_RECOVERY_ATTEMPTS: 10,

    stats: { received: 0, decoded: 0, rendered: 0, dropped: 0 },
    _lastCountersPost: 0,
    _lastLatencyMs: 0,
};

function post(msg, transfer) {
    self.postMessage(msg, transfer || []);
}

function setStatus(state, msg) {
    post({ type: 'status', state, msg });
}

// Level-triggered IDR request with a 1s throttle (mirrors StreamView._requestIdr).
function requestIdr(reason) {
    const now = performance.now();
    if (S._idrRequested && now - S._lastIdrRequestMs < 1000) return;
    S._idrRequested = true;
    S._lastIdrRequestMs = now;
    post({ type: 'requestidr', reason });
}

function postCounters(force) {
    const now = performance.now();
    if (!force && now - S._lastCountersPost < 500) return;
    S._lastCountersPost = now;
    post({
        type: 'counters',
        received: S.stats.received,
        decoded: S.stats.decoded,
        rendered: S.stats.rendered,
        dropped: S.stats.dropped,
        latencyMs: S._lastLatencyMs,
        resolution: S._lastResolution || '',
    });
}

// ── Decoder lifecycle ────────────────────────────────────────────────────────

function setupDecoder() {
    if (S.decoder) {
        try {
            S.decoder.close();
        } catch (e) {}
        S.decoder = null;
    }
    S.decoderConfigured = false;
    S.decoderConfiguring = false;

    S.decoder = new VideoDecoder({
        output: (frame) => {
            if (!S._firstDecoderOutputLogged) {
                S._firstDecoderOutputLogged = true;
                console.log(
                    '[VideoWorker] First decoded frame: ' +
                        (frame.displayWidth || frame.codedWidth) +
                        'x' +
                        (frame.displayHeight || frame.codedHeight) +
                        ' format=' +
                        (frame.format || 'null'),
                );
            }
            onDecodedFrame(frame);
        },
        error: (err) => {
            console.error('[VideoWorker] VideoDecoder error:', err.message);
            handleDecoderError(err);
        },
    });
}

function handleDecoderError(err) {
    if (S.stopped) return;
    if (S._decoderRecovering) return;
    S._recoveryAttempts++;

    // AV1 that never produced a frame → broken AV1 decode in this browser.
    if (S.videoCodec === CODEC_AV1 && S.stats.decoded === 0 && S._recoveryAttempts >= 3) {
        S._fatalDecodeError = true;
        if (S.decoder) {
            try {
                S.decoder.close();
            } catch (e) {}
            S.decoder = null;
        }
        S.decoderConfigured = false;
        S.pendingFrames = [];
        post({
            type: 'fatal',
            msg: 'AV1 decoding failed in this browser — select H.264 or HEVC in Settings',
        });
        return;
    }
    if (S._recoveryAttempts > S.MAX_RECOVERY_ATTEMPTS) {
        setStatus('error', 'Max recovery attempts exceeded');
        return;
    }
    S._decoderRecovering = true;

    if (S.decoder) {
        try {
            S.decoder.close();
        } catch (e) {}
        S.decoder = null;
    }
    S.decoderConfigured = false;
    S.decoderConfiguring = false;
    S._referenceValid = false;

    for (const frame of S.frameQueue) {
        try {
            frame.close();
        } catch (e) {}
    }
    S.frameQueue = [];
    S.pendingFrames = [];
    S.nalParser.reset();

    setupDecoder();
    requestIdr('decoder error');
    setStatus('connecting', 'Recovering...');
    S._decoderRecovering = false;
}

// ── H.264 / HEVC decoder configuration (ported from configureDecoder) ─────────

function configureDecoder() {
    if (S.stopped) return;
    if (S.decoderConfigured || S.decoderConfiguring || !S.nalParser.isReady()) return;
    S.decoderConfiguring = true;
    const codecType = S.nalParser.codec;

    const desc = buildDescription(S.nalParser);
    if (!desc) {
        S.decoderConfiguring = false;
        return;
    }

    const codec = getCodecString(S.nalParser);
    if (!codec) {
        S.decoderConfiguring = false;
        setStatus('error', 'Unknown codec');
        return;
    }

    const applyConfig = (cfg, noDescription = false) => {
        const _doConfigure = (config, hwAccel) => {
            const cfgToUse = hwAccel
                ? { ...config, hardwareAcceleration: 'prefer-hardware' }
                : config;
            try {
                S.decoder.configure(cfgToUse);
                S.decoderConfigured = true;
                S.decoderConfiguring = false;
                S._noDescription = noDescription;
                console.log(
                    '[VideoWorker] Decoder CONFIGURED codec=' +
                        cfgToUse.codec +
                        ' noDescription=' +
                        noDescription +
                        ' hw=' +
                        (cfgToUse.hardwareAcceleration || 'none'),
                );
                flushPendingFrames();
                if (!S._proactiveIdrScheduled) {
                    S._proactiveIdrScheduled = true;
                    setTimeout(() => {
                        if (!S.stopped) post({ type: 'requestidr', reason: 'proactive' });
                    }, 250);
                }
                return true;
            } catch (e) {
                S.decoderConfiguring = false;
                if (hwAccel && e.name === 'NotSupportedError') throw e;
                return false;
            }
        };
        try {
            return _doConfigure(cfg, true);
        } catch (hwErr) {
            return _doConfigure(cfg, false);
        }
    };

    const tryCodecs = (configs, index, onExhausted) => {
        if (index >= configs.length) {
            onExhausted();
            return;
        }
        const cfg = configs[index];
        const noDescription = cfg._noDescription === true;
        VideoDecoder.isConfigSupported(cfg)
            .then((result) => {
                if (result.supported) {
                    if (!applyConfig(cfg, noDescription))
                        tryCodecs(configs, index + 1, onExhausted);
                } else {
                    tryCodecs(configs, index + 1, onExhausted);
                }
            })
            .catch(() => tryCodecs(configs, index + 1, onExhausted));
    };

    // Detect HDR from the codec string (HEVC Main10 / AV1 10-bit).
    const isHdr = isHevcHdrProfile(codec) || isAv1HdrProfile(codec);

    const shared = { codedWidth: 1920, codedHeight: 1080, optimizeForLatency: true };
    // Decoder color space: HDR (BT.2020 + PQ) or SDR (BT.709).
    const vColor = isHdr
        ? {
              colorSpace: {
                  primaries: 'bt2020',
                  transfer: 'pq',
                  matrix: 'bt2020-ncl',
                  fullRange: false,
              },
          }
        : {
              colorSpace: {
                  primaries: 'bt709',
                  transfer: 'bt709',
                  matrix: 'bt709',
                  fullRange: false,
              },
          };

    const fallbacks =
        codecType === CODEC_HEVC ? HEVC_FALLBACK_CODEC_STRINGS : H264_FALLBACK_CODEC_STRINGS;
    const colorConfig = { codec, description: desc.buffer, ...shared, ...vColor };

    // HEVC: Annex B (no description) first — Chromium keyframe validator only
    // parses start codes; AVCC-with-description comes after.
    if (codecType === CODEC_HEVC) {
        const annexBCfgs = [];
        const hev1Primary = codec.replace(/^hvc1/, 'hev1');
        annexBCfgs.push({ codec: hev1Primary, ...shared, ...vColor, _noDescription: true });
        annexBCfgs.push({ codec: hev1Primary, ...shared, _noDescription: true });
        for (const fb of HEVC_ANNEXB_CODEC_STRINGS) {
            if (fb === hev1Primary) continue;
            annexBCfgs.push({ codec: fb, ...shared, ...vColor, _noDescription: true });
            annexBCfgs.push({ codec: fb, ...shared, _noDescription: true });
        }
        tryCodecs(annexBCfgs, 0, () => tryHevcAvccConfigs(codec, desc, fallbacks, shared, vColor));
        return;
    }

    // H.264: AVCC with description + fallback variants
    const configsToTry = [colorConfig];
    for (const fbCodec of fallbacks) {
        if (fbCodec === codec) continue;
        configsToTry.push({
            codec: fbCodec,
            description: desc.buffer,
            codedWidth: 1920,
            codedHeight: 1080,
            optimizeForLatency: true,
            ...vColor,
        });
        configsToTry.push({
            codec: fbCodec,
            description: desc.buffer,
            codedWidth: 1920,
            codedHeight: 1080,
            optimizeForLatency: true,
        });
    }
    configsToTry.push({ codec, description: desc.buffer, codedWidth: 1920, codedHeight: 1080 });
    configsToTry.push({ codec, description: desc.buffer });
    configsToTry.push({
        codec,
        description: desc,
        codedWidth: 1920,
        codedHeight: 1080,
        optimizeForLatency: true,
    });
    configsToTry.push({ codec, optimizeForLatency: true });
    configsToTry.push({ codec });

    tryCodecs(configsToTry, 0, () => {
        const avc3Configs = [
            { codec: 'avc3.42E01E', ...shared, ...vColor, optimizeForLatency: true },
            { codec: 'avc3.42E01E', ...shared, optimizeForLatency: true },
            { codec: 'avc3.42E01E', ...shared },
        ];
        tryCodecs(avc3Configs, 0, () => {
            S.decoderConfiguring = false;
            setStatus('error', 'Codec not supported by browser');
        });
    });
}

function tryHevcAvccConfigs(codec, desc, fallbacks, shared, vColor) {
    const cfgs = [];
    cfgs.push({ codec, description: desc.buffer, ...shared, ...vColor });
    cfgs.push({ codec, description: desc.buffer, ...shared });
    for (const fb of fallbacks) {
        if (fb === codec) continue;
        cfgs.push({ codec: fb, description: desc.buffer, ...shared, ...vColor });
        cfgs.push({ codec: fb, description: desc.buffer, ...shared });
    }
    cfgs.push({ codec, description: desc, ...shared });
    cfgs.push({ codec, description: desc.buffer, optimizeForLatency: true });
    cfgs.push({ codec, description: desc.buffer, codedWidth: 1920, codedHeight: 1080 });
    cfgs.push({ codec, description: desc.buffer });
    cfgs.push({ codec, ...shared });
    cfgs.push({ codec, optimizeForLatency: true });

    const tryNext = (idx) => {
        if (idx >= cfgs.length) {
            S.decoderConfiguring = false;
            handleHevcFallback();
            return;
        }
        const cfg = cfgs[idx];
        VideoDecoder.isConfigSupported(cfg)
            .then((r) => {
                if (r.supported) {
                    try {
                        S.decoder.configure(cfg);
                        S.decoderConfigured = true;
                        S.decoderConfiguring = false;
                        S._noDescription = false;
                        flushPendingFrames();
                    } catch (e) {
                        S.decoderConfiguring = false;
                        handleHevcFallback();
                    }
                } else {
                    tryNext(idx + 1);
                }
            })
            .catch(() => tryNext(idx + 1));
    };
    tryNext(0);
}

function handleHevcFallback() {
    S.decoderConfiguring = false;
    S._codecFallbackRequested = true;
    S.stopped = true; // stop processing; main will quit + relaunch with H.264
    post({ type: 'codecfallback', from: 'hevc', to: 'h264' });
}

function flushPendingFrames() {
    // After configure, a keyframe MUST be fed first (delta-first → green output).
    if (S.pendingFrames.length > 1 && !S.pendingFrames[0].isKeyframe) {
        const keyIdx = S.pendingFrames.findIndex((e) => e.isKeyframe);
        if (keyIdx > 0) {
            const [keyframe] = S.pendingFrames.splice(keyIdx, 1);
            S.pendingFrames.unshift(keyframe);
        }
    }
    while (S.pendingFrames.length > 0) {
        const entry = S.pendingFrames.shift();
        decodeFrame(entry.data, entry.isKeyframe, entry.backendTs);
    }
}

function decodeFrame(data, isKeyframe, backendTs) {
    if (!S.decoderConfigured) {
        if (S.pendingFrames.length < 120) S.pendingFrames.push({ data, isKeyframe, backendTs });
        return;
    }
    if (!S.decoder) return;
    if (S.decoder.state === 'closed') {
        handleDecoderError(new Error('Decoder state is closed'));
        return;
    }

    if (!isKeyframe && !S._referenceValid) {
        S.stats.dropped++;
        requestIdr('reference invalid');
        return;
    }

    if (!isKeyframe && S.decoder.decodeQueueSize >= S.DECODE_QUEUE_MAX) {
        const now = performance.now();
        if (S._queueStallStart === 0) S._queueStallStart = now;
        const saturatedMs = now - S._queueStallStart;
        if (saturatedMs > S.QUEUE_RESET_MS) {
            S._queueStallStart = 0;
            handleDecoderError(new Error('Decode queue stalled'));
            return;
        }
        if (saturatedMs > S.QUEUE_STALL_MS) {
            S.stats.dropped++;
            S._referenceValid = false;
            requestIdr('decode queue overflow');
            return;
        }
    } else {
        S._queueStallStart = 0;
    }

    let timestamp =
        backendTs !== undefined && backendTs > 0 ? backendTs * 1000 : S.frameCount * 16667;
    if (timestamp <= S._lastChunkTs) timestamp = S._lastChunkTs + 1;
    S._lastChunkTs = timestamp;
    S.frameCount++;

    const type = isKeyframe ? 'key' : 'delta';
    const useAnnexB = S._noDescription && S.nalParser.codec === CODEC_HEVC;
    const avccData = toAvcc(data, S.decoderConfigured, S.nalParser.codec, useAnnexB);

    try {
        const chunk = new EncodedVideoChunk({ type, timestamp, duration: 16667, data: avccData });
        S.decoder.decode(chunk);
        S.stats.received++;
        if (isKeyframe) {
            S._referenceValid = true;
            S._idrRequested = false;
            S._queueStallStart = 0;
        }
    } catch (err) {
        S.stats.dropped++;
        handleDecoderError(err);
    }
}

// ── AV1 pipeline ─────────────────────────────────────────────────────────────

function handleAv1Frame(data, isKeyframe) {
    if (!S.decoderConfigured && !S.decoderConfiguring) {
        if (isKeyframe) {
            if (S.pendingFrames.length > 0) S.pendingFrames = [];
            S._idrRequested = false;
            const seqHeader = findSequenceHeader(data);
            configureAv1Decoder(seqHeader || undefined);
        } else {
            if (S.pendingFrames.length < 120) S.pendingFrames.push({ data, isKeyframe });
            if (S.pendingFrames.length > 30 && S.pendingFrames.length % 30 === 0) {
                requestIdr('AV1 no keyframe while buffering');
            }
            return;
        }
    }
    decodeAv1Frame(data, isKeyframe);
}

function configureAv1Decoder(seqHeaderObu) {
    if (S.decoderConfigured || S.decoderConfiguring) return;
    if (!S.decoder) setupDecoder();
    S.decoderConfiguring = true;

    const configs = buildAv1DecoderConfigs(seqHeaderObu || null);
    const tryCodecs = (index) => {
        if (index >= configs.length) {
            S.decoderConfiguring = false;
            setStatus('error', 'AV1 codec not supported by browser');
            return;
        }
        const cfg = configs[index];
        VideoDecoder.isConfigSupported(cfg)
            .then((result) => {
                if (result.supported) {
                    try {
                        S.decoder.configure(cfg);
                        S.decoderConfigured = true;
                        S.decoderConfiguring = false;
                        while (S.pendingFrames.length > 0 && !S.pendingFrames[0].isKeyframe) {
                            S.pendingFrames.shift();
                            S.stats.dropped++;
                        }
                        while (S.pendingFrames.length > 0) {
                            const entry = S.pendingFrames.shift();
                            decodeAv1Frame(entry.data, entry.isKeyframe);
                        }
                    } catch (e) {
                        tryCodecs(index + 1);
                    }
                } else {
                    tryCodecs(index + 1);
                }
            })
            .catch(() => tryCodecs(index + 1));
    };
    tryCodecs(0);
}

function decodeAv1Frame(data, isKeyframe) {
    if (!S.decoderConfigured) {
        if (S.pendingFrames.length < 120) S.pendingFrames.push({ data, isKeyframe });
        return;
    }
    if (!S.decoder) return;

    const timestamp = S.frameCount * 16667;
    S.frameCount++;
    const type = isKeyframe ? 'key' : 'delta';
    const obuData = stripNonEssentialObus(data);
    try {
        const chunk = new EncodedVideoChunk({ type, timestamp, duration: 16667, data: obuData });
        S.decoder.decode(chunk);
        S.stats.received++;
    } catch (err) {
        S.stats.dropped++;
    }
}

// ── Decoded-frame handling + render (render-on-decode, no rAF) ────────────────

function onDecodedFrame(frame) {
    S.stats.decoded++;
    S._recoveryAttempts = 0;

    if (S._steadyToPerfOffset !== null && frame.timestamp > 0) {
        // frame.timestamp is backendTs*1000 (µs) for H.264/HEVC. Derive e2e.
        const captureMs = frame.timestamp / 1000;
        const capturePerf = captureMs - S._steadyToPerfOffset;
        const latency = performance.now() - capturePerf;
        if (latency > 0 && latency < 5000) S._lastLatencyMs = latency;
    }

    if (S.frameQueue.length >= 3) {
        frame.close();
        S.stats.dropped++;
        return;
    }
    S.frameQueue.push(frame);

    // Track resolution from frame dims; retry past the first frame since some
    // decoders report 0×0 initially (otherwise the overlay stays stuck on "?").
    const w = frame.displayWidth || frame.codedWidth || 0;
    if (w > 0) S._lastResolution = w + '×' + (frame.displayHeight || frame.codedHeight || 0);

    if (!S._firstFrameReported) {
        S._firstFrameReported = true;
        console.log('[VideoWorker] Posting firstframe, resolution=' + (S._lastResolution || '?'));
        post({ type: 'firstframe', resolution: S._lastResolution || '' });
    }

    pump();
}

// Render the freshest queued frame; serialize via the rendering guard so two
// VideoFrames are never touched concurrently (Chrome Windows HEVC NV12 race).
function pump() {
    if (S.stopped || S.rendering || S.frameQueue.length === 0) return;
    while (S.frameQueue.length > 1) {
        S.frameQueue.shift().close();
        S.stats.dropped++;
    }
    const frame = S.frameQueue.shift();
    S.rendering = true;
    drawFrame(frame)
        .then((ok) => {
            if (ok) S.stats.rendered++;
        })
        .finally(() => {
            S.rendering = false;
            postCounters(false);
            pump();
        });
}

// Delegate to the renderer (owns context + HEVC NV12 fallbacks). The renderer
// closes the frame; stats stay here per the render boundary.
async function drawFrame(frame) {
    if (!S.renderer) {
        try {
            frame.close();
        } catch (e) {}
        return false;
    }
    await S.renderer.draw(frame);
    return true;
}

// ── Main-thread message handling ─────────────────────────────────────────────

// Core frame entry (mirrors StreamView._processVideoFrame from the AV1 dispatch
// onward; stale/gap/stats/overlay stay on the main thread before this point).
function processFrame(data, isKeyframe, backendTs) {
    if (S.stopped || S._fatalDecodeError) return;

    // Establish the worker-local clock offset for latency once.
    if (backendTs !== undefined && backendTs > 0 && S._steadyToPerfOffset === null) {
        S._steadyToPerfOffset = backendTs - performance.now() - 30; // 30ms LAN floor
    }

    if (S.videoCodec === CODEC_AV1) {
        handleAv1Frame(data, isKeyframe);
        return;
    }

    // H.264 / HEVC: extract SPS/PPS from the first keyframe, then configure.
    if (!S.nalParser.isReady()) {
        if (isKeyframe) {
            if (S._idrRequested && S.pendingFrames.length > 0) {
                S.pendingFrames = [];
                S._idrRequested = false;
            }
            const ready = S.nalParser.feed(data);
            if (ready) configureDecoder();
        } else {
            if (S.pendingFrames.length > 30 && S.pendingFrames.length % 30 === 0) {
                requestIdr('no keyframe while buffering');
            }
        }
    }
    if (!S.decoderConfigured && S.nalParser.isReady()) configureDecoder();

    decodeFrame(data, isKeyframe, backendTs);
}

self.onmessage = (e) => {
    const m = e.data;
    switch (m.type) {
        case 'init':
            S.canvas = m.canvas;
            S.videoCodec = m.videoCodec;
            S.isChromeWindowsHevc = !!m.isChromeWindowsHevc;
            S.transport = m.transport || 'webrtc';
            // Always use a low-latency (desynchronized) context in the worker: it
            // lets the canvas bypass the document compositor and present sooner,
            // clawing back the ~1-frame latency that an OffscreenCanvas presented
            // from a worker otherwise adds vs the main-thread rAF path. (m.tearing
            // is kept for reference; tearing is acceptable for a real-time stream.)
            // Renderer assigned in a microtask; well before any decoded frame reaches
            // drawFrame. mw_webgpu flag arrives via the message (no localStorage here).
            createVideoRenderer(S.canvas, {
                desynchronized: true,
                videoCodec: S.videoCodec,
                isChromeWindowsHevc: S.isChromeWindowsHevc,
                webgpu: !!m.webgpu,
                algo: m.algo,
                hdr: !!m.hdr,
            }).then((r) => {
                S.renderer = r;
                // Apply an output size that may have arrived before the renderer.
                if (S.outW > 0 && S.outH > 0) r.setOutputSize(S.outW, S.outH);
                console.log(
                    '[VideoWorker] renderer=' + r.kind + ' (webgpu requested=' + !!m.webgpu + ')',
                );
                post({ type: 'rendererinfo', kind: r.kind, hdr: !!r.hdrActive });
            });
            S.nalParser = new NalParser();
            setupDecoder();
            console.log(
                '[VideoWorker] init codec=' +
                    S.videoCodec +
                    ' chromeWinHevc=' +
                    S.isChromeWindowsHevc,
            );
            break;
        case 'frame': {
            const data = new Uint8Array(m.data);
            processFrame(data, m.isKeyframe, m.backendTs);
            break;
        }
        case 'frameloss':
            S._referenceValid = false;
            break;
        case 'stats': {
            // Clock-offset refinement, mirroring StreamView._handleStatsMessage:
            // streamTimeMs is the backend steady clock at send time; receipt here
            // adds ~RTT/2 plus one postMessage hop (~ms). Replaces the crude
            // first-frame estimate (fixed 30ms pipeline floor) that otherwise
            // pins the worker's latency figure to a guess for the whole session.
            if (typeof m.streamTimeMs === 'number' && m.streamTimeMs >= 0) {
                const rtt = m.rttMs > 0 ? m.rttMs : 0;
                const refined = m.streamTimeMs - performance.now() + rtt / 2;
                if (!S._offsetFromStats) {
                    S._steadyToPerfOffset = refined;
                    S._offsetFromStats = true;
                } else {
                    S._steadyToPerfOffset = S._steadyToPerfOffset * 0.7 + refined * 0.3;
                }
            }
            break;
        }
        case 'resize':
            S.outW = m.outW;
            S.outH = m.outH;
            if (S.renderer) S.renderer.setOutputSize(S.outW, S.outH);
            break;
        case 'stop':
            S.stopped = true;
            if (S.decoder) {
                try {
                    S.decoder.close();
                } catch (e) {}
                S.decoder = null;
            }
            for (const f of S.frameQueue) {
                try {
                    f.close();
                } catch (e) {}
            }
            S.frameQueue = [];
            S.pendingFrames = [];
            break;
    }
};
