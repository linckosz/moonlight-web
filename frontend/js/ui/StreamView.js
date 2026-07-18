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
 * Fullscreen streaming overlay with WebCodecs video decoding and Canvas rendering.
 *
 * Receives raw H.264 Annex B frames over WebRTC DataChannel from the backend,
 * decodes them via WebCodecs VideoDecoder, and renders to a <canvas> element.
 * Captures keyboard/mouse events and sends them back as JSON over the input DC.
 */
import { WebRtcDataChannel } from '../api/WebRtcDataChannel.js';
import { WebRtcMedia } from '../api/WebRtcMedia.js';
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { AudioPipeline } from '../audio/AudioPipeline.js';
import { JitterController } from '../stream/JitterController.js';
import { GamepadManager } from '../stream/GamepadManager.js';
import {
    NalParser,
    splitNals,
    buildDescription,
    getCodecString,
    toAvcc,
    H264_FALLBACK_CODEC_STRINGS,
    HEVC_FALLBACK_CODEC_STRINGS,
    HEVC_ANNEXB_CODEC_STRINGS,
    CODEC_H264,
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

import { IS_TOUCH_DEVICE, IS_MOBILE_OR_TABLET, pickAutoEnhancer } from '../util/BrowserDetect.js';
import { createVideoRenderer } from '../stream/renderers/createRenderer.js';
import { t } from '../i18n/i18n.js';
import { Icons } from './icons.js';
import { StreamViewKeyboard } from './StreamViewKeyboard.js';
import { StreamViewTouch } from './StreamViewTouch.js';
import { StreamViewFullscreen } from './StreamViewFullscreen.js';

/**
 * Workaround for Chrome GPU compositor bug on Windows: the first HEVC
 * VideoFrame drawn to a Canvas2D via drawImage(VideoFrame) reads the NV12
 * UV plane from uninitialized GPU memory, producing a green image.
 *
 * Two-part fix:
 *   1. ALWAYS use createImageBitmap() for rendering (never drawImage with
 *      a VideoFrame directly).  This routes through the RGBA bitmap path
 *      which does not hit the buggy YUV compositor pipeline.
 *   2. For the first few NV12 frames, call frame.copyTo() before
 *      createImageBitmap() to force a low-level D3D11 GPU readback that
 *      fully reads both Y and UV planes into CPU memory.  This ensures
 *      the GPU texture data is fully resident before createImageBitmap
 *      reads it — some Windows GPU/driver combinations also have issues
 *      with createImageBitmap(VideoFrame) on NV12 HEVC textures.
 *
 * Alt-Tab (GPU context loss/restore) also fixes the green image — confirming
 * it's a compositor/driver initialization issue, not a decode problem.
 */

/**
 * Sliding window statistics tracker.
 * Maintains a fixed-duration window of samples and provides min/max/avg.
 */
class SlidingStats {
    constructor(windowMs = 5000) {
        this._windowMs = windowMs;
        this._samples = []; // [{ time, value }]
    }

    addSample(value) {
        this._samples.push({ time: performance.now(), value });
        this._prune();
    }

    get count() {
        this._prune();
        return this._samples.length;
    }

    get min() {
        this._prune();
        if (this._samples.length === 0) return 0;
        let m = Infinity;
        for (const s of this._samples) {
            if (s.value < m) m = s.value;
        }
        return m;
    }

    get max() {
        this._prune();
        if (this._samples.length === 0) return 0;
        let m = -Infinity;
        for (const s of this._samples) {
            if (s.value > m) m = s.value;
        }
        return m;
    }

    get avg() {
        this._prune();
        if (this._samples.length === 0) return 0;
        let sum = 0;
        for (const s of this._samples) {
            sum += s.value;
        }
        return sum / this._samples.length;
    }

    _prune() {
        const cutoff = performance.now() - this._windowMs;
        this._samples = this._samples.filter((s) => s.time > cutoff);
    }
}

export class StreamView {
    constructor(
        container,
        signalingUrl,
        host,
        videoCodec,
        gamingMode = true,
        upnpEnabled = true,
        upnpAvailable = true,
        transport = 'webrtc',
        transportMode = undefined,
        isRemote = false,
        showPerformanceStats = true,
        touchSensitivity = 2.0,
        tearing = false,
        videoWorker = true,
        videoEnhancement = 'off',
        videoEnhancementAlgo = 'auto',
        yuv444 = false,
        hdrEnabled = false,
        touchScreen = false,
        audioTimeStretch = true,
        opts = {},
    ) {
        this.container = container;
        // ── Standby mode (seamless quality switching) ───────────────────────
        // A standby view connects, decodes and renders HIDDEN and MUTED next to
        // the live one; input/wake-lock/overlays stay off until activate() flips
        // it to the visible, controlling view (called on its first frame).
        this._standby = opts.standby === true;
        // Backend stream slot this view runs on (0 = /ws ports, 1 = /ws1) and
        // the uniqueid it launched with — quit() scopes its /quit with them so
        // retiring one leg of a dual stream never cancels the other.
        this._sessionSlot = typeof opts.sessionSlot === 'number' ? opts.sessionSlot : 0;
        this._slotUniqueId = opts.slotUniqueId || null;
        // Audio time-stretch (WSOLA) — server-controlled kill switch.
        this._audioTimeStretch = audioTimeStretch !== false;
        // Mobile only: direct touch-screen input (absolute finger position) in
        // place of the relative trackpad model. Off by default.
        this._touchScreen = touchScreen === true;
        // YUV 4:4:4 chroma negotiated by the backend (vs default 4:2:0). Used
        // only to annotate the codec in the stats overlay.
        this._yuv444 = yuv444 === true;
        // HDR mode requested by the user. Actual HDR depends on both the
        // user's preference and Sunshine's negotiated format (HEVC Main10 /
        // AV1 10-bit). _hdrNegotiated is set after codec detection.
        this._hdrEnabled = hdrEnabled === true;
        this._hdrNegotiated = false;
        // Allow tearing: when true, VSync pacing is disabled — the canvas 2D
        // context is created with desynchronized=true and frames are presented
        // as soon as they are decoded (lower latency, possible tearing) on
        // transports that render through the canvas (DataChannel / WSS).
        // Only effective on Chromium desktop; ignored elsewhere.
        this._tearing = tearing === true;
        this.signalingUrl = signalingUrl;
        this.host = host;
        this.videoCodec = videoCodec || 'auto';
        this._transport = transport;
        this._transportMode = transportMode || transport;
        this._gamingMode = gamingMode;
        // Force gaming mode off on touch devices — pointer lock and mouse
        // capture are irrelevant when input is touch-based.
        if (IS_TOUCH_DEVICE) {
            this._gamingMode = false;
        }
        this._upnpEnabled = upnpEnabled;
        this._upnpAvailable = upnpAvailable;
        this._isRemote = isRemote;
        this._showPerfStats = showPerformanceStats;

        // ── OffscreenCanvas video worker ────────────────────────────────────
        // Moves WebCodecs decode + canvas rendering off the main UI thread.
        // Controlled by the "Decode on worker thread" setting, a tri-state.
        // The setting is hidden from the UI (kept as a debug override); the
        // effective default is 'auto':
        //   'auto' (default) → ON above 4 logical cores, OFF at ≤4, regardless
        //     of device type. Below 5 cores the extra hot thread contends for
        //     scarce cores and presentation can be slower → lower fps.
        //   'on' / 'off' (or legacy boolean true/false) → explicit override.
        // Never used for the native RTP media transport (the browser renders the
        // <video> directly). Falls back to the main-thread pipeline automatically
        // if the worker cannot start.
        const workerMode =
            videoWorker === true ? 'on' : videoWorker === false ? 'off' : videoWorker || 'auto';
        let workerWanted;
        if (workerMode === 'on') {
            workerWanted = true;
        } else if (workerMode === 'off') {
            workerWanted = false;
        } else {
            // Auto: enable above 4 logical cores, regardless of device type.
            const cores = navigator.hardwareConcurrency || 4;
            workerWanted = cores > 4;
        }
        this._useWorker = false;
        try {
            this._useWorker =
                workerWanted &&
                !this._hdrEnabled &&
                transport !== 'webrtc-media' &&
                typeof Worker !== 'undefined' &&
                typeof OffscreenCanvas !== 'undefined' &&
                typeof HTMLCanvasElement !== 'undefined' &&
                typeof HTMLCanvasElement.prototype.transferControlToOffscreen === 'function';
        } catch (e) {
            this._useWorker = false;
        }
        this._videoWorker = null;
        // Renderer selection for the canvas path (DC/WSS; webrtc-media uses <video>).
        // WebGPU is the preferred renderer on ALL devices; createVideoRenderer falls
        // back to Canvas2D when WebGPU is unavailable. The algo decides what WebGPU
        // does: 'off' = pass-through (Enhancer disabled), 'sgsr'/'fsr1' = upscaler.
        // 'force2d' (debug) bypasses WebGPU to exercise the Canvas2D path.
        // Forwarded to the worker too (localStorage is unavailable there).
        let algo = 'off'; // default: WebGPU pass-through (no upscaler)
        let wantWebGpu = true; // WebGPU is the preferred canvas renderer
        if (videoEnhancement === 'on') {
            const sel = videoEnhancementAlgo || 'auto';
            if (sel === 'force2d') {
                wantWebGpu = false; // debug: force the Canvas2D renderer
            } else if (sel === 'auto') {
                // 'auto' picks by platform (see pickAutoEnhancer): desktops + iOS
                // → FSR1; beefy 1080p+ Android → FSR1; everything else → SGSR.
                // WebGPU absence is handled downstream (Canvas2D, no enhancement).
                algo = pickAutoEnhancer();
            } else {
                algo = sel === 'sgsr' || sel === 'fsr1' ? sel : 'sgsr';
            }
        }
        this._videoEnhancementAlgo = algo;
        // Whether the user enabled the Enhancer (used to flag it OFF in the overlay
        // when the stream lands on webrtc-media, where it can't be applied).
        this._videoEnhancementRequested = videoEnhancement === 'on';
        this._wantWebGpu = wantWebGpu;
        // Dev override (no UI): also force the Canvas2D path for comparison.
        try {
            if (localStorage.getItem('mw_force_2d') === '1') this._wantWebGpu = false;
        } catch (e) {}

        // HDR routing (DataChannel/WSS only; decided after algo/wantWebGpu):
        //  - true HDR via the <video> sink (MediaStreamTrackGenerator): the canvas
        //    paths tone-map HDR away, so <video> presents it natively. The Enhancer
        //    is effectively OFF when HDR (it can't run on the <video> sink).
        //  - HDR→SDR (ACES) tone-map path: raw PQ planes read back (P010) and
        //    tone-mapped in the renderer's Pass 0, then FSR1/SGSR run on a normal
        //    SDR canvas. Costs a software decode (opaque hardware frames don't
        //    support copyTo). Auto-selected when the Enhancer is on (it can't run
        //    on the <video> sink) or when the display can't show HDR (HDR sent to
        //    an SDR output clips → overexposed colors). Dev override:
        //    mw_hdr_tonemap = '1' forces it on, '0' forces it off.
        // Display HDR capability — main thread only (matchMedia doesn't exist in
        // workers) and evaluated once at stream start (renderers can't hot-swap);
        // reflects the OS toggle too (Windows "HDR off" → matches false).
        let displayHdr = false;
        try {
            displayHdr = window.matchMedia('(dynamic-range: high)').matches;
        } catch (e) {}
        this._displayHdr = displayHdr;
        let tonemapWanted = this._videoEnhancementAlgo !== 'off' || !displayHdr;
        try {
            const dev = localStorage.getItem('mw_hdr_tonemap');
            if (dev === '1') tonemapWanted = true;
            else if (dev === '0') tonemapWanted = false;
        } catch (e) {}
        this._hdrTonemap =
            tonemapWanted &&
            this._hdrEnabled &&
            transport !== 'webrtc-media' &&
            this._wantWebGpu &&
            !!navigator.gpu;
        this._useVideoSink =
            this._hdrEnabled &&
            !this._hdrTonemap &&
            transport !== 'webrtc-media' &&
            typeof MediaStreamTrackGenerator !== 'undefined';
        this._workerLastDecoded = 0;

        // Backend timestamp tracking for stale frame detection.
        // WebRTC SCTP unordered delivery (ordered=false) can reassemble frames
        // out of order.  An older frame arriving after a newer one would
        // overwrite current canvas content with stale pixels ("ghosting").
        // We track the maximum backend timestamp seen and drop frames whose
        // timestamp is behind it, unless they are bootstrapping keyframes.
        this._maxBackendTs = undefined;

        /** Callback invoked after quit() completes cleanup. Used by MoonlightApp
         *  to restore the underlying main view (apps/hosts). */
        this.onQuit = null;

        /** Callback invoked when the transport fails to establish a connection
         *  (ICE failure / closed before ever connecting). MoonlightApp relaunches
         *  with the next transport in the priority chain. Receives a reason string. */
        this.onConnectionFailed = null;
        /** Callback invoked on SUSTAINED network congestion (IDR storms, backend
         *  SCTP backpressure drops, PLI storms). MoonlightApp reacts by
         *  relaunching the stream with a degraded session-only profile
         *  (bitrate −30% steps, media transport, 60 fps cap). */
        this.onCongestion = null;
        // Congestion monitor: sliding window of congestion signals — see
        // _recordCongestionEvent().
        this._congEvents = [];
        this._congFiredAt = 0;
        this._lastBpDrops = 0; // cumulative backend backpressure drops (stats msg)
        this._lastPliCount = 0; // cumulative PLIs sent (media mode getStats)
        /** True once the transport has connected at least once (DCs open / media
         *  playing). Distinguishes a connection failure (→ chain fallback) from a
         *  mid-stream disconnect (→ normal quit). */
        this._everConnected = false;
        /** Guard so onConnectionFailed fires at most once per session. */
        this._connectionFailureReported = false;

        // ── UPnP status ─────────────────────────────────────────────────────
        // UPnP toasts intentionally suppressed: they have no value during a
        // streaming session and only clutter the UI. The UPnP state is visible
        // in the Admin/Settings UI before the stream starts, where the user can
        // act on it. During streaming, the toast is pure distraction.
        if (this._transport === 'webrtc-media') {
            this.webrtc = new WebRtcMedia(signalingUrl);
        } else if (this._transport === 'wss') {
            // Legacy WSS mode: direct WebSocket passthrough via StreamRelay,
            // without any WebRTC PeerConnection or DataChannels.
            this.webrtc = new WebRtcDataChannel(signalingUrl, {
                wssMode: true,
                wssFragmented: false,
            });
        } else {
            this.webrtc = new WebRtcDataChannel(signalingUrl);
        }
        // Browser-driven transport fallback: ICE failures surface as errors so
        // MoonlightApp can relaunch with the next transport (no in-session WS
        // reroute). WebRtcMedia ignores this flag (no in-session WS fallback).
        this.webrtc._chainFallback = true;
        this.pointerLocked = false;
        /** Gaming mode focus state: true when pointer lock is active (cursor captured).
         *  false initially (cursor visible, absolute mouse tracking).
         *  Set to true on first click, reset when pointer lock is lost. */
        this._mouseFocused = false;
        // Last known pointer position in client coordinates (updated on every
        // mousemove). Used to re-evaluate the local cursor hide/show decision
        // when the window regains focus without the pointer moving — otherwise
        // the local arrow stays visible over the host cursor (double cursor).
        this._lastMouseClientX = undefined;
        this._lastMouseClientY = undefined;
        // Per-session "closed" flags: the user can dismiss the stats and the
        // immersive exit overlay with their × button; they stay hidden after.
        this._statsClosed = false;
        this._immersiveClosed = false;

        // Audio transport: on WebRTC transports (webrtc / webrtc-media) audio is a
        // native RTP Opus track decoded by the browser (jitter buffer + in-band
        // FEC + PLC) and played through an <audio> element — no AudioPipeline.
        // Only WSS (no PeerConnection, so no RTP track) keeps the AudioPipeline
        // (WASM/WebCodecs Opus decode + worklet jitter buffer).
        this._nativeAudio = this._transport === 'webrtc' || this._transport === 'webrtc-media';
        this.audioPipeline = this._nativeAudio
            ? null
            : new AudioPipeline({ timeStretch: this._audioTimeStretch });
        this._audioLogged = false;

        // WebCodecs
        this.decoder = null;
        this.decoderConfigured = false;
        this.decoderConfiguring = false;
        this.nalParser = new NalParser();
        this.frameQueue = [];
        this.pendingFrames = []; // frames buffered before decoder config
        this.frameCount = 0;
        this.renderRunning = false;
        this._rendering = false; // Guard: prevents overlapping GPU render ops
        this._immediateRender = false; // Tearing on: draw is driven by decoder output, not rAF
        this._renderer = null; // VideoRenderer (Canvas2D / WebGPU); owns the context
        this._activeRendererKind = null; // 'canvas2d' | 'webgpu' (for the overlay)
        this._rendererHdrActive = false; // true once a renderer reports an HDR canvas
        this._resizeObserver = null; // tracks the DOM output size (device px)
        this._outW = 0;
        this._outH = 0;
        // Recycled buffer for HEVC RGBA copyTo — avoids per-frame allocation
        this._rgbaBuffer = null;
        this._rgbaBufferSize = 0;

        // Note: frame reordering via a reorder buffer was removed after causing
        // excessive IDR request flooding (every SCTP gap triggered a skip+IDR).
        // The backend's stale-buffered-keyframe detection is sufficient for the
        // green-image fix. Frames are processed in arrival order.

        // Stats
        this.stats = { received: 0, decoded: 0, rendered: 0, dropped: 0 };

        // Overlay stats
        this._overlayEl = null;
        this._overlayInterval = null;
        // Immersive-mode exit reminder (discreet, draggable, top-center).
        this._immersiveOverlay = null;
        this._resolution = ''; // "1920x1080" — set once on first frame
        this._codec = this.videoCodec; // Same as videoCodec
        this._transport = transport; // "webrtc" or "wss"
        this._totalBytes = 0; // Cumulative video bytes for bitrate
        this._startTime = performance.now(); // Stream start time for bitrate calc
        this._fpsTimestamps = []; // performance.now() per decoded frame

        // ── Stats overlay ───────────────────────────────────────────────────
        // Latency is the sum of independently measured legs (moonlight-qt
        // style): each leg is timed within a single clock domain, so no
        // cross-machine clock offset is ever needed. (The previous
        // steady_clock→performance.now() offset approach displayed the offset
        // estimation error as a constant — the overlay froze on a wrong value
        // until the next offset re-blend.)
        this._hostRttStats = new SlidingStats(5000); // backend ↔ Sunshine one-way (ms)
        this._browserRttStats = new SlidingStats(5000); // browser ↔ backend RTT (ms)
        this._decodeLatencyStats = new SlidingStats(5000); // backend pipeline latency (ms)
        this._hostProcStats = new SlidingStats(5000); // Sunshine capture→encode (RTP ext, ms)
        this._clientLatencyStats = new SlidingStats(2000); // decode() submit → render done (ms)
        this._chunkSubmitTimes = new Map(); // chunk timestamp (µs) → perf.now() at decode()
        this._pingSeq = 0;
        this._pingInterval = null;

        // ── webrtc-media native stats (getStats polling) ──────────────────────
        // RTP media track frames bypass the JS decoder, so fps/bitrate/latency
        // must be read from RTCPeerConnection.getStats() instead of the
        // WebCodecs pipeline counters used by the DataChannel transports.
        this._mediaStatsTimer = null;
        this._mediaFps = 0;
        this._mediaBitrateMbps = 0;
        // Per-tick latency samples (jitter buffer + network + decode). A sliding
        // window instead of a sticky "last good value": if the poll stops
        // producing samples the display goes back to '--' instead of freezing.
        this._mediaLatencyStats = new SlidingStats(3000);
        this._lastInboundBytes = 0;
        this._lastInboundFrames = 0;
        this._lastInboundStatsTime = 0;
        // Interval deltas for the latency estimate (jitterBufferDelay and
        // totalDecodeTime are cumulative since stream start — the cumulative
        // average lags reality more and more as the session ages).
        this._lastJbDelay = 0;
        this._lastJbEmitted = 0;
        this._lastDecodeTime = -1;
        this._lastDecodedForLatency = 0;

        // ── Adaptive jitter buffer (webrtc-media only, behind mw_jitter_auto) ──
        // Sits at 0 on a clean link (= current behavior); grows only when jitter/
        // loss/freezes appear, which also reactivates the backend NACK (neutralized
        // at target=0). Control law lives in JitterController; we feed it raw stats.
        this._jitterAuto = false;
        try {
            this._jitterAuto = localStorage.getItem('mw_jitter_auto') === '1';
        } catch (e) {}
        this._jitterController = new JitterController();

        // Gamepad bridge (Xbox/PlayStation) — created lazily on first connect.
        this._gamepadManager = null;

        // Forward stats/pong messages from backend to the stats overlay system.
        // Set before setupWebRtc() so it's active when connect() is called.
        this.webrtc.onStats = (msg) => this._handleStatsMessage(msg);

        // Session taken over by another device → graceful cyberpunk exit.
        this.webrtc.onTakeover = () => this._handleTakeover();

        // Device access revoked by the admin → same forced-exit path.
        this.webrtc.onRevoked = () => this._handleRevoked();

        // Physical keys currently held down (e.code → keyup payload). Used to
        // release everything when the window loses focus: the OS can steal focus
        // mid-press (e.g. the Windows key opens the local Start menu), so the
        // keyup never reaches us and the host keeps the key — turning a later
        // "e" into Win+E, etc. We synthesize the missing keyups on blur/hidden.
        this._heldPhysKeys = new Map();

        // ── Clipboard bridge state ─────────────────────────────────────────
        // Enabled by a backend 'clipboardcaps' message, sent only when the
        // streamed host is the backend machine (shared clipboard).
        this._clipboardEnabled = false;
        // Swallowed Ctrl/Cmd+V keydown waiting for its native 'paste' event
        // ({ code, keydownMsg, injectCtrl, timer }).
        this._pendingPasteKey = null;
        // e.code of a V key whose keyup must be swallowed (the backend
        // already injected the complete paste chord).
        this._suppressPasteKeyUpCode = null;
        // Host clipboard text waiting for a user gesture to be written
        // locally (Safari requires a gesture for clipboard.writeText,
        // Firefox a <5s-old one; Chrome writes immediately).
        this._pendingClipboardWrite = null;

        // Bound handlers
        this._onKeyDown = (e) => this.handleKeyDown(e);
        this._onKeyUp = (e) => this.handleKeyUp(e);
        this._onPaste = (e) => this.handlePaste(e);
        this._onPointerDownFlush = () => this._flushPendingClipboard();
        this._onWindowBlur = () => this._releaseAllPhysKeys();
        // Regaining window focus (Alt-Tab back from another app) may leave the
        // pointer over the streamed picture without a mousemove — re-apply the
        // cursor hide so the local arrow doesn't double the host cursor.
        this._onWindowFocus = () => this._refreshLocalCursorOnFocus();
        this._onMouseMove = (e) => this.handleMouseMove(e);
        this._onMouseDown = (e) => this.handleMouseDown(e);
        this._onMouseUp = (e) => this.handleMouseUp(e);
        this._onWheel = (e) => this.handleWheel(e);
        this._onPointerLockChange = () => this.handlePointerLockChange();
        this._onContextMenu = (e) => e.preventDefault();

        // ── Touch event handlers (mobile) ─────────────────────────────────
        this._onTouchStart = (e) => this.handleTouchStart(e);
        this._onTouchMove = (e) => this.handleTouchMove(e);
        this._onTouchEnd = (e) => this.handleTouchEnd(e);

        // Touch state — laptop-trackpad model: relative cursor deltas,
        // tap = left click, two-finger tap = right click, two-finger drag =
        // scroll (or pinch = zoom), three-finger drag = pan the zoomed view.
        this._touchActive = false;
        this._touchStartX = 0;
        this._touchStartY = 0;
        this._touchLastX = 0;
        this._touchLastY = 0;
        this._touchStartTime = 0;
        this._touchFingerCount = 0;
        this._touchTapThreshold = 10; // px max distance for a tap (vs drag)
        this._touchTapTimeThreshold = 300; // ms max duration for a tap
        this._touchHadTwoFingers = false; // true if 2 fingers were active during the current touch sequence
        this._touchMaxFingers = 0; // max simultaneous fingers in the current sequence (1/2/3)
        this._touchMoved = false; // true once the primary finger moved past the tap threshold
        this._touchDragging = false; // true while a long-press drag holds the left button down
        this._touchLongPressTimer = null; // timer that engages drag after a still hold
        this._touchLongPressMs = 450; // ms hold (still) before a drag engages
        this._uiTouchX = 0; // start position of a tap on stream UI (buttons / stats card)
        this._uiTouchY = 0;
        // Pinch-zoom state (mobile): scale + pan applied to the streamed display only.
        this._zoom = 1; // current display scale (1..8)
        this._panX = 0; // pan offset in CSS px (applied before scale)
        this._panY = 0;
        this._pinchPrevDist = 0; // previous finger spacing during a multi-finger gesture
        this._pinchPrevCx = null; // previous centroid (of all touches)
        this._pinchPrevCy = null;
        this._twoFingerMode = null; // locked gesture for the current 2-finger sequence: 'zoom' | 'scroll'
        this._lastMoveFingerCount = 0; // finger count of the last touchmove (reseed trackers on change)
        // Two-finger scroll: amplification + inertial (momentum) glide after release.
        this._scrollScale = 4; // finger px → wheel delta units (faster scroll)
        this._scrollAccum = 0; // fractional wheel-delta carry between frames
        this._scrollSamples = []; // recent {t, y} centroid samples (flick velocity)
        this._scrollMomentumRaf = null; // rAF id for the inertial glide loop
        // Trackpad acceleration factor (CSS px → host deltas), user-configurable in Settings.
        this._touchSensitivity =
            typeof touchSensitivity === 'number' && touchSensitivity > 0 ? touchSensitivity : 2.0;

        // Virtual keyboard (touch devices): hidden capture element + toggle button.
        this._kbdBtn = null;
        this._kbdCapture = null;
        this._kbdVisible = false;
        this._kbdBlurAt = 0; // timestamp of last capture blur
        this._onViewportResize = null; // VisualViewport handler (keyboard push-up)

        // Mobile fullscreen button state
        this._mobileFsBtn = null;

        // CSS fallback fullscreen (when Fullscreen API fails, e.g. iOS canvas)
        this._cssFullscreen = false;

        // Keyboard Lock (Chrome/Edge): while in native fullscreen, lock the
        // Escape key so it is delivered to our handler (and forwarded to the
        // host as VK_ESCAPE) instead of exiting fullscreen. Fullscreen is then
        // left only via the keyboard combo. Synced on every fullscreenchange.
        this._keyboardLocked = false;
        this._onFsChangeLock = () => this._syncKeyboardLock();
        // Transient on-screen hint element (reused for the fullscreen-exit tip).
        this._transientHintEl = null;
        this._transientHintTimer = null;

        // Visibility change: when returning from Alt-Tab, force browser to
        // re-composite all layers.  On Chrome Windows, the GPU compositor may
        // cache a corrupt layer (green tint from NV12→RGB bug in Canvas2D).
        // Invalidation via will-change toggle forces a fresh composite.
        this._onVisibilityChange = () => {
            // Going to background can swallow keyups — release everything so no
            // modifier stays stuck on the host.
            if (document.visibilityState === 'hidden') this._releaseAllPhysKeys();
            if (document.visibilityState === 'visible' && !this._quitting) {
                // Coming back to the tab may leave the pointer over the picture
                // without a mousemove — re-hide the local cursor (double cursor).
                this._refreshLocalCursorOnFocus();
                const header = document.querySelector('.stream-header');
                if (header) {
                    // Toggle will-change to force layer re-compositing
                    header.style.willChange = 'transform, opacity';
                    requestAnimationFrame(() => {
                        header.style.willChange = '';
                    });
                }
            }
        };

        // beforeunload: fire-and-forget quit when tab/window is closed
        this._onBeforeUnload = () => {
            if (this._quitting) return;
            navigator.sendBeacon(
                `/api/hosts/${this.host.uuid}/quit`,
                new Blob(['{}'], { type: 'application/json' }),
            );
        };

        // pagehide: iOS/WebKit rarely fires beforeunload (tab close, app close),
        // so the quit beacon must also be sent here. Sent even when
        // event.persisted is true (bfcache): the stream cannot survive a page
        // freeze, so a restored page would resume in a dead state anyway.
        this._onPageHide = () => this._onBeforeUnload();

        // Reference validity: false after a gap/error, true once a keyframe is decoded.
        // Delta frames are dropped when false to avoid corrupted output.
        this._referenceValid = true;
        // Last seen frameId for gap detection (frameId is optional per transport).
        this._lastFrameId = -1;

        // Decoder queue backpressure: a saturated decodeQueueSize is tolerated
        // for QUEUE_STALL_MS (transient bursts: TCP delivery on WSS, keyframe
        // decode cost on mobile) before deltas are dropped; the decoder is only
        // reset after QUEUE_RESET_MS of sustained saturation.
        this.DECODE_QUEUE_MAX = 8;
        this.QUEUE_STALL_MS = 200;
        this.QUEUE_RESET_MS = 1000;
        this._queueStallStart = 0;
        // Last EncodedVideoChunk timestamp (µs) — enforces monotonicity.
        this._lastChunkTs = -1;

        // IDR request state: if no keyframe arrives after buffering many frames
        // without decoder config, we ask the backend to request an IDR from Sunshine.
        // This is a safety net for the rare race where the initial IDR is lost.
        this._idrRequested = false;
        this._idrTimeout = null;
        // Timestamp of the last requestidr sent — drives the 1s retry in _requestIdr().
        this._lastIdrRequestMs = 0;
        // Terminal decoder failure (codec unusable in this browser): all
        // further video processing is skipped, an error overlay is shown.
        this._fatalDecodeError = false;

        // Proactive IDR scheduling: after the VideoDecoder is first configured,
        // request a clean keyframe to overwrite any green/corrupted first decode.
        // Only fires once per stream session (set true after scheduling).
        this._proactiveIdrScheduled = false;

        // Shortcuts slide auto-hide timer (5s after first frame)
        this._shortcutsTimeout = null;

        // Decoder recovery guard: prevents re-entrant recovery when the error
        // callback fires during setupDecoder() (called from within recovery).
        this._decoderRecovering = false;
        // Recovery attempt counter + limit: prevents infinite recovery loops if
        // the bitstream is fundamentally corrupted (e.g., persistent packet loss).
        this._recoveryAttempts = 0;
        this.MAX_RECOVERY_ATTEMPTS = 10;

        // Guard flag to prevent re-entrant quit() calls
        this._quitting = false;

        // Set when the session was taken over by another device, so the generic
        // disconnect path (onClose) stays silent — _handleTakeover() owns the exit.
        this._takenOver = false;

        // HEVC fallback: set to true when the browser does not support HEVC decoding
        // (e.g. Windows Chrome). The onQuit callback should detect this and re-launch
        // with H.264 forced via MoonlightApp.launchApp(host, app, 'h264').
        this._codecFallbackRequested = false;
        // Target {codec, hdr} for the next launch attempt, set by
        // _requestCodecFallback() following the HEVC HDR → AV1 HDR → HEVC SDR →
        // H.264 SDR chain. Read by MoonlightApp._onStreamingQuit().
        this._codecFallback = null;

        // Platform flag: Chrome Windows — gates the HEVC NV12 RGBA copyTo path
        // (green-tint workaround). Set by _logPlatformInfo() before first frame.
        this._isChromeWindowsHevc = false;

        // Annex B mode: true when the decoder was configured WITHOUT a description.
        // In this mode, EncodedVideoChunk data uses Annex B format (start codes)
        // instead of AVCC (length prefixes).  Chromium's keyframe validator
        // (AnalyzeAnnexB) only parses Annex B — without this, HEVC keyframes
        // are falsely rejected on Chrome/Edge.
        this._noDescription = false;

        this.render();

        // Show step 1 immediately (before WebRTC connection)
        this._updateStartupStep(1);

        // Hide the "click to capture" hint in normal mode (no pointer lock)
        if (!this._gamingMode && this.hintEl) {
            this.hintEl.style.display = 'none';
        }

        // Pre-flight: non-blocking connectivity check for diagnostics.
        // Runs in parallel with the WS connection — results help diagnose
        // cases where HTTPS works but WSS doesn't (proxy, antivirus, TLS).
        this._preflightConnectivityCheck();

        // Platform / feature detection — helps diagnose iOS WebKit issues
        // where WebCodecs or Canvas rendering may behave differently.
        this._logPlatformInfo();

        this.setupWebRtc();
        // Standby: no input until promote — a hidden view must never swallow
        // the keyboard/mouse of the live one. bindEvents() runs in activate().
        if (!this._standby) this.bindEvents();
        this.startRenderLoop();
        // Native-audio transports decode via the browser <audio> element; only
        // WSS needs the AudioPipeline.
        if (!this._nativeAudio) this.initAudioAsync();
    }

    // --- Connectivity pre-flight (non-blocking diagnostic) ------------------

    /**
     * Non-blocking HTTP(S) health check that runs in parallel with the
     * WebSocket connection attempt.  Logs the result to help diagnose
     * network issues when HTTPS works but WSS does not.
     *
     * Common failure patterns:
     *   - Timeout (5s) → firewall blocking port 443
     *   - TypeError (Failed to fetch) → DNS resolution failure or TLS cert error
     *   - OK + WS fails → proxy/antivirus intercepting WS upgrade specifically
     */
    async _preflightConnectivityCheck() {
        try {
            const controller = new AbortController();
            const timeoutId = setTimeout(() => controller.abort(), 5000);

            const resp = await fetch('/api/health', { signal: controller.signal });
            clearTimeout(timeoutId);

            if (resp.ok) {
                console.log(
                    '[PreFlight] HTTPS /api/health OK (status=' +
                        resp.status +
                        ') — server is reachable',
                );
            } else {
                console.warn(
                    '[PreFlight] HTTPS /api/health returned ' +
                        resp.status +
                        ' — unexpected status',
                );
            }
        } catch (err) {
            if (err.name === 'AbortError') {
                console.warn(
                    '[PreFlight] HTTPS /api/health timed out (5s) — ' +
                        'possible firewall blocking port 443',
                );
            } else {
                console.warn(
                    '[PreFlight] HTTPS /api/health failed: "' +
                        err.message +
                        '" — possible DNS or TLS issue',
                );
            }
        }
    }

    // --- Platform / feature detection ---------------------------------------

    /** Log browser platform and WebCodecs feature availability for diagnostics.
     *  Helps identify iOS WebKit limitations (createImageBitmap, AudioWorklet). */
    _logPlatformInfo() {
        const ua = navigator.userAgent || '';
        const isIOS = /iPad|iPhone|iPod/.test(ua) || (/Mac/.test(ua) && 'ontouchend' in document);
        const isSafari = /Safari\//.test(ua) && !/Chrome\//.test(ua);
        const hasVideoDecoder = typeof VideoDecoder !== 'undefined';
        const hasIsConfigSupported =
            hasVideoDecoder && typeof VideoDecoder.isConfigSupported === 'function';
        const hasCreateImageBitmap = typeof createImageBitmap !== 'undefined';
        const hasAudioWorklet = typeof AudioWorkletNode !== 'undefined';
        const hasCanvas2D = typeof CanvasRenderingContext2D !== 'undefined';
        const hasIsContextLost =
            hasCanvas2D && typeof CanvasRenderingContext2D.prototype.isContextLost === 'function';

        if (!hasIsContextLost) {
            console.warn(
                '[Platform] CanvasRenderingContext2D.isContextLost() is NOT supported ' +
                    '— Safari/WebKit limitation. Context loss detection disabled.',
            );
        }

        // Detect Chrome on Windows — the HEVC NV12 RGBA copyTo path
        // (green-tint workaround for Chrome Windows D3D11 compositor) is
        // only safe on Windows. Chrome macOS/iOS has a stride bug in
        // frame.copyTo({ format: 'RGBA' }) that causes 4x horizontal stretch.
        const _isChromeWin =
            /Chrome\//.test(ua) && /Windows/.test(ua) && !/Edg\//.test(ua) && !/OPR\//.test(ua);
        this._isChromeWindowsHevc = _isChromeWin;

        console.log(
            '[Platform] iOS=' +
                isIOS +
                ' Safari=' +
                isSafari +
                ' VideoDecoder=' +
                hasVideoDecoder +
                ' isConfigSupported=' +
                hasIsConfigSupported +
                ' createImageBitmap=' +
                hasCreateImageBitmap +
                ' AudioWorklet=' +
                hasAudioWorklet +
                ' Canvas2D=' +
                hasCanvas2D +
                ' isContextLost=' +
                hasIsContextLost +
                ' ChromeWinHEVC=' +
                _isChromeWin +
                ' UA: ' +
                ua.substring(0, 120),
        );

        if (isIOS || isSafari) {
            console.log(
                '[Platform] Running on Apple platform — ' +
                    'createImageBitmap(VideoFrame) requires iOS 17+ / Safari 17+. ' +
                    'If the screen is black, the Canvas2DRenderer fallback chain ' +
                    'should handle this automatically.',
            );
        }
    }

    // =========================================================================
    // Audio Pipeline
    // =========================================================================

    /**
     * Initialise the AudioPipeline asynchronously.
     *
     * Safe to call from the constructor — non-blocking.  Audio starts silently
     * until the worklet module loads (usually <100ms).
     */
    async initAudioAsync() {
        const ok = await this.audioPipeline.init();
        if (ok) {
            console.log('[StreamView] AudioPipeline ready');
            // Resume if the context was suspended (autoplay policy).
            // StreamView is created after a user click, so this usually succeeds.
            if (this.audioPipeline.context && this.audioPipeline.context.state === 'suspended') {
                await this.audioPipeline.resume();
            }
        } else {
            console.warn('[StreamView] AudioPipeline init failed — audio will be silent');
        }
    }

    /**
     * Handle an incoming Opus audio packet from the transport (DataChannel /
     * media-DC / WSS). Decoding to PCM happens inside AudioPipeline (WebCodecs).
     * @param {Uint8Array} sample - One raw Opus packet.
     */
    handleAudioSample(sample) {
        if (this._quitting) return;

        if (!this._audioLogged) {
            console.log(
                '[StreamView] Audio sample received, size=' +
                    sample.length +
                    ', writing to pipeline...',
            );
            this._audioLogged = true;
        }

        // Write to AudioPipeline (init may not be complete yet — safe to drop)
        // Standby: swallow samples — the live view owns the speakers until
        // promote (AudioPipeline has no gain control; not feeding it mutes it).
        if (this.audioPipeline && this.audioPipeline.ready && !this._standby) {
            this.audioPipeline.write(sample);
        }
    }

    // =========================================================================
    // DOM rendering
    // =========================================================================

    render() {
        // Remove any stale #stream-view left over from a previous session whose
        // teardown hasn't completed (e.g. relaunch/resume while the prior view is
        // still tearing down). Two containers would each carry their own
        // #stream-startup-overlay, and getElementById('stream-view') below would
        // resolve to the OLD one — leaving a step-2-stuck overlay visible on top
        // while the live overlay (this._startupOverlay) advances unseen.
        // A STANDBY view must NOT sweep: the live view it will replace is still
        // playing — the two coexist until promote.
        if (!this._standby) {
            document.querySelectorAll('#stream-view').forEach((stale) => stale.remove());
        }

        const el = document.createElement('div');
        el.id = 'stream-view';
        el.className = 'stream-overlay';
        // Every element lookup below is scoped to this root (el.querySelector),
        // so a hidden standby view resolves its OWN children even while the
        // live view's identically-id'd elements are still in the document.
        this._rootEl = el;
        if (this._standby) {
            // Hidden, not display:none — decode/render must keep presenting so
            // the promote can swap on an already-flowing surface.
            el.style.visibility = 'hidden';
        }
        el.innerHTML = `
            <div class="stream-header">
                <button class="btn stream-quit-btn" id="btn-stream-quit">${IS_MOBILE_OR_TABLET ? t('stream.stop') : t('stream.stopStreaming')}</button>
            </div>
            <div class="stream-canvas-area">
                <canvas id="stream-canvas" class="stream-canvas"></canvas>
                <video id="stream-video" class="stream-video" autoplay muted playsinline></video>
                <audio id="stream-audio" autoplay playsinline></audio>
                <div id="stream-input-layer" class="stream-input-layer"></div>
                <div class="stream-click-hint" id="stream-hint">
                    ${t('stream.clickToCapture')}
                </div>
            </div>
        `;
        document.getElementById('app').appendChild(el);

        // Stream owns the screen: hide the underlying app (header/content/footer)
        // so zoom/pan or the iOS keyboard can never reveal it behind the stream.
        document.body.classList.add('streaming-active');

        // Keep the device awake while streaming (prevents iPhone screen lock /
        // PC sleep after the idle timeout). Re-acquired on visibility change.
        // Standby: the live view already holds one — acquired on activate().
        if (!this._standby) this._acquireWakeLock();

        // Whole-surface input element: touch events are captured on the full
        // overlay (trackpad model), not just the canvas/video rectangle.
        this.streamEl = el;

        this.canvasArea = el.querySelector('.stream-canvas-area');
        this.canvas = el.querySelector('#stream-canvas');
        // Default GPU-accelerated 2D context — matches Mac Chrome behavior.
        // Mac Chrome handles NV12→RGBA correctly via Metal; Windows Chrome
        // should do the same via D3D11.
        // The Canvas2DRenderer owns the context now — it is created later, once
        // platform detection (_isChromeWindowsHevc) has run: the main-thread
        // renderer in setupWebRtc.onOpen, the worker renderer in _initVideoWorker.
        // render() must NOT touch the canvas here: a 2D context or a size set on
        // it would prevent transferControlToOffscreen() in worker mode.
        // Prevent browser default touch behaviors (scroll, zoom, pull-to-refresh)
        // on the WHOLE overlay — touches must never scroll/move <video>/<canvas>.
        this.streamEl.style.touchAction = 'none';
        this.streamEl.style.overscrollBehavior = 'none';
        this.canvas.style.touchAction = 'none';
        // Track the DOM output size for the renderer (WebGPU scales to it; the
        // worker has no DOM, so the size is forwarded as a 'resize' message).
        this._setupOutputSizeObserver();
        // Video element for native RTP media track mode (webrtc-media)
        this.videoEl = el.querySelector('#stream-video');
        // The video never receives pointer/touch events directly: input is
        // handled by the overlay (touch) and canvas (mouse). This also blocks
        // iOS gestures that would drag or zoom the <video> element.
        this.videoEl.style.touchAction = 'none';
        this.videoEl.style.pointerEvents = 'none';
        if (this._transport === 'webrtc-media') {
            this.canvas.style.display = 'none';
            this.videoEl.style.display = 'block';
            // Minimize playout delay for real-time streaming
            if ('playoutDelayHint' in this.videoEl) {
                this.videoEl.playoutDelayHint = 0;
            }
            // Pass video element to WebRtcMedia for media track rendering
            if (this.webrtc && typeof this.webrtc.setVideoElement === 'function') {
                this.webrtc.setVideoElement(this.videoEl);
            }
        }

        // Native RTP Opus audio track target (webrtc / webrtc-media). The browser
        // decodes Opus (jitter buffer + FEC + PLC) and plays it through this
        // <audio> element. Wired before setupWebRtc() → connect() so ontrack finds
        // it. Unused on WSS (which decodes via the AudioPipeline).
        this.audioEl = el.querySelector('#stream-audio');
        if (this.audioEl) {
            this.audioEl.style.display = 'none';
            // Standby: muted until promote (no double audio with the live view).
            if (this._standby) this.audioEl.muted = true;
            if (this._nativeAudio && this.webrtc) this.webrtc.audioElement = this.audioEl;
        }

        // Transparent input-capture layer covering the whole canvas/video area.
        // All mouse/wheel events bind here (not the canvas) so input works in
        // every transport: in webrtc-media the canvas is hidden and the <video>
        // has pointer-events:none, so without this layer the mouse was dead.
        // Sitting on top of <video> also fixes iOS Safari swallowing touches
        // over the video element.
        this.inputEl = el.querySelector('#stream-input-layer');
        this.inputEl.style.touchAction = 'none';

        // statusEl kept for backward compatibility — setStatus() is now a no-op
        this.statusEl = null;
        this.hintEl = el.querySelector('#stream-hint');

        el.querySelector('#btn-stream-quit').onclick = () => this._handleManualQuit();

        // ── Streaming stats overlay (top-center card, elegant styling) ─────
        this._overlayEl = document.createElement('div');
        this._overlayEl.id = 'stream-stats-overlay';
        this._overlayEl.className = 'stream-stats-overlay';
        this._overlayEl.innerHTML =
            '<div class="stats-waiting">' + t('stream.connecting') + '</div>';
        this._rootEl.appendChild(this._overlayEl);

        // The stats card can sit over the game; let the user drag it out of the
        // way. Position is intentionally not persisted so it resets to the
        // top-left default on every new streaming session.
        this._makeStatsDraggable(this._overlayEl);
        // × hides the stats card for the rest of the session.
        this._overlayEl.addEventListener('click', (e) => {
            if (!e.target.closest('.overlay-close-btn')) return;
            e.stopPropagation();
            e.preventDefault();
            this._closeOverlayEl(this._overlayEl);
        });

        // ── Immersive-mode exit reminder (top-center, draggable) ───────────
        // Discreet card that only appears once immersive mode has captured the
        // mouse. It reminds the single combo that frees the cursor, releases
        // the full keyboard lock and leaves fullscreen. Touch devices never
        // use immersive mode, so it is never built there.
        if (!IS_TOUCH_DEVICE) {
            this._immersiveOverlay = document.createElement('div');
            this._immersiveOverlay.id = 'stream-immersive-overlay';
            this._immersiveOverlay.className = 'stream-immersive-overlay';
            this._buildImmersiveOverlayContent();
            this._rootEl.appendChild(this._immersiveOverlay);
            this._makeStatsDraggable(this._immersiveOverlay);
            // × hides the reminder for the rest of the session.
            this._immersiveOverlay.addEventListener('click', (e) => {
                if (!e.target.closest('.overlay-close-btn')) return;
                e.stopPropagation();
                e.preventDefault();
                this._closeOverlayEl(this._immersiveOverlay);
            });
        }

        // ── Cyberpunk "signal acquired" reveal ─────────────────────────────
        // Full-screen one-shot boot animation played the instant the first
        // frame arrives (see _playStreamReveal, triggered from step 3): a cyan
        // scan-beam sweep + RGB-glitch + HUD corner brackets that converge,
        // "materializing" the streamed screen. pointer-events:none so Stop
        // stays clickable; removes itself after ~1s.
        this._revealEl = document.createElement('div');
        this._revealEl.id = 'stream-reveal';
        this._revealEl.className = 'stream-reveal';
        this._revealEl.setAttribute('aria-hidden', 'true');
        this._revealEl.innerHTML =
            '<div class="reveal-grid"></div>' +
            '<div class="reveal-beam"></div>' +
            '<div class="reveal-glitch"></div>' +
            '<div class="reveal-brackets">' +
            '<span class="rb tl"></span><span class="rb tr"></span>' +
            '<span class="rb bl"></span><span class="rb br"></span>' +
            '</div>' +
            '<div class="reveal-text">' +
            t('stream.signalAcquired') +
            '</div>';
        this._revealEl.style.display = 'none';
        // Append to the canvas area (not the whole overlay) so the reveal only
        // covers the streamed image, leaving the header/letterbox untouched.
        this.canvasArea.appendChild(this._revealEl);

        // Start overlay update timer (every 500ms)
        this._overlayInterval = setInterval(() => this._updateOverlay(), 500);

        // ── Keyboard shortcuts slide ────────────────────────────────────────
        this._shortcutsSlide = document.createElement('div');
        this._shortcutsSlide.id = 'stream-shortcuts-slide';
        this._shortcutsSlide.className = 'stream-shortcuts-slide';
        this._shortcutsSlide.style.display = 'none';
        this._buildShortcutsSlideContent();
        // Click/tap to dismiss the shortcuts/gesture hint immediately.
        // Use pointerdown/touchstart with stopPropagation: a plain 'click'
        // never fires on touch because handleTouchStart() preventDefaults the
        // bubbled touchstart on streamEl, suppressing the synthetic click.
        const dismiss = (e) => {
            e.stopPropagation();
            e.preventDefault();
            this._hideShortcutsSlide();
        };
        this._shortcutsSlide.addEventListener('pointerdown', dismiss);
        this._shortcutsSlide.addEventListener('touchstart', dismiss, { passive: false });
        this._rootEl.appendChild(this._shortcutsSlide);

        // ── Startup overlay (centered 3-step status) ───────────────────────
        this._startupOverlay = document.createElement('div');
        this._startupOverlay.id = 'stream-startup-overlay';
        this._startupOverlay.className = 'stream-startup-overlay';
        this._startupOverlay.innerHTML = [
            '<div class="startup-loader" aria-hidden="true">',
            '  <div class="startup-loader-ring"></div>',
            '</div>',
            '<div class="startup-step active" data-step="1">',
            '  <span class="startup-step-dot"></span>',
            '  <span class="startup-step-label">' + t('stream.connecting') + '</span>',
            '</div>',
            '<div class="startup-step" data-step="2">',
            '  <span class="startup-step-dot"></span>',
            '  <span class="startup-step-label">' + t('stream.startingVideo') + '</span>',
            '</div>',
            '<div class="startup-step" data-step="3">',
            '  <span class="startup-step-dot"></span>',
            '  <span class="startup-step-label">' + t('stream.streamReady') + '</span>',
            '</div>',
        ].join('');
        this._rootEl.appendChild(this._startupOverlay);

        // ── Header fullscreen button (desktop: always; mobile: landscape) ──
        this._mobileFsBtn = document.createElement('button');
        this._mobileFsBtn.id = 'btn-stream-fs';
        this._mobileFsBtn.className = 'btn-stream-fs';
        // In immersive (gaming) mode, advertise the fullscreen toggle combo right
        // on the button — mirrors the immersive exit reminder (Z combo). The
        // button is only ever shown out of fullscreen, where the header is visible.
        this._mobileFsBtn.innerHTML =
            Icons.fullscreen +
            '<span class="fs-label">' +
            t('stream.fullscreen') +
            '</span>' +
            (this._gamingMode ? this._fsComboKeysHtml() : '');
        this._mobileFsBtn.title = t('stream.enterFullscreen');
        this._mobileFsBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            // Same behaviour as the keyboard combo: toggle real browser
            // fullscreen of the stream (header hidden via :fullscreen CSS).
            this.toggleFullscreen();
        });
        const header = document.querySelector('.stream-header');
        if (header) {
            // Insert before the quit button (first child)
            header.insertBefore(this._mobileFsBtn, header.firstChild);
        }
        // Initial state: desktop = visible, mobile = landscape only
        this._updateMobileFsButtonVisibility();

        // ── Virtual keyboard (touch devices only) ──────────────────────────
        if (IS_TOUCH_DEVICE) {
            // Hidden capture element: focusing it opens the OS soft keyboard.
            // Key/text events are read here and forwarded to the host.
            // contenteditable (not <textarea>): a real form field makes iOS
            // Safari show its AutoFill accessory bar (passwords/cards/keyboard
            // switcher). A contenteditable element opens the soft keyboard with
            // none of that bar.
            this._kbdCapture = document.createElement('div');
            this._kbdCapture.id = 'stream-kbd-capture';
            this._kbdCapture.className = 'stream-kbd-capture';
            this._kbdCapture.setAttribute('contenteditable', 'true');
            this._kbdCapture.setAttribute('inputmode', 'text');
            this._kbdCapture.setAttribute('autocorrect', 'off');
            this._kbdCapture.setAttribute('autocapitalize', 'off');
            this._kbdCapture.setAttribute('spellcheck', 'false');
            this._kbdCapture.setAttribute('aria-hidden', 'true');
            // Out of the tab order: removes iOS's prev/next field-navigation
            // chevrons from the keyboard accessory bar (still programmatically
            // focusable via .focus()).
            this._kbdCapture.setAttribute('tabindex', '-1');
            this._rootEl.appendChild(this._kbdCapture);
            this._setupKeyboardCapture();

            // Header toggle button: keyboard glyph + tiny up arrow.
            this._kbdBtn = document.createElement('button');
            this._kbdBtn.id = 'btn-stream-keyboard';
            this._kbdBtn.className = 'btn-stream-kbd';
            this._kbdBtn.innerHTML = Icons.keyboard;
            this._kbdBtn.title = t('stream.showKeyboard');
            this._kbdBtn.addEventListener('click', (e) => {
                e.stopPropagation();
                this._toggleVirtualKeyboard();
            });
            if (header) header.insertBefore(this._kbdBtn, header.firstChild);

            // Special-keys bar shown above the soft keyboard (Win/Esc/Tab/
            // modifiers/Del + arrows). Latching modifiers carry into the next key.
            this._buildKbToolbar();

            // VisualViewport: shrink the overlay so the stream stays fully
            // visible above the soft keyboard (push-up, esp. portrait).
            if (window.visualViewport) {
                this._onViewportResize = () => this._handleViewportResize();
                window.visualViewport.addEventListener('resize', this._onViewportResize);
                window.visualViewport.addEventListener('scroll', this._onViewportResize);
            }

            // Hard scroll lock: the stream view never scrolls — any document
            // scroll (iOS auto-scroll to a focused field, rubber-banding) is
            // snapped straight back to the top.
            this._onWindowScroll = () => {
                if (window.scrollX || window.scrollY) window.scrollTo(0, 0);
            };
            window.addEventListener('scroll', this._onWindowScroll, { passive: true });

            // Keep the soft keyboard open: while it is visible, prevent any tap
            // outside the capture (stream area, dark band under the resized
            // overlay, toolbar gaps/keys) from blurring the contenteditable.
            // Header controls (Stop / Fullscreen / Keyboard toggle) are excluded
            // so they still work. Capture phase + passive:false so preventDefault
            // runs before the browser moves focus, without stopping propagation
            // (the trackpad touch handlers still run).
            this._onDocKeepFocus = (e) => {
                if (!this._kbdVisible) return;
                const t = e.target;
                if (t === this._kbdCapture) return;
                if (t && t.closest && t.closest('.stream-header')) return;
                e.preventDefault();
            };
            document.addEventListener('touchstart', this._onDocKeepFocus, {
                capture: true,
                passive: false,
            });
            document.addEventListener('mousedown', this._onDocKeepFocus, { capture: true });
        }
    }

    // =========================================================================
    // WebCodecs VideoDecoder
    // =========================================================================

    // ── OffscreenCanvas video worker (opt-in) ───────────────────────────────
    // Create the worker, transfer the canvas, and start the decode+render
    // pipeline off the main thread. Called from setupWebRtc once platform
    // detection (_isChromeWindowsHevc) has run. Falls back to the main-thread
    // path if the Worker cannot be created (before the canvas is transferred).
    _initVideoWorker() {
        try {
            const worker = new Worker(new URL('../stream/VideoDecodeWorker.js', import.meta.url), {
                type: 'module',
            });
            // Transfer only AFTER the worker exists, so a construction failure
            // leaves the canvas intact for the main-thread fallback below.
            const offscreen = this.canvas.transferControlToOffscreen();
            worker.onmessage = (e) => this._onWorkerMessage(e.data);
            worker.onerror = (err) => {
                console.error('[StreamView] Video worker error:', err.message || err);
            };
            worker.postMessage(
                {
                    type: 'init',
                    canvas: offscreen,
                    videoCodec: this.videoCodec,
                    isChromeWindowsHevc: this._isChromeWindowsHevc,
                    transport: this._transport,
                    tearing: this._tearing,
                    webgpu: this._wantWebGpu,
                    algo: this._videoEnhancementAlgo,
                    hdr: this._hdrEnabled,
                },
                [offscreen],
            );
            this._videoWorker = worker;
            // Forward the measured output size (worker has no DOM / ResizeObserver).
            this._applyOutputSize();
            console.log('[StreamView] Video worker started (OffscreenCanvas)');
        } catch (e) {
            console.error('[StreamView] Video worker init failed, using main thread:', e.message);
            this._useWorker = false;
            this._videoWorker = null;
            // Recover the main-thread render path (canvas not transferred yet).
            createVideoRenderer(this.canvas, {
                desynchronized: this._tearing,
                videoCodec: this.videoCodec,
                isChromeWindowsHevc: this._isChromeWindowsHevc,
                webgpu: this._wantWebGpu,
                algo: this._videoEnhancementAlgo,
                hdr: this._hdrEnabled && !this._hdrTonemap,
                hdrTonemap: this._hdrTonemap,
                videoEl: this._useVideoSink ? this.videoEl : null,
            }).then((r) => {
                this._renderer = r;
                this._activeRendererKind = r.kind;
                this._rendererHdrActive = !!r.hdrActive;
                this._applyRendererSink(r);
                this._applyOutputSize();
                this.setupDecoder();
                this.startRenderLoop();
            });
        }
    }

    // Handle messages from the video worker (mirrors the main-thread side
    // effects that must stay on the UI thread: IDR requests, codec fallback,
    // status/overlay updates).
    _onWorkerMessage(m) {
        switch (m.type) {
            case 'requestidr':
                this._requestIdr(m.reason || 'worker');
                break;
            case 'rendererinfo':
                // Worker reports which renderer it created (for the stats overlay).
                this._activeRendererKind = m.kind;
                this._rendererHdrActive = !!m.hdr;
                break;
            case 'codecfallback':
                // HEVC unsupported in this browser → re-launch with H.264.
                this._handleHevcFallback();
                break;
            case 'firstframe':
                console.log(
                    '[StreamView] firstframe received from worker, ' +
                        'alreadyRendered=' +
                        !!this._firstFrameRendered +
                        ' resolution=' +
                        (m.resolution || '?'),
                );
                if (m.resolution) this._resolution = m.resolution;
                // Always run _markFirstFrame, even if a previous path already
                // flipped _firstFrameRendered: the overlay teardown inside is
                // idempotent and must never be skipped, otherwise it stays
                // stuck on step 2 while the stream plays.
                this._markFirstFrame();
                break;
            case 'status':
                this.setStatus(m.state, m.msg);
                break;
            case 'counters': {
                // Approximate decode fps: push one timestamp per newly decoded frame
                // into the sliding window the overlay already uses.
                const dDecoded = m.decoded - this._workerLastDecoded;
                this._workerLastDecoded = m.decoded;
                const now = performance.now();
                for (let i = 0; i < dDecoded && i < 240; i++) this._fpsTimestamps.push(now);
                this.stats.received = m.received;
                this.stats.decoded = m.decoded;
                this.stats.rendered = m.rendered;
                this.stats.dropped = m.dropped;
                if (m.resolution && m.resolution !== this._resolution)
                    this._resolution = m.resolution;
                // Client pipeline leg measured inside the worker (its own clock).
                if (m.clientMs > 0) this._clientLatencyStats.addSample(m.clientMs);
                break;
            }
            case 'fatal':
                this._fatalDecodeError = true;
                this.setStatus('error', m.msg);
                break;
        }
    }

    setupDecoder() {
        if (this.decoder) {
            console.log('[StreamView] Closing existing decoder');
            try {
                this.decoder.close();
            } catch (e) {}
            this.decoder = null;
        }
        this.decoderConfigured = false;
        this.decoderConfiguring = false;

        console.log('[StreamView] Creating new VideoDecoder');
        this.decoder = new VideoDecoder({
            output: (frame) => {
                // Log only the first decoded frame: per-frame logging floods
                // the console at 60fps and costs CPU on mobile.
                if (!this._firstDecoderOutputLogged) {
                    this._firstDecoderOutputLogged = true;
                    console.log(
                        '[StreamView] First decoded frame: ' +
                            (frame.displayWidth || frame.codedWidth) +
                            'x' +
                            (frame.displayHeight || frame.codedHeight) +
                            ' format=' +
                            (frame.format || 'null'),
                    );
                }
                this.onDecodedFrame(frame);
            },
            error: (err) => {
                console.error('[StreamView] VideoDecoder error:', err.message, err);
                if (err.code) console.error('[StreamView] Error code:', err.code);
                this._handleDecoderError(err);
            },
        });
        console.log('[StreamView] VideoDecoder created, state=' + this.decoder.state);
    }

    /**
     * Recover from a VideoDecoder error by creating a new decoder and
     * requesting an IDR (keyframe) from the backend.
     *
     * The VideoDecoder is single-use: once it enters 'closed' state due to
     * a decoding error, it cannot be re-opened.  We must:
     *   1. Close the broken decoder.
     *   2. Create a new VideoDecoder via setupDecoder().
     *   3. Reset the NAL parser so SPS/PPS are re-extracted from the next
     *      keyframe.
     *   4. Clear pending frames — they reference corrupted/lost reference
     *      frames and cannot be decoded by the new decoder.
     *   5. Request an IDR from the backend so Sunshine sends a fresh keyframe.
     *
     * After recovery, the next keyframe triggers configureDecoder() which
     * reconfigures the new decoder and resumes normal playback.
     */
    _handleDecoderError(err) {
        // Guard: prevent recovery during quit (or the manual-quit animation)
        if (this._quitting || this._manualQuitting) return;
        // Guard: prevent re-entrant recovery (error callback may fire during
        // setupDecoder(), which would loop back to this method)
        if (this._decoderRecovering) return;
        // Guard: limit total recovery attempts to avoid infinite loops on
        // a fundamentally broken connection
        this._recoveryAttempts++;

        // Decode capability failure: the config passed isConfigSupported() but the
        // browser cannot actually decode it (e.g. Android Chrome with HEVC Main10
        // HDR — hardware advertises support but decoding errors every frame). If no
        // frame ever decoded after a few attempts and a fallback step exists, switch
        // codec/HDR instead of looping recovery until exhaustion. Checked BEFORE the
        // AV1 dead-end below so AV1 HDR can still fall back to HEVC SDR.
        if (this.stats.decoded === 0 && this._recoveryAttempts >= 3) {
            const target = this._computeCodecFallbackTarget();
            if (target) {
                console.warn(
                    '[StreamView] Decoder never produced a frame after ' +
                        this._recoveryAttempts +
                        ' attempts — triggering codec fallback',
                );
                this._requestCodecFallback();
                return;
            }
        }

        // AV1 decoder that NEVER produced a frame: the browser's AV1 decode is
        // broken despite isConfigSupported=true (WebKit bug in early Safari 18).
        // Retrying forever just spams IDR requests — fail with a clear message.
        // Reached only when no fallback step remains (e.g. AV1 SDR last resort).
        if (this.videoCodec === 'av1' && this.stats.decoded === 0 && this._recoveryAttempts >= 3) {
            console.error(
                '[StreamView] AV1 decoder never produced a frame after ' +
                    this._recoveryAttempts +
                    ' attempts — AV1 decoding is broken in this browser',
            );
            if (this._reportConnectionFailed('decoder_unsupported')) return;
            this._fatalDecodeError = true;
            if (this.decoder) {
                try {
                    this.decoder.close();
                } catch (e) {
                    /* ignore */
                }
                this.decoder = null;
            }
            this.decoderConfigured = false;
            this.pendingFrames = [];
            this.setStatus(
                'error',
                'AV1 decoding failed in this browser — select H.264 or HEVC in Settings',
            );
            return;
        }

        if (this._recoveryAttempts > this.MAX_RECOVERY_ATTEMPTS) {
            console.error(
                '[StreamView] Max recovery attempts (' +
                    this.MAX_RECOVERY_ATTEMPTS +
                    ') reached, giving up',
            );
            // Never decoded a single frame → WebCodecs is unusable here; a
            // native media transport may still work — advance the chain.
            if (this.stats.decoded === 0 && this._reportConnectionFailed('decoder_unsupported'))
                return;
            this.setStatus('error', 'Max recovery attempts exceeded');
            return;
        }
        this._decoderRecovering = true;

        console.warn(
            '[StreamView] Starting decoder recovery (' +
                this._recoveryAttempts +
                '/' +
                this.MAX_RECOVERY_ATTEMPTS +
                ') from: ' +
                (err ? err.message : 'unknown'),
        );

        // 1. Close the broken decoder
        if (this.decoder) {
            try {
                this.decoder.close();
            } catch (e) {
                /* ignore */
            }
            this.decoder = null;
        }
        this.decoderConfigured = false;
        this.decoderConfiguring = false;
        // Decoder error invalidates the reference: drop deltas until next keyframe.
        this._referenceValid = false;

        // 2. Clear frame queue — frames output by the old decoder are invalid
        //    once the decoder is closed
        for (const frame of this.frameQueue) {
            try {
                frame.close();
            } catch (e) {
                /* ignore */
            }
        }
        this.frameQueue = [];

        // 3. Clear pending frames — they reference lost/corrupted reference
        //    frames and would produce more errors on the new decoder
        this.pendingFrames = [];

        // 4. Reset NAL parser to force re-extraction of SPS/PPS from the
        //    next keyframe
        this.nalParser.reset();

        // 5. Create a new decoder
        this.setupDecoder();

        // 6. Request an IDR so Sunshine sends a clean keyframe
        this._requestIdr('decoder error');

        this.setStatus('connecting', 'Recovering...');

        this._decoderRecovering = false;
    }

    // Request an IDR from Sunshine via the backend. Level-triggered with a
    // 1s retry: a one-shot flag deadlocks the stream forever if the recovery
    // keyframe is lost or the backend 300ms throttle swallows a request
    // (frozen-stream bug on mobile WSS).
    _requestIdr(reason) {
        if (!this.webrtc) return;
        const now = performance.now();
        if (this._idrRequested && now - this._lastIdrRequestMs < 1000) return;
        this._idrRequested = true;
        this._lastIdrRequestMs = now;
        console.log('[StreamView] Requesting IDR (' + reason + ')');
        this.webrtc.send({ type: 'requestidr' });
        this._recordCongestionEvent('sv:' + reason);
    }

    /**
     * Congestion monitor. Records one congestion signal — an IDR request
     * actually sent, a batch of backend backpressure drops, or receiver PLIs —
     * and fires onCongestion() when signals are SUSTAINED: ≥5 events within a
     * 20s window. The first 10s of the stream are ignored (startup recovery
     * traffic), and a 30s cooldown separates two detections so the relaunched
     * degraded stream has time to prove itself.
     */
    _recordCongestionEvent(reason) {
        if (!this.onCongestion || this._quitting || this._manualQuitting) return;
        const now = performance.now();
        if (now - this._startTime < 10000) return;
        // Every recorded signal (not just sustained detections) is reported so
        // the app can measure quiet time for the automatic quality upgrade.
        if (typeof this.onCongestionSignal === 'function') this.onCongestionSignal();
        this._congEvents.push(now);
        while (this._congEvents.length > 0 && now - this._congEvents[0] > 20000) {
            this._congEvents.shift();
        }
        if (this._congEvents.length >= 5 && now - this._congFiredAt > 30000) {
            this._congFiredAt = now;
            this._congEvents.length = 0;
            console.warn('[StreamView] Sustained congestion detected (last: ' + reason + ')');
            this.onCongestion();
        }
    }

    configureDecoder() {
        if (this._quitting) return;
        if (this.decoderConfigured || this.decoderConfiguring || !this.nalParser.isReady()) {
            return;
        }
        this.decoderConfiguring = true;
        const codecType = this.nalParser.codec;

        const desc = buildDescription(this.nalParser);
        if (!desc) {
            console.warn('[StreamView] Failed to build codec description');
            this.decoderConfiguring = false;
            return;
        }

        const codec = getCodecString(this.nalParser);
        if (!codec) {
            console.error('[StreamView] Could not determine codec string');
            this.decoderConfiguring = false;
            this.setStatus('error', 'Unknown codec');
            return;
        }

        // Detect HDR: true if the codec string indicates a 10-bit profile.
        // HEVC: hvc1.2.x = Main10 (HDR), AV1: av01.x.x.10 = 10-bit (HDR).
        const isHdr = isHevcHdrProfile(codec) || isAv1HdrProfile(codec);
        this._hdrNegotiated = isHdr;

        console.log(
            '[StreamView] Configuring VideoDecoder: codec=' +
                codec +
                ' descLen=' +
                desc.length +
                ' codecType=' +
                this.nalParser.codec +
                ' hdr=' +
                isHdr,
        );

        if (!VideoDecoder.isConfigSupported) {
            console.error('[StreamView] WebCodecs VideoDecoder not available');
            this.decoderConfiguring = false;
            this.setStatus('error', 'WebCodecs not supported');
            return;
        }

        const applyConfig = (cfg, noDescription = false) => {
            // Try hardware-accelerated decoder first (GPU decoding on Android)
            const _doConfigure = (config, hwAccel) => {
                let cfgToUse;
                if (this._hdrTonemap) {
                    // HDR→SDR tone-map + FSR1 needs CPU-readable frames: hardware
                    // decoders output opaque frames (format=null) on which
                    // VideoFrame.copyTo/allocationSize are unsupported. Force a
                    // software decoder so the YUV planes can be read back.
                    cfgToUse = { ...config, hardwareAcceleration: 'prefer-software' };
                } else if (hwAccel) {
                    cfgToUse = { ...config, hardwareAcceleration: 'prefer-hardware' };
                } else {
                    cfgToUse = config;
                }
                try {
                    this.decoder.configure(cfgToUse);
                    this.decoderConfigured = true;
                    this.decoderConfiguring = false;
                    this._noDescription = noDescription;
                    console.log(
                        '[StreamView] VideoDecoder configured: codec=' +
                            cfgToUse.codec +
                            ' hwAccel=' +
                            (cfgToUse.hardwareAcceleration || 'none') +
                            ', dequeuing ' +
                            this.pendingFrames.length +
                            ' pending frames',
                    );
                    this.flushPendingFrames();

                    // Proactive IDR after initial decoder configuration.
                    // The first decoded frame from a freshly configured decoder may
                    // contain green/corrupted pixels because the decoder's internal
                    // buffers (reference frames, MV predictors) are uninitialized.
                    // We request an IDR ~250ms after config so Sunshine sends a clean
                    // keyframe that overwrites any corrupted first-frame output.
                    if (!this._proactiveIdrScheduled) {
                        this._proactiveIdrScheduled = true;
                        setTimeout(() => {
                            if (!this._quitting && this.webrtc) {
                                console.log(
                                    '[StreamView] Proactive IDR after initial decoder config',
                                );
                                this.webrtc.send({ type: 'requestidr' });
                            }
                        }, 250);
                    }

                    return true;
                } catch (e) {
                    console.error('[StreamView] decoder.configure() failed:', e.message, e);
                    this.decoderConfiguring = false;
                    // Re-throw NotSupportedError to trigger HW fallback
                    if (hwAccel && e.name === 'NotSupportedError') {
                        throw e;
                    }
                    return false;
                }
            };

            try {
                // Phase 1: try with hardware acceleration
                return _doConfigure(cfg, true);
            } catch (hwErr) {
                // Phase 2: fallback to software when prefer-hardware is not supported
                console.warn(
                    '[GPU] prefer-hardware rejected (' +
                        hwErr.message +
                        '), falling back to software',
                );
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
            // Probe what _doConfigure will actually configure: the tone-map path
            // forces prefer-software, and e.g. Chrome has no software HEVC — the
            // plain config would probe "supported", then configure() would fail
            // asynchronously (3 decoder errors before the codec fallback kicks in).
            const probeCfg = this._hdrTonemap
                ? { ...cfg, hardwareAcceleration: 'prefer-software' }
                : cfg;
            VideoDecoder.isConfigSupported(probeCfg)
                .then((result) => {
                    if (result.supported) {
                        if (!applyConfig(cfg, noDescription)) {
                            tryCodecs(configs, index + 1, onExhausted);
                        }
                    } else {
                        console.warn(
                            '[StreamView] Config NOT supported: codec=' +
                                cfg.codec +
                                ', trying next',
                        );
                        tryCodecs(configs, index + 1, onExhausted);
                    }
                })
                .catch((err) => {
                    console.error(
                        '[StreamView] isConfigSupported error for codec=' + cfg.codec + ':',
                        err.message,
                        err,
                    );
                    tryCodecs(configs, index + 1, onExhausted);
                });
        };

        // Shared config fields
        const shared = {
            codedWidth: 1920,
            codedHeight: 1080,
            optimizeForLatency: true,
        };

        // Decoder color space: HDR (BT.2020 + PQ) or SDR (BT.709).
        // HDR is detected from the codec string (HEVC Main10 / AV1 10-bit).
        // HDR content uses ST 2084 (PQ) transfer, BT.2020 color primaries,
        // and either YCbCr (matrix: 'bt2020ncl') or RGB (full range).
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

        // Build configs: for HEVC, try Annex B (no description) first.
        // Chromium's keyframe validator (AnalyzeAnnexB) only parses start-code
        // format — without Annex B, HEVC keyframes are falsely rejected on
        // Chrome/Edge.  Non-Chromium browsers fall back to AVCC + description.
        const configsToTry = [];
        let fallbacks =
            codecType === CODEC_HEVC ? HEVC_FALLBACK_CODEC_STRINGS : H264_FALLBACK_CODEC_STRINGS;
        // HDR (10-bit) stream: never fall back to an 8-bit Main profile codec
        // string. An 8-bit decoder configures fine for a 10-bit bitstream but then
        // throws "Decoding error" on every frame (observed on Android Chrome, which
        // has no HEVC Main10 WebCodecs support) → infinite recovery spiral. Keep only
        // Main10 profiles; if none are supported, the chain exhausts and degrades
        // gracefully to an H.264 re-launch via _handleHevcFallback().
        let annexbStrings = HEVC_ANNEXB_CODEC_STRINGS;
        if (isHdr && codecType === CODEC_HEVC) {
            fallbacks = fallbacks.filter((c) => isHevcHdrProfile(c));
            annexbStrings = annexbStrings.filter((c) => isHevcHdrProfile(c));
        }
        // Chrome WebCodecs has historically rejected HEVC configs that include
        // an explicit colorSpace. We attempt it as the primary config for HEVC
        // too; if Chrome rejects it, the fallback chain skips to the next
        // config (without colorSpace) automatically.
        const colorConfig = {
            codec: codec,
            description: desc.buffer,
            ...shared,
            ...vColor,
        };

        // ── HEVC Annex B phase (no description) ──
        // Chromium keyframe validator only handles start codes.  Try Annex B
        // configs first; if all fail, fall back to AVCC with description.
        if (codecType === CODEC_HEVC) {
            const annexBCfgs = [];
            const hev1Primary = codec.replace(/^hvc1/, 'hev1');
            annexBCfgs.push({ codec: hev1Primary, ...shared, ...vColor, _noDescription: true });
            annexBCfgs.push({ codec: hev1Primary, ...shared, _noDescription: true });
            for (const fb of annexbStrings) {
                if (fb === hev1Primary) continue;
                annexBCfgs.push({ codec: fb, ...shared, ...vColor, _noDescription: true });
                annexBCfgs.push({ codec: fb, ...shared, _noDescription: true });
            }

            tryCodecs(annexBCfgs, 0, () => {
                this._tryHevcAvccConfigs(codec, desc, fallbacks, shared, vColor);
            });
            return;
        }

        // H.264 path: AVCC with description (no Annex B needed)
        configsToTry.push(colorConfig);

        for (const fbCodec of fallbacks) {
            if (fbCodec === codec) continue;
            // With colorSpace (works on Chrome desktop)
            configsToTry.push({
                codec: fbCodec,
                description: desc.buffer,
                codedWidth: 1920,
                codedHeight: 1080,
                optimizeForLatency: true,
                ...vColor,
            });
            // Without colorSpace (Safari iOS does not support colorSpace in configure())
            configsToTry.push({
                codec: fbCodec,
                description: desc.buffer,
                codedWidth: 1920,
                codedHeight: 1080,
                optimizeForLatency: true,
            });
        }

        // ── Additional fallback variants for H.264 ─────────────────────────
        // Safari iOS (WebKit) may reject configs with optimizeForLatency (Chrome
        // proprietary extension) or with explicit colorSpace (BT.709 in our configs).
        // These variants strip one variable at a time, mirroring the HEVC fallback
        // chain above.
        if (codecType === CODEC_H264) {
            // Variant A: no optimizeForLatency (not supported by Safari WebCodecs)
            configsToTry.push({
                codec: codec,
                description: desc.buffer,
                codedWidth: 1920,
                codedHeight: 1080,
            });
            // Variant B: no optimizeForLatency, no codedWidth/codedHeight
            configsToTry.push({
                codec: codec,
                description: desc.buffer,
            });
            // Variant C: use Uint8Array directly (some Safari versions reject
            // ArrayBuffer-based description)
            configsToTry.push({
                codec: codec,
                description: desc,
                codedWidth: 1920,
                codedHeight: 1080,
                optimizeForLatency: true,
            });
            // Last resort: bare codec string, no description at all.
            // Safari iOS may auto-detect the H.264 config from the bitstream.
            configsToTry.push({
                codec: codec,
                optimizeForLatency: true,
            });
            configsToTry.push({
                codec: codec,
            });
        }

        tryCodecs(configsToTry, 0, () => {
            // All avc1 configs exhausted — try avc3 (in-band SPS/PPS).
            // Some browsers/devices prefer avc3 over avc1 for hardware decoding.
            console.warn('[GPU] All avc1 configs rejected, trying avc3 (in-band SPS/PPS)');
            const avc3Configs = [
                { codec: 'avc3.42E01E', ...shared, ...vColor, optimizeForLatency: true },
                { codec: 'avc3.42E01E', ...shared, optimizeForLatency: true },
                { codec: 'avc3.42E01E', ...shared },
            ];
            tryCodecs(avc3Configs, 0, () => {
                console.error('[StreamView] All H.264 configs (including avc3) rejected');
                this.decoderConfiguring = false;
                this.setStatus('error', 'Codec not supported by browser');
            });
        });
    }

    /**
     * Phase B for HEVC: try AVCC configs with codec description.
     * Called when all Annex B (no-description) configs were exhausted.
     * On total exhaustion, triggers H.264 fallback via _handleHevcFallback().
     */
    _tryHevcAvccConfigs(codec, desc, fallbacks, shared, vColor) {
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
                console.warn('[StreamView] Phase B exhausted, H.264 fallback');
                this.decoderConfiguring = false;
                this._handleHevcFallback();
                return;
            }
            const cfg = cfgs[idx];
            VideoDecoder.isConfigSupported(cfg)
                .then((r) => {
                    if (r.supported) {
                        try {
                            this.decoder.configure(cfg);
                            this.decoderConfigured = true;
                            this.decoderConfiguring = false;
                            this._noDescription = false;
                            console.log('[StreamView] Phase B OK: ' + cfg.codec);
                            this.flushPendingFrames();
                        } catch (e) {
                            console.error('[StreamView] Phase B configure() fail:', e.message);
                            this.decoderConfiguring = false;
                            // configure() throw leaves decoder in unknown state — abort
                            this._handleHevcFallback();
                        }
                    } else {
                        tryNext(idx + 1);
                    }
                })
                .catch((err) => {
                    console.error('[StreamView] Phase B isConfigSupported error:', err.message);
                    tryNext(idx + 1);
                });
        };
        tryNext(0);
    }

    /**
     * Handle HEVC decoder fallback when the browser does not support HEVC decoding.
     *
     * On Windows Chrome, VideoDecoder.isConfigSupported() returns false for all
     * HEVC configurations because Chrome lacks the HEVC codec entirely (unlike
     * Edge which licenses it from Microsoft).
     *
     * This method:
     *   1. Sets _codecFallbackRequested so MoonlightApp can detect the fallback
     *      and re-launch with H.264 forced.
     *   2. Sends a diagnostic message via the DataChannel for backend logging.
     *   3. Calls quit() to stop the current stream — the onQuit callback in
     *      MoonlightApp._onStreamingQuit() will detect _codecFallbackRequested
     *      and automatically re-launch with H.264.
     */
    _handleHevcFallback() {
        this._requestCodecFallback();
    }

    /**
     * Request the next codec/HDR step when the current decode config is
     * unsupported by the browser. Fallback chain:
     *
     *   HEVC HDR → AV1 HDR → HEVC SDR → H.264 SDR → (give up)
     *
     * The chosen target is stored in this._codecFallback; MoonlightApp's
     * _onStreamingQuit() reads it and re-launches with the forced codec/HDR.
     * When no step remains (already H.264 SDR), the stream errors out normally.
     */
    _requestCodecFallback() {
        // Guard: only request the fallback once (decoder errors fire repeatedly).
        // _manualQuitting: Stop pressed — a fallback flag set now would make
        // _onStreamingQuit() relaunch the session the user just stopped.
        if (this._codecFallbackRequested || this._quitting || this._manualQuitting) return;
        this.decoderConfiguring = false;

        const target = this._computeCodecFallbackTarget();

        if (!target) {
            // Already H.264 SDR — nothing left to try with WebCodecs. A native
            // media transport (<video> RTP) decodes through the browser's normal
            // media stack and may still work (e.g. GPU-less boxes / browsers
            // without WebCodecs H.264): hand off to the transport chain first.
            this._codecFallback = null;
            this._codecFallbackRequested = false;
            console.error('[StreamView] Codec fallback exhausted (H.264 SDR unsupported)');
            if (this._reportConnectionFailed('decoder_unsupported')) return;
            this.setStatus('error', 'Codec not supported by browser');
            this.quit();
            return;
        }

        const cur = (this.videoCodec === 'auto' ? 'hevc' : this.videoCodec).toLowerCase();
        this._codecFallback = target;
        this._codecFallbackRequested = true;

        console.warn(
            `[StreamView] Codec fallback: ${cur}${this._hdrEnabled ? ' HDR' : ''} → ` +
                `${target.codec}${target.hdr ? ' HDR' : ' SDR'}`,
        );

        // Best-effort diagnostic for backend logging (stream is about to stop).
        try {
            if (this.webrtc && typeof this.webrtc.send === 'function') {
                this.webrtc.send({
                    type: 'codec_fallback',
                    from: cur,
                    to: target.codec,
                    hdr: target.hdr,
                });
            }
        } catch (e) {
            // Ignore send errors — the stream is about to be torn down
        }

        // Quit the stream. MoonlightApp._onStreamingQuit() will detect
        // _codecFallback and re-launch with the next codec/HDR step.
        this.quit();
    }

    /**
     * Compute the next {codec, hdr} step in the fallback chain from the current
     * codec + HDR state, or null when nothing remains (already H.264 SDR).
     *
     *   HEVC HDR → AV1 HDR → HEVC SDR → H.264 SDR → null
     */
    _computeCodecFallbackTarget() {
        // Negotiated codec ('auto' implies an HDR-capable codec → treat as HEVC).
        const cur = (this.videoCodec === 'auto' ? 'hevc' : this.videoCodec).toLowerCase();
        const hdr = this._hdrEnabled;

        if (hdr) {
            // HEVC HDR → AV1 HDR ; any other HDR codec (AV1 HDR, …) → HEVC SDR.
            return cur === 'hevc' ? { codec: 'av1', hdr: true } : { codec: 'hevc', hdr: false };
        }
        if (cur === 'hevc' || cur === 'av1') {
            return { codec: 'h264', hdr: false };
        }
        return null;
    }

    flushPendingFrames() {
        // After decoder configure, a keyframe MUST be the first frame fed.
        // Delta frames can arrive before the keyframe (SCTP unordered delivery
        // over high-latency links, e.g. remote UPnP). Feeding a delta first
        // produces green/garbage output — the green-image bug.
        if (this.pendingFrames.length > 1 && !this.pendingFrames[0].isKeyframe) {
            const keyIdx = this.pendingFrames.findIndex((e) => e.isKeyframe);
            if (keyIdx > 0) {
                // Delta arrived before the keyframe (SCTP reordering): move the
                // keyframe to the front so the decoder is fed it first.
                const [keyframe] = this.pendingFrames.splice(keyIdx, 1);
                this.pendingFrames.unshift(keyframe);
            }
        }
        while (this.pendingFrames.length > 0) {
            const entry = this.pendingFrames.shift();
            this.decodeFrame(entry.data, entry.isKeyframe, entry.backendTs);
        }
    }

    decodeFrame(data, isKeyframe, backendTs) {
        if (!this.decoderConfigured) {
            // Buffer until decoder is ready (limit to avoid OOM)
            if (this.pendingFrames.length < 120) {
                this.pendingFrames.push({ data, isKeyframe, backendTs });
            }
            return;
        }

        // Belt-and-suspenders: decoder was nulled during cleanup but a frame
        // slipped past the decoderConfigured check (re-entrant event processing).
        if (!this.decoder) {
            console.warn('[StreamView] decodeFrame: decoder is null, dropping frame');
            return;
        }

        // VideoDecoder is single-use: once closed by an async error, it stays
        // closed.  Detect this early and trigger recovery rather than flooding
        // the console with "Cannot call 'decode' on a closed codec".
        if (this.decoder.state === 'closed') {
            console.warn('[StreamView] Decoder is closed, triggering recovery');
            this._handleDecoderError(new Error('Decoder state is closed'));
            return;
        }

        // Drop delta frames when the reference picture is invalid (gap or decoder error).
        // Keyframes always pass through — they restore the reference.
        if (!isKeyframe && !this._referenceValid) {
            this.stats.dropped++;
            // Retry while waiting — the original request may have been lost.
            this._requestIdr('reference invalid');
            return;
        }

        // Decoder queue backpressure: transient saturation is absorbed by
        // decoding through (instant delta-dropping caused a perpetual
        // IDR/stutter cycle on mobile WSS). Only SUSTAINED saturation drops
        // deltas + requests an IDR; a decoder reset needs an even longer stall.
        if (!isKeyframe && this.decoder.decodeQueueSize >= this.DECODE_QUEUE_MAX) {
            const now = performance.now();
            if (this._queueStallStart === 0) this._queueStallStart = now;
            const saturatedMs = now - this._queueStallStart;
            if (saturatedMs > this.QUEUE_RESET_MS) {
                console.warn(
                    '[StreamView] Decode queue stalled >' +
                        this.QUEUE_RESET_MS +
                        'ms (queueSize=' +
                        this.decoder.decodeQueueSize +
                        ') — resetting decoder',
                );
                this._queueStallStart = 0;
                this._handleDecoderError(new Error('Decode queue stalled'));
                return;
            }
            if (saturatedMs > this.QUEUE_STALL_MS) {
                this.stats.dropped++;
                this._referenceValid = false;
                this._requestIdr('decode queue overflow');
                return;
            }
            // Transient burst: fall through and decode, the queue absorbs it.
        } else {
            this._queueStallStart = 0;
        }

        // Timestamp from the backend capture clock (ms → µs) when available —
        // reflects real frame pacing instead of a synthetic 60fps clock.
        // Monotonicity is enforced (WebCodecs dislikes duplicate timestamps).
        let timestamp =
            backendTs !== undefined && backendTs > 0 ? backendTs * 1000 : this.frameCount * 16667;
        if (timestamp <= this._lastChunkTs) timestamp = this._lastChunkTs + 1;
        this._lastChunkTs = timestamp;
        this.frameCount++;

        const type = isKeyframe ? 'key' : 'delta';

        // Convert Annex B (start codes) to the expected output format.
        //   _noDescription=true  → Annex B (start codes), all NALs kept
        //   _noDescription=false → AVCC (length prefixes), VPS/SPS/PPS stripped
        const useAnnexB = this._noDescription && this.nalParser.codec === CODEC_HEVC;
        const avccData = toAvcc(data, this.decoderConfigured, this.nalParser.codec, useAnnexB);

        // Debug: catch empty AVCC data (all NALs stripped, including the IRAP)
        if (avccData.length === 0) {
            console.error(
                '[StreamView] EMPTY AVCC DATA for ' +
                    type +
                    ' frame #' +
                    this.frameCount +
                    ' — stripParams=' +
                    this.decoderConfigured +
                    ' codec=' +
                    this.nalParser.codec +
                    ' inputSize=' +
                    data.length,
            );
            // Check if all NALs were stripped: split and log types
            const nals = splitNals(data);
            const types = nals.map((n) => {
                if (n.length >= 2 && this.nalParser.codec === CODEC_HEVC) return (n[0] >> 1) & 0x3f;
                return n.length >= 1 ? n[0] & 0x1f : -1;
            });
            console.error('[StreamView] NAL types in empty-avcc frame:', types.join(', '));
        }

        try {
            const chunk = new EncodedVideoChunk({
                type: type,
                timestamp: timestamp,
                duration: 16667,
                data: avccData,
            });
            this._trackChunkSubmit(timestamp);
            this.decoder.decode(chunk);
            this.stats.received++;
            // Keyframe successfully submitted: reference is valid again.
            // Also restart the saturation clock — the stale value would count
            // the recovery wait as queue stall and trigger an instant reset.
            if (isKeyframe) {
                this._referenceValid = true;
                this._idrRequested = false;
                this._queueStallStart = 0;
            }
        } catch (err) {
            console.error('[StreamView] decode() error:', err.message, err);
            this.stats.dropped++;
            this._handleDecoderError(err);
        }
    }

    /**
     * Record the decode() submit time of a chunk for the client pipeline
     * latency measurement. Entries are consumed in onDecodedFrame; frames the
     * decoder never outputs (drops, decoder reset) would leak, so the map is
     * cleared when it grows past a burst-sized bound.
     */
    _trackChunkSubmit(timestamp) {
        if (this._chunkSubmitTimes.size > 240) this._chunkSubmitTimes.clear();
        this._chunkSubmitTimes.set(timestamp, performance.now());
    }

    onDecodedFrame(frame) {
        this.stats.decoded++;

        // A successful decode means we've recovered — reset the counter
        this._recoveryAttempts = 0;

        // FPS tracking: record decode timestamp (2s sliding window in _updateOverlay)
        this._fpsTimestamps.push(performance.now());

        // Capture resolution from frame dimensions. Kept out of the first-frame
        // block below: on some decoders (e.g. iOS Safari/HEVC) the very first
        // frame reports 0×0, which would otherwise leave the overlay stuck on
        // "?". Retry until a valid size is seen, and pick up resolution changes.
        const fw = frame.displayWidth || frame.codedWidth || 0;
        if (fw > 0) {
            const res = fw + '×' + (frame.displayHeight || frame.codedHeight || 0);
            if (res !== this._resolution) this._resolution = res;
        }

        // ── Client pipeline latency ─────────────────────────────────────────
        // Pair the decoder output with its decode() submit time (same clock,
        // matched via the chunk timestamp — works for real backendTs-derived
        // timestamps and AV1's synthetic ones alike). The sample completes
        // after render in _pumpRender, so it covers decode queue + decode +
        // frame queue + render: the browser-side leg of the total latency.
        const submitPerf = this._chunkSubmitTimes.get(frame.timestamp);
        if (submitPerf !== undefined) {
            this._chunkSubmitTimes.delete(frame.timestamp);
            frame._mwSubmitPerf = submitPerf;
        }

        // Limit queue depth to prevent unbounded memory growth.
        // Drop the NEW frame instead of closing an old one — the old frame
        // may still be rendering in the renderer's draw() (async + not awaited).
        // Closing it mid-render zeroes its NV12 buffer → green screen on Chrome.
        if (this.frameQueue.length >= 3) {
            frame.close();
            this.stats.dropped++;
            return;
        }

        this.frameQueue.push(frame);

        // VSync off (option A): present as soon as the frame is decoded instead of
        // waiting for the next rAF — shaves up to one refresh interval of latency.
        // Serialized by the _rendering guard. VSync on keeps the rAF-paced loop.
        if (this._immediateRender) this._pumpRender();

        // Update status on first frame
        if (!this._firstFrameRendered) {
            console.log('[StreamView] FIRST DECODED FRAME! Setting status to Live');
            this._markFirstFrame();
        }
    }

    /**
     * First decoded/presented frame of this stream instance: flip the status to
     * Live, reveal the stats overlay, tear down the startup overlay and show
     * the shortcuts slide. The overlay work is idempotent (safe to re-run);
     * the onFirstFrame callback fires exactly once. Used by the relaunch
     * freeze-frame bridge (and later by the seamless stream switcher).
     */
    _markFirstFrame() {
        const first = !this._firstFrameRendered;
        this._firstFrameRendered = true;
        this.setStatus('live', 'Live');
        if (!this._standby) {
            // Show stats overlay (only if enabled in settings)
            if (this._overlayEl && this._showPerfStats) this._overlayEl.style.display = '';
            // Mark startup step 3 ("Stream ready!") and hide overlay after 1.5s
            this._updateStartupStep(3);
            setTimeout(() => this._hideStartupOverlay(), 500);
            // Show keyboard shortcuts slide (5s auto-hide)
            this._showShortcutsSlide();
        }
        if (first && typeof this.onFirstFrame === 'function') {
            try {
                this.onFirstFrame();
            } catch (e) {
                console.warn('[StreamView] onFirstFrame callback failed:', e);
            }
        }
    }

    /**
     * Promote a standby view to the live, controlling one (seamless quality
     * switching): reveal the root, attach input, unmute audio, start gamepad
     * polling and take the wake lock. The caller (MoonlightApp) swaps
     * visibility/ownership and retires the previous view.
     */
    activate() {
        if (!this._standby) return;
        this._standby = false;
        if (this._rootEl) this._rootEl.style.visibility = '';
        this._acquireWakeLock();
        this.bindEvents();
        if (this._gamepadManager) this._gamepadManager.start();
        if (this.audioEl) this.audioEl.muted = false;
        // Startup overlay was never shown on a standby view — drop it outright.
        this._hideStartupOverlay();
        if (this._firstFrameRendered) {
            if (this._overlayEl && this._showPerfStats) this._overlayEl.style.display = '';
        }
        // Desktop gaming mode: best-effort pointer-lock re-acquisition. The
        // retiring view still holds the lock at promote time and loses it when
        // its input layer is removed; grab it the moment it is released.
        // Chromium usually honors the relock (sticky activation from earlier
        // gameplay clicks); if it refuses, the "click to capture" hint covers.
        if (this._gamingMode && this.inputEl) {
            const tryLock = () => {
                try {
                    const p = this.inputEl.requestPointerLock();
                    if (p && typeof p.catch === 'function')
                        p.catch(() => {
                            /* hint flow covers it */
                        });
                } catch (e) {
                    /* hint flow covers it */
                }
            };
            if (!document.pointerLockElement) {
                tryLock();
            } else {
                const onChange = () => {
                    document.removeEventListener('pointerlockchange', onChange);
                    if (!document.pointerLockElement) tryLock();
                };
                document.addEventListener('pointerlockchange', onChange);
                setTimeout(
                    () => document.removeEventListener('pointerlockchange', onChange),
                    3000,
                );
            }
        }
    }

    // Swap the visible surface to match the active renderer: the VideoElement
    // (HDR) renderer presents on <video>, every canvas renderer on <canvas>.
    // Decode + the rAF render loop still run on the main thread (draw() writes
    // each frame to the renderer's sink), so this only toggles visibility.
    _applyRendererSink(r) {
        if (!r) return;
        if (r.kind === 'video-element') {
            if (this.canvas) this.canvas.style.display = 'none';
            if (this.videoEl) this.videoEl.style.display = 'block';
        } else {
            if (this.videoEl && this._transport !== 'webrtc-media')
                this.videoEl.style.display = 'none';
            if (this.canvas) this.canvas.style.display = '';
        }
    }

    /**
     * Best-effort snapshot of the last presented frame, used to bridge a
     * transport relaunch with a frozen image instead of a black loader.
     * Returns a detached <canvas>, or null when nothing was presented yet or
     * the surface cannot be read back (worker mode: the placeholder canvas
     * whose control was transferred to an OffscreenCanvas is not drawable).
     */
    captureLastFrame() {
        try {
            if (!this._firstFrameRendered) return null;
            const video = this._videoIsDisplay() ? this.videoEl : null;
            const src = video || this.canvas;
            if (!src) return null;
            const w = video ? video.videoWidth : src.width;
            const h = video ? video.videoHeight : src.height;
            if (!(w > 0 && h > 0)) return null;
            const c = document.createElement('canvas');
            c.width = w;
            c.height = h;
            c.getContext('2d').drawImage(src, 0, 0, w, h);
            return c;
        } catch (e) {
            console.warn('[StreamView] captureLastFrame failed:', e);
            return null;
        }
    }

    /** Height in pixels of the incoming video, 0 when unknown. Used by the
     *  degradation ladder when the user setting is "Same as Host" (height 0). */
    currentFrameHeight() {
        try {
            const m = /×(\d+)/.exec(this._resolution || '');
            if (m) return parseInt(m[1], 10) || 0;
            if (this._videoIsDisplay() || this._transport === 'webrtc-media')
                return (this.videoEl && this.videoEl.videoHeight) || 0;
            return (this.canvas && this.canvas.height) || 0;
        } catch (e) {
            return 0;
        }
    }

    startRenderLoop() {
        if (this.renderRunning) return;
        // Media track mode: video is rendered natively via <video>, no canvas loop needed.
        if (this._transport === 'webrtc-media') return;
        // Worker mode: the worker owns the OffscreenCanvas and its render loop.
        if (this._useWorker) return;
        this.renderRunning = true;

        // Tearing on (option A): rendering is driven by decoder output (onDecodedFrame
        // → _pumpRender) for lower latency. The rAF loop then only handles context-loss
        // detection. Tearing off (default): the rAF loop paces rendering to the
        // display refresh (VSync).
        this._immediateRender = this._tearing;

        const loop = (now) => {
            if (!this.renderRunning) return;

            // Renderer is created asynchronously (in onOpen / worker fallback) —
            // skip ticks until it exists. Frames arrive only after that.
            if (!this._renderer) {
                requestAnimationFrame(loop);
                return;
            }

            // Detect Canvas2D context loss (GPU driver crash, Alt-Tab on some GPUs).
            // When lost, the canvas is permanently blank — recreate the context.
            if (this._renderer.isContextLost()) {
                console.warn('[StreamView] Canvas2D context lost, recreating...');
                this._renderer.recreateContext();
                // Force re-composite all overlay layers by briefly toggling transform
                const header = document.querySelector('.stream-header');
                if (header) {
                    header.style.transform = 'translateZ(0)';
                    requestAnimationFrame(() => {
                        header.style.transform = '';
                    });
                }
            }

            // VSync on: rAF paces the rendering. VSync off: decoder output drives it.
            if (!this._immediateRender) this._pumpRender();

            requestAnimationFrame(loop);
        };

        requestAnimationFrame(loop);
    }

    /**
     * Render the freshest queued frame, serialized by the _rendering guard.
     * Called once per rAF (VSync on) or once per decoded frame (VSync off).
     *
     * One frame is rendered at a time: HEVC NV12 on Chrome Windows fails when two
     * VideoFrame objects are accessed concurrently (createImageBitmap / copyTo race),
     * so the guard prevents overlapping GPU ops. Bursts (DC mode) drop all but the
     * freshest frame to keep latency low.
     */
    _pumpRender() {
        if (!this.renderRunning || !this._renderer) return;

        // Presentation pacing. VSync (rAF-paced) mode keeps ONE frame in
        // reserve and presents the oldest queued frame: a frame arriving a few
        // ms late is shown at the next tick instead of producing the
        // "repeat + skip" judder of drop-to-freshest (this is what makes the
        // media transport feel smoother). Costs at most one refresh interval,
        // transiently, only while the reserve is occupied — nothing in steady
        // state. Immediate mode (VSync off) keeps only the freshest frame for
        // minimum latency, as before.
        const maxQueued = this._immediateRender ? 1 : 2;

        // Previous render still in flight — trim the queue; the draw's
        // finally() re-pumps in immediate mode, the next rAF does so otherwise.
        if (this._rendering) {
            while (this.frameQueue.length > maxQueued) {
                const old = this.frameQueue.shift();
                old.close();
                this.stats.dropped++;
            }
            return;
        }

        while (this.frameQueue.length > maxQueued) {
            const old = this.frameQueue.shift();
            old.close();
            this.stats.dropped++;
        }
        if (this.frameQueue.length === 0) return;

        this._rendering = true;
        const frame = this.frameQueue.shift();

        // Fire-and-forget with guard: the renderer resizes the canvas to the frame,
        // draws and closes it; stats stay here. Read the submit time before the
        // draw — the renderer closes the VideoFrame.
        const submitPerf = frame._mwSubmitPerf;
        this._renderer
            .draw(frame)
            .then(() => {
                this.stats.rendered++;
                // Complete the client pipeline sample: decode() submit → render
                // done, all on this thread's clock.
                if (submitPerf !== undefined) {
                    const ms = performance.now() - submitPerf;
                    if (ms >= 0 && ms < 5000) this._clientLatencyStats.addSample(ms);
                }
            })
            .finally(() => {
                this._rendering = false;
                // Immediate mode: drain a frame that arrived while we were drawing.
                if (this._immediateRender && this.frameQueue.length > 0) this._pumpRender();
            });
    }

    stopRenderLoop() {
        this.renderRunning = false;
    }

    // Observe the canvas area and feed the output size (CSS box × dPR) to the
    // renderer. Canvas2D ignores it (no-op); WebGPU uses it as its backing res.
    _setupOutputSizeObserver() {
        if (typeof ResizeObserver === 'undefined' || !this.canvasArea) return;
        const apply = () => {
            const rect = this.canvasArea.getBoundingClientRect();
            const dpr = window.devicePixelRatio || 1;
            const w = Math.max(1, Math.round(rect.width * dpr));
            const h = Math.max(1, Math.round(rect.height * dpr));
            if (w === this._outW && h === this._outH) return;
            this._outW = w;
            this._outH = h;
            this._applyOutputSize();
        };
        this._resizeObserver = new ResizeObserver(apply);
        this._resizeObserver.observe(this.canvasArea);
        apply(); // initial measure
    }

    // Quantized zoom factor folded into the output size so the WebGPU enhancer
    // renders its backing at the *zoomed* device resolution (a box-sized canvas
    // CSS-scaled by pinch-zoom would otherwise blur). Quantized (ceil) so GPU
    // textures aren't reallocated on every pinch frame; the renderer clamps the
    // result to its max texture dimension. No effect on Canvas2D (ignores it).
    _outputZoomScale() {
        return Math.max(1, Math.ceil(this._zoom || 1));
    }

    // Push the current output size to the active renderer (main) or worker.
    // Re-called when the renderer/worker is created (size measured earlier) and
    // whenever the zoom step changes.
    _applyOutputSize() {
        if (this._outW <= 0 || this._outH <= 0) return;
        const s = this._outputZoomScale();
        this._lastOutputZoomScale = s;
        const w = this._outW * s,
            h = this._outH * s;
        if (this._useWorker) {
            if (this._videoWorker) {
                this._videoWorker.postMessage({ type: 'resize', outW: w, outH: h });
            }
        } else if (this._renderer) {
            this._renderer.setOutputSize(w, h);
        }
    }

    /** Report a connection failure to MoonlightApp (transport chain fallback).
     *  Returns true if the failure was handed off (caller should stop its own
     *  error/quit handling); false if there is no handler. Fires at most once. */
    _reportConnectionFailed(reason) {
        // The user already pressed Stop (exit animation running, quit() on its
        // way) — swallow the failure instead of handing it to the transport
        // chain, which would relaunch the session the user just stopped.
        if (this._manualQuitting || this._quitting) return true;
        if (this._connectionFailureReported) return true; // already handed off
        if (typeof this.onConnectionFailed !== 'function') return false;
        this._connectionFailureReported = true;
        console.warn('[StreamView] Connection failed before established:', reason);
        const cb = this.onConnectionFailed;
        this.onConnectionFailed = null;
        // Defer so the current event handler unwinds before teardown/relaunch.
        setTimeout(() => cb(reason), 0);
        return true;
    }

    // =========================================================================
    // WebRTC DataChannel (replaces legacy WebSocket binary transport)
    // =========================================================================

    setupWebRtc() {
        this.webrtc.onOpen = () => {
            // _manualQuitting: Stop pressed while connecting — don't spin up the
            // decoder/renderer, quit() is about to tear everything down.
            if (this._quitting || this._manualQuitting) return;
            this.connected = true;
            // Transport established at least once → past the connection-failure
            // window. Later failures are mid-stream disconnects, not fallbacks.
            this._everConnected = true;
            this.setStatus('connecting', 'Waiting for stream...');
            this._updateStartupStep(2);

            // No-video watchdog: transport up but nothing ever rendered — the
            // decode/render path is broken on this transport (not the network),
            // e.g. WebCodecs can't decode on a GPU-less box while the native
            // <video> path would. Hand it to the transport chain instead of
            // sitting on the "Waiting for stream" overlay forever.
            if (this._noVideoTimer) clearTimeout(this._noVideoTimer);
            this._noVideoTimer = setTimeout(() => {
                if (this._firstFrameRendered || this._quitting || this._manualQuitting) return;
                const webcodecs = this._transport !== 'webrtc-media';
                const reason =
                    webcodecs && this.stats.decoded === 0 ? 'decoder_unsupported' : 'no_video';
                console.error('[StreamView] No video 15s after connect (' + reason + ')');
                if (this._reportConnectionFailed(reason)) return;
                this.setStatus('error', 'No video received');
                this.quit();
            }, 15000);

            // Start ping timer for browser-side RTT measurement.
            // Sends a ping every 2s on the input DC; backend echoes back a pong.
            if (this._pingInterval) clearInterval(this._pingInterval);
            this._pingInterval = setInterval(() => {
                if (this._quitting) return;
                const seq = this._pingSeq++;
                this.webrtc.send({ type: 'ping', seq, ts: performance.now() });
            }, 2000);

            // Start polling connected gamepads (Xbox/PlayStation). Sends a
            // controller snapshot over the input transport only on change.
            if (!this._gamepadManager && navigator.getGamepads) {
                this._gamepadManager = new GamepadManager((msg) => {
                    if (!this._quitting) this.webrtc.send(msg);
                });
            }
            // Standby: created but not polling — started in activate().
            if (this._gamepadManager && !this._standby) this._gamepadManager.start();

            if (this._transport === 'webrtc-media') {
                // Media track mode: video arrives natively via <video> element.
                // No WebCodecs decoder needed. The first video frame triggers
                // the 'live' status via the video element's playing event.
                if (this.videoEl) {
                    const onMediaLive = () => {
                        if (this._firstFrameRendered) return;
                        const w = this.videoEl.videoWidth || 0;
                        const h = this.videoEl.videoHeight || 0;
                        if (w > 0) this._resolution = w + '×' + h;
                        this._markFirstFrame();
                    };
                    // The video track may already be playing by the time the
                    // DataChannels open (ontrack + play() can fire before onOpen),
                    // so a one-shot 'playing' listener would miss it and leave the
                    // overlay stuck on step 2. Bind several signals AND check the
                    // current state; onMediaLive is idempotent.
                    this.videoEl.onplaying = onMediaLive;
                    this.videoEl.addEventListener('timeupdate', onMediaLive);
                    if (
                        !this.videoEl.paused &&
                        this.videoEl.currentTime > 0 &&
                        this.videoEl.readyState >= 2
                    ) {
                        onMediaLive();
                    }
                }
                // Native RTP stats: poll getStats() for fps/bitrate/latency,
                // since these frames never reach the WebCodecs pipeline.
                this._startMediaStatsPolling();
            } else {
                // DataChannel mode: set up WebCodecs decoder (main thread) OR
                // hand decode+render to the OffscreenCanvas worker (opt-in).
                if (this._useWorker) {
                    this._initVideoWorker();
                } else {
                    // Create the main-thread renderer here (not in render()) so the
                    // resolved _isChromeWindowsHevc flag gates the HEVC NV12 path.
                    createVideoRenderer(this.canvas, {
                        desynchronized: this._tearing,
                        videoCodec: this.videoCodec,
                        isChromeWindowsHevc: this._isChromeWindowsHevc,
                        webgpu: this._wantWebGpu,
                        algo: this._videoEnhancementAlgo,
                        hdr: this._hdrEnabled && !this._hdrTonemap,
                        hdrTonemap: this._hdrTonemap,
                        videoEl: this._useVideoSink ? this.videoEl : null,
                    }).then((r) => {
                        this._renderer = r;
                        this._activeRendererKind = r.kind;
                        this._rendererHdrActive = !!r.hdrActive;
                        console.log(
                            '[StreamView] renderer=' +
                                r.kind +
                                ' hdr=' +
                                this._hdrEnabled +
                                ' tonemap=' +
                                this._hdrTonemap +
                                ' displayHdr=' +
                                this._displayHdr +
                                ' videoSink=' +
                                this._useVideoSink +
                                ' algo=' +
                                this._videoEnhancementAlgo,
                        );
                        this._applyRendererSink(r);
                        this._applyOutputSize();
                    });
                    this.setupDecoder();
                }

                // Safety timeout: if no decoder config after 3 seconds, request an IDR.
                // In worker mode the worker owns the NAL parser and requests its
                // own IDRs, so skip the main-thread nalParser readiness probe.
                this._idrTimeout = setTimeout(() => {
                    if (!this._useWorker && !this.nalParser.isReady()) {
                        console.warn(
                            '[StreamView] No keyframe after 3s, requesting IDR from backend',
                        );
                        this._requestIdr('no keyframe after 3s');
                    }
                }, 3000);
            }

            // Mobile fullscreen/button init (all transport modes)
            this._initMobileFullscreen();
        };
        this.webrtc.onClose = () => {
            // _manualQuitting: Stop pressed, exit animation running — quit() is
            // already scheduled, don't treat the close as a disconnect.
            if (this._quitting || this._manualQuitting) return;
            // Taken over by another device — _handleTakeover() drives the exit.
            if (this._takenOver) return;
            // Never connected → connection failure, let the chain try the next
            // transport instead of showing a disconnect error.
            if (!this._everConnected && this._reportConnectionFailed('closed before connect'))
                return;
            this.connected = false;
            this.setStatus('disconnected', 'Disconnected');
            Toast.error(t('stream.streamDisconnected'));
            setTimeout(() => this.quit(), 3000);
        };
        this.webrtc.onError = (err) => {
            if (this._quitting || this._manualQuitting) return;
            console.error('[StreamView] WebRTC error:', err.message);
            // Never connected → connection failure, defer to the transport chain.
            if (!this._everConnected && this._reportConnectionFailed(err.message)) return;
            Toast.error(t('stream.webrtcError'));
        };
        // Video frames: DataChannel mode uses WebCodecs callbacks
        if (this._transport !== 'webrtc-media') {
            this.webrtc.onVideo = (frame, isKeyframe, backendTs, frameId) =>
                this.handleVideoFrame(frame, isKeyframe, backendTs, frameId);
            // Frame loss from reassembly: invalidate reference until next keyframe.
            // IDR is already requested by WebRtcDataChannel (throttled) — don't double it.
            this.webrtc.onFrameLoss = (frameId, wasKeyframe) => {
                this._referenceValid = false;
                if (this._videoWorker) this._videoWorker.postMessage({ type: 'frameloss' });
            };
            // IDR requests actually sent by the transport feed the congestion
            // monitor (loss/starvation-driven — a storm means a saturated link).
            this.webrtc.onIdrRequested = (reason) => this._recordCongestionEvent('dc:' + reason);
        }
        // Audio: WSS feeds Opus packets to the AudioPipeline. WebRTC transports
        // use a native RTP Opus track (rendered via the <audio> element), so no
        // onAudio callback is wired there.
        if (!this._nativeAudio) {
            this.webrtc.onAudio = (sample) => this.handleAudioSample(sample);
        }
        this.webrtc.connect();
    }

    // ── webrtc-media native stats polling (getStats) ──────────────────────

    /**
     * Poll RTCPeerConnection.getStats() every second to derive fps, bitrate,
     * resolution and an end-to-end-ish latency for the native RTP media track.
     * These cannot come from the WebCodecs counters (frames bypass the decoder).
     */
    _startMediaStatsPolling() {
        if (this._mediaStatsTimer) return;
        const pc = this.webrtc && this.webrtc.pc;
        if (!pc || typeof pc.getStats !== 'function') return;

        // Reset adaptive jitter-buffer controller state for this session.
        this._jitterController.reset();

        this._mediaStatsTimer = setInterval(async () => {
            if (this._quitting) return;
            const peer = this.webrtc && this.webrtc.pc;
            if (!peer || typeof peer.getStats !== 'function') return;
            try {
                const report = await peer.getStats();
                let inbound = null;
                let candidatePair = null;
                report.forEach((s) => {
                    if (s.type === 'inbound-rtp' && s.kind === 'video') inbound = s;
                    else if (
                        s.type === 'candidate-pair' &&
                        (s.nominated || s.selected || s.state === 'succeeded')
                    ) {
                        candidatePair = s;
                    }
                });
                if (!inbound) return;

                const now = performance.now();
                // Resolution
                if (inbound.frameWidth > 0 && inbound.frameHeight > 0) {
                    this._resolution = inbound.frameWidth + '×' + inbound.frameHeight;
                }
                // FPS: prefer the engine-reported value, fall back to a delta
                if (typeof inbound.framesPerSecond === 'number') {
                    this._mediaFps = Math.round(inbound.framesPerSecond);
                } else if (this._lastInboundStatsTime > 0) {
                    const dt = (now - this._lastInboundStatsTime) / 1000;
                    if (dt > 0) {
                        this._mediaFps = Math.round(
                            ((inbound.framesDecoded || 0) - this._lastInboundFrames) / dt,
                        );
                    }
                }
                // Bitrate from bytesReceived delta
                if (this._lastInboundStatsTime > 0) {
                    const dt = (now - this._lastInboundStatsTime) / 1000;
                    const dBytes = (inbound.bytesReceived || 0) - this._lastInboundBytes;
                    if (dt > 0 && dBytes >= 0) {
                        this._mediaBitrateMbps = (dBytes * 8) / dt / 1e6;
                    }
                }
                this._lastInboundBytes = inbound.bytesReceived || 0;
                this._lastInboundFrames = inbound.framesDecoded || 0;
                this._lastInboundStatsTime = now;

                // PLIs sent by the receiver = the media-mode equivalent of IDR
                // requests: each one means the decoder lost its reference.
                const pliCount = inbound.pliCount || 0;
                if (pliCount > this._lastPliCount) {
                    this._recordCongestionEvent('pli +' + (pliCount - this._lastPliCount));
                }
                this._lastPliCount = pliCount;

                // Adaptive jitter buffer: drive receiver.jitterBufferTarget from
                // measured jitter/loss/freezes (webrtc-media only, flag-gated).
                if (this._jitterAuto) {
                    const cmd = this._jitterController.update({
                        packetsLost: inbound.packetsLost || 0,
                        packetsReceived: inbound.packetsReceived || 0,
                        freezeCount: inbound.freezeCount || 0,
                        jitterMs: (typeof inbound.jitter === 'number' ? inbound.jitter : 0) * 1000,
                        rttMs:
                            candidatePair && candidatePair.currentRoundTripTime > 0
                                ? candidatePair.currentRoundTripTime * 1000
                                : 0,
                    });
                    if (cmd !== null) {
                        if (
                            this.webrtc &&
                            typeof this.webrtc.setVideoJitterBufferTarget === 'function'
                        ) {
                            this.webrtc.setVideoJitterBufferTarget(cmd);
                        }
                        const s = this._jitterController.lastSignals;
                        console.log(
                            '[JitterAuto] target=' +
                                Math.round(cmd) +
                                'ms jitter=' +
                                (s ? s.jitterEwma.toFixed(1) : '?') +
                                ' loss=' +
                                (s ? (s.lossRate * 100).toFixed(1) : '?') +
                                '%' +
                                ' rtt=' +
                                (s ? Math.round(s.rttMs) : '?') +
                                ' freezes=' +
                                (s ? s.freezes : '?'),
                        );
                    }
                }

                // Client-side latency ≈ current jitter-buffer delay + network
                // RTT/2 + per-frame decode time. The Sunshine-side legs
                // (hostProcMs, hostRttMs) are added in _updateOverlay.
                // jitterBufferDelay/jitterBufferEmittedCount are CUMULATIVE since
                // stream start: use interval deltas for a *current* value (the
                // cumulative average lags jitter-buffer changes by minutes on a
                // long session). First tick falls back to the cumulative average.
                let latency = 0;
                const jbDelay = inbound.jitterBufferDelay || 0;
                const jbEmitted = inbound.jitterBufferEmittedCount || 0;
                if (jbEmitted > this._lastJbEmitted) {
                    latency +=
                        ((jbDelay - this._lastJbDelay) / (jbEmitted - this._lastJbEmitted)) * 1000;
                } else if (jbDelay > 0 && jbEmitted > 0) {
                    latency += (jbDelay / jbEmitted) * 1000;
                }
                this._lastJbDelay = jbDelay;
                this._lastJbEmitted = jbEmitted;
                if (candidatePair && candidatePair.currentRoundTripTime > 0) {
                    latency += (candidatePair.currentRoundTripTime * 1000) / 2;
                }
                // Decode time per frame (delta) — parity with the DC estimate,
                // which includes decode. Not exposed by all browsers (guarded).
                const decTime = inbound.totalDecodeTime;
                const decFrames = inbound.framesDecoded || 0;
                if (
                    typeof decTime === 'number' &&
                    this._lastDecodeTime >= 0 &&
                    decFrames > this._lastDecodedForLatency
                ) {
                    latency +=
                        ((decTime - this._lastDecodeTime) /
                            (decFrames - this._lastDecodedForLatency)) *
                        1000;
                }
                this._lastDecodeTime = typeof decTime === 'number' ? decTime : -1;
                this._lastDecodedForLatency = decFrames;
                if (latency > 0 && latency < 5000) this._mediaLatencyStats.addSample(latency);
            } catch (e) {
                // getStats can throw transiently during teardown — ignore
            }
        }, 1000);
    }

    _stopMediaStatsPolling() {
        if (this._mediaStatsTimer) {
            clearInterval(this._mediaStatsTimer);
            this._mediaStatsTimer = null;
        }
    }

    // ── Draggable stats overlay ──────────────────────────────────────────
    // Pointer-driven drag so the card can be moved off important parts of the
    // game. Switches the card from CSS top/left anchoring to inline px coords,
    // clamped to the viewport. Not persisted: reset on every new session.
    _makeStatsDraggable(el) {
        let dragging = false;
        let startX = 0,
            startY = 0,
            startLeft = 0,
            startTop = 0;

        const onMove = (e) => {
            if (!dragging) return;
            const dx = e.clientX - startX;
            const dy = e.clientY - startY;
            const maxLeft = Math.max(0, window.innerWidth - el.offsetWidth);
            const maxTop = Math.max(0, window.innerHeight - el.offsetHeight);
            el.style.left = Math.min(maxLeft, Math.max(0, startLeft + dx)) + 'px';
            el.style.top = Math.min(maxTop, Math.max(0, startTop + dy)) + 'px';
            el.style.right = 'auto';
            // Drop any CSS centering transform (immersive overlay) so the px
            // left/top above is the real visual position, not offset by -50%.
            el.style.transform = 'none';
            // Mark as manually placed: auto-positioning must not override it.
            el.classList.add('user-moved');
            // On touch devices, block move handling of the main stream — touch moves
            // on the stats card must not feed into the game's mouse tracking.
            if (e.pointerType === 'touch') e.stopPropagation();
            e.preventDefault();
        };
        const onUp = (e) => {
            if (!dragging) return;
            if (e.pointerType === 'touch') e.stopPropagation();
            dragging = false;
            el.classList.remove('dragging');
            try {
                el.releasePointerCapture(e.pointerId);
            } catch (_) {}
            window.removeEventListener('pointermove', onMove);
            window.removeEventListener('pointerup', onUp);
            e.preventDefault();
        };
        el.addEventListener('pointerdown', (e) => {
            if (e.button !== 0 && e.pointerType === 'mouse') return;
            // The close (×) button handles its own pointer events — never drag.
            if (e.target.closest && e.target.closest('.overlay-close-btn')) return;
            const rect = el.getBoundingClientRect();
            startX = e.clientX;
            startY = e.clientY;
            startLeft = rect.left;
            startTop = rect.top;
            dragging = true;
            el.classList.add('dragging');
            try {
                el.setPointerCapture(e.pointerId);
            } catch (_) {}
            // Prevent the pointer from locking onto the view content behind it,
            // and block menu interaction on touch devices.
            if (e.pointerType === 'touch') e.stopPropagation();
            window.addEventListener('pointermove', onMove);
            window.addEventListener('pointerup', onUp);
            e.preventDefault();
        });
    }

    // ── Overlay close (×) button ─────────────────────────────────────────

    /** Markup for the small × button that dismisses a draggable overlay. */
    _overlayCloseBtnHtml() {
        const label = t('stream.closeOverlay');
        return (
            '<button type="button" class="overlay-close-btn" aria-label="' +
            label +
            '" title="' +
            label +
            '">×</button>'
        );
    }

    /** Dismiss a draggable overlay (× button). */
    _closeOverlayEl(el) {
        if (el === this._overlayEl) {
            this._statsClosed = true;
            if (this._overlayEl) this._overlayEl.style.display = 'none';
        } else if (el === this._immersiveOverlay) {
            this._immersiveClosed = true;
            this._updateImmersiveOverlay();
        }
    }

    // ── Immersive-mode exit reminder ─────────────────────────────────────

    /**
     * Platform-correct fullscreen-toggle combo (…+X) as <kbd> chips, shown on
     * the header Fullscreen button in immersive mode. Same modifiers as the
     * exit reminder, ending in X (toggle fullscreen) instead of Z (release).
     */
    _fsComboKeysHtml() {
        const isMac = /Mac/.test(navigator.platform);
        const modA = isMac ? 'Cmd' : 'Ctrl';
        const modB = isMac ? 'Option' : 'Alt';
        const modC = isMac ? 'Ctrl' : 'Shift';
        const keys = isMac ? [modC, modB, modA, 'X'] : [modC, modA, modB, 'X'];
        let html = '<span class="fs-combo">';
        for (let i = 0; i < keys.length; i++) {
            if (i > 0) html += '<span class="fs-plus">+</span>';
            html += '<kbd>' + keys[i] + '</kbd>';
        }
        html += '</span>';
        return html;
    }

    /**
     * Build the immersive overlay content: the platform-correct exit combo
     * plus a one-line reminder of what it does (free the mouse + leave
     * fullscreen). Built once — the combo never changes during a session.
     */
    _buildImmersiveOverlayContent() {
        if (!this._immersiveOverlay) return;
        const isMac = /Mac/.test(navigator.platform);
        const modA = isMac ? 'Cmd' : 'Ctrl';
        const modB = isMac ? 'Option' : 'Alt';
        const modC = isMac ? 'Ctrl' : 'Shift';
        // Win order: Shift + Ctrl + Alt + Z — Mac: Ctrl + Option + Cmd + Z
        const keys = isMac ? [modC, modB, modA, 'Z'] : [modC, modA, modB, 'Z'];
        let html = '<span class="imm-keys">';
        for (let i = 0; i < keys.length; i++) {
            if (i > 0) html += '<span class="imm-plus">+</span>';
            html += '<kbd>' + keys[i] + '</kbd>';
        }
        html += '</span>';
        html += '<span class="imm-text">' + t('stream.immersiveExitTitle') + '</span>';
        html += this._overlayCloseBtnHtml();
        this._immersiveOverlay.innerHTML = html;
        this._immersiveOverlay.title = t('stream.immersiveExitTitle');
    }

    /**
     * Show the immersive overlay only while immersive mode actually holds the
     * mouse (pointer locked). Hidden otherwise so it never clutters the view.
     */
    _updateImmersiveOverlay() {
        if (!this._immersiveOverlay) return;
        // Visible whenever immersive mode is on (after the first frame), NOT only
        // while the mouse is captured: a captured (pointer-locked) cursor cannot
        // be moved onto the card to drag it, so it must be reachable pre-capture.
        const show = this._gamingMode && this._firstFrameRendered && !this._immersiveClosed;
        this._immersiveOverlay.classList.toggle('visible', show);
        if (show) this._positionImmersiveOverlay();
    }

    /**
     * Place the immersive reminder.
     *  - Non-fullscreen (Fullscreen button visible): dock it in the header just
     *    right of the centered Fullscreen button — outside the streamed image,
     *    so it never covers the game (and the fade effect stays off there).
     *  - Fullscreen (button hidden): back to the CSS default (top-center).
     * No-op once the user has dragged the card (manual position wins).
     */
    _positionImmersiveOverlay() {
        const el = this._immersiveOverlay;
        if (!el) return;
        const fsBtn = this._mobileFsBtn;
        // Default: button back to its CSS-centered position (cleared each pass so
        // the pair-centering offset below is never left stale).
        if (fsBtn) fsBtn.style.transform = '';
        if (el.classList.contains('user-moved')) return;
        // Require a REAL on-screen rect: in native fullscreen the header (and its
        // button) is hidden via CSS, so style.display stays '' while the rect
        // collapses to 0. Docking off a 0-rect would slam the card to top-left
        // over the stats card. Only dock when the button is actually laid out.
        const r = fsBtn && fsBtn.isConnected ? fsBtn.getBoundingClientRect() : null;
        const fsVisible = r && r.width > 0 && r.height > 0 && fsBtn.style.display !== 'none';
        if (fsVisible && el.classList.contains('visible')) {
            // Center the PAIR (Fullscreen button + reminder) as a group: shift the
            // button left by half the reminder's footprint, then dock the reminder
            // to its right. The midpoint of the pair lands on the viewport center.
            const gap = 12;
            const overlayW = el.getBoundingClientRect().width;
            const shift = (overlayW + gap) / 2;
            fsBtn.style.transform = `translate(calc(-50% - ${shift}px), -50%)`;
            const rb = fsBtn.getBoundingClientRect();
            el.style.left = Math.round(rb.right + gap) + 'px';
            el.style.top = Math.round(rb.top + rb.height / 2) + 'px';
            el.style.transform = 'translateY(-50%)';
        } else {
            // Restore the CSS default (left:50% + translateX(-50%), safe-area top).
            el.style.left = '';
            el.style.top = '';
            el.style.transform = '';
        }
    }

    // ── Stats overlay (refreshed every 500ms) ────────────────────────────

    _updateOverlay() {
        // Refresh the immersive exit reminder on the same 500ms tick so it
        // appears once streaming starts, independent of the perf-stats setting.
        this._updateImmersiveOverlay();

        if (!this._overlayEl) return;

        // Dismissed by the user via its × button — stays hidden for the session.
        if (this._statsClosed) {
            this._overlayEl.style.display = 'none';
            return;
        }

        // Hide entire overlay when performance stats are disabled in settings
        if (!this._showPerfStats) {
            this._overlayEl.style.display = 'none';
            return;
        }

        // Before first frame: show minimal waiting state
        if (!this._firstFrameRendered) {
            this._overlayEl.innerHTML =
                '<div class="stats-waiting">' +
                t('stream.connecting') +
                '</div>' +
                this._overlayCloseBtnHtml();
            this._overlayEl.style.display = '';
            return;
        }

        const now = performance.now();
        const elapsed = (now - this._startTime) / 1000;
        let fps = 0;
        let bitrateMbps = 0;
        const isMedia = this._transport === 'webrtc-media';

        if (isMedia) {
            // Native RTP: values come from the getStats() poller
            fps = this._mediaFps;
            bitrateMbps = this._mediaBitrateMbps;
        } else {
            // FPS: decoded frames in the last 2 seconds
            const cutoff = now - 2000;
            this._fpsTimestamps = this._fpsTimestamps.filter((t) => t > cutoff);
            fps = Math.round(this._fpsTimestamps.length / 2);

            // Bitrate: total bytes / elapsed seconds
            if (elapsed > 0.5) {
                bitrateMbps = (this._totalBytes * 8) / elapsed / 1e6;
            }
        }

        const codec = this.videoCodec === 'auto' ? 'h264' : this.videoCodec;

        // Build styled overlay HTML — each stat on its own line with a descriptive label
        let html = '<div class="stats-body">';

        // Resolution
        html +=
            '<div class="stats-row">' +
            '<span class="stats-label">' +
            t('stream.statResolution') +
            '</span>' +
            '<span class="stats-value">' +
            (this._resolution || '?') +
            '</span>' +
            '</div>';

        // Framerate
        html +=
            '<div class="stats-row">' +
            '<span class="stats-label">' +
            t('stream.statFramerate') +
            '</span>' +
            '<span class="stats-value">' +
            fps +
            ' fps</span>' +
            '</div>';

        // Bitrate
        html +=
            '<div class="stats-row">' +
            '<span class="stats-label">' +
            t('stream.statBitrate') +
            '</span>' +
            '<span class="stats-value">' +
            bitrateMbps.toFixed(1) +
            ' Mbps</span>' +
            '</div>';

        // Codec (annotated with 4:4:4 when full-chroma was negotiated, and HDR
        // when a 10-bit profile was requested). A '*' marks that the canvas
        // actually presents in HDR (rgba16float accepted); no '*' means the
        // decode is HDR but the canvas fell back to SDR.
        let codecLabel = codec.toUpperCase() + (this._yuv444 ? ' 4:4:4' : '');
        if (this._hdrEnabled) {
            // HDR→SDR when ACES tone-mapping feeds the enhancer; HDR*/HDR for the
            // <video> sink (true HDR) vs SDR-fallback canvas.
            if (this._hdrTonemap) codecLabel += ' HDR→SDR';
            else codecLabel += this._rendererHdrActive ? ' HDR*' : ' HDR';
        }
        html +=
            '<div class="stats-row">' +
            '<span class="stats-label">' +
            t('stream.statCodec') +
            '</span>' +
            '<span class="stats-value">' +
            codecLabel +
            '</span>' +
            '</div>';

        // Enhancer: show the active algo when the WebGPU renderer is up. On
        // webrtc-media (<video>, no canvas) the Enhancer can't be applied — if the
        // user enabled it, flag it OFF so they know it isn't active.
        let enhancerName = null;
        if (this._activeRendererKind === 'webgpu') {
            enhancerName =
                this._videoEnhancementAlgo === 'fsr1'
                    ? 'FSR1'
                    : this._videoEnhancementAlgo === 'off'
                      ? t('stream.enhancerOff')
                      : 'SGSR';
        } else if (this._transport === 'webrtc-media' && this._videoEnhancementRequested) {
            enhancerName = 'OFF (not available on MediaTrack)';
        } else if (
            this._activeRendererKind === 'video-element' &&
            this._videoEnhancementRequested
        ) {
            // Escape angle brackets: this string is injected via innerHTML, a raw
            // <video> would spawn a real video element (huge empty box in the overlay).
            enhancerName = 'OFF (HDR via &lt;video&gt;)';
        }
        if (enhancerName !== null) {
            html +=
                '<div class="stats-row">' +
                '<span class="stats-label">' +
                t('stream.statEnhancer') +
                '</span>' +
                '<span class="stats-value">' +
                enhancerName +
                '</span>' +
                '</div>';
        }

        // Transport
        html +=
            '<div class="stats-row">' +
            '<span class="stats-label">' +
            t('stream.statTransport') +
            '</span>' +
            '<span class="stats-value">' +
            this._transportMode +
            '</span>' +
            '</div>';

        // End-to-end latency: sum of independently measured legs, each averaged
        // over its own short sliding window (moonlight-qt composes its overlay
        // the same way from per-second stats windows — the value therefore
        // tracks reality every refresh instead of freezing on a stale estimate):
        //   hostProcMs        Sunshine capture→encode (RTP extension)
        //   hostRttMs         Sunshine↔backend one-way (ENet RTT / 2)
        //   decodeLatencyUs   backend pipeline (frame submit → transport send)
        //   browserRtt / 2    backend↔browser one-way (ping/pong)
        //   client pipeline   decode queue + decode + frame queue + render
        //                     (per-frame, browser clock) — or the jitter-buffer/
        //                     decode getStats deltas on the native media track.
        let avgLatency = '--';
        {
            const clientStats = isMedia ? this._mediaLatencyStats : this._clientLatencyStats;
            let latency = 0;
            let haveLatency = false;
            if (clientStats.count > 0) {
                latency += clientStats.avg;
                haveLatency = true;
            }
            // Media mode: the getStats sample already contains the network RTT/2.
            if (!isMedia && this._browserRttStats.count > 0) {
                latency += this._browserRttStats.avg / 2;
                haveLatency = true;
            }
            if (this._hostRttStats.count > 0) latency += this._hostRttStats.avg;
            if (this._hostProcStats.count > 0) latency += this._hostProcStats.avg;
            if (!isMedia && this._decodeLatencyStats.count > 0) {
                latency += this._decodeLatencyStats.avg;
            }
            if (haveLatency) avgLatency = latency.toFixed(1) + 'ms';
        }
        html +=
            '<div class="stats-row stats-latency-row">' +
            '<span class="stats-label">' +
            t('stream.statLatency') +
            '</span>' +
            '<span class="stats-value stats-latency">' +
            avgLatency +
            '</span>' +
            '</div>';

        html += '</div>';
        html += this._overlayCloseBtnHtml();

        this._overlayEl.innerHTML = html;
    }

    // ── Stats message handler (ping/pong + periodic backend stats) ─────────

    _handleStatsMessage(msg) {
        if (msg.type === 'rumble') {
            if (this._gamepadManager) this._gamepadManager.rumble(msg.index, msg.low, msg.high);
            return;
        }
        if (msg.type === 'clipboardcaps') {
            // Backend advertises clipboard sync (streamed host == backend
            // machine). Until this arrives, Ctrl+V is forwarded as a plain
            // keystroke like before.
            this._clipboardEnabled = !!msg.available;
            console.log(
                '[StreamView] Host clipboard sync ' +
                    (this._clipboardEnabled ? 'enabled' : 'disabled'),
            );
            return;
        }
        if (msg.type === 'clipboard') {
            if (typeof msg.text === 'string') this._applyHostClipboard(msg.text);
            return;
        }
        if (msg.type === 'pong') {
            const browserRtt = performance.now() - msg.ts;
            if (browserRtt > 0 && browserRtt < 10000) {
                this._browserRttStats.addSample(browserRtt);
            }
        } else if (msg.type === 'stats') {
            if (msg.hostRttMs !== undefined && msg.hostRttMs > 0) {
                this._hostRttStats.addSample(msg.hostRttMs);
            }
            if (msg.decodeLatencyUs !== undefined && msg.decodeLatencyUs > 0) {
                this._decodeLatencyStats.addSample(msg.decodeLatencyUs / 1000);
            }
            // Sunshine capture→encode latency, averaged backend-side over the
            // frames of the last stats window (moonlight-qt's "host processing
            // latency", from the RTP extension). 0 = not reported by the host.
            if (msg.hostProcMs !== undefined && msg.hostProcMs > 0) {
                this._hostProcStats.addSample(msg.hostProcMs);
            }
            // Backend SCTP backpressure drops (cumulative). Frames dropped
            // backend-side never get a frameId, so this is the only signal the
            // frontend has of the "keyframe slideshow" congestion regime.
            if (typeof msg.bpDrops === 'number') {
                if (msg.bpDrops > this._lastBpDrops) {
                    this._recordCongestionEvent(
                        'backend drops +' + (msg.bpDrops - this._lastBpDrops),
                    );
                }
                this._lastBpDrops = msg.bpDrops;
            }
        }
    }

    handleVideoFrame(data, isKeyframe, backendTs, frameId = 0) {
        // Stop processing frames once quit() has started.  The DC may still
        // deliver queued messages during the async HTTP /quit call.
        if (this._quitting) return;
        // Terminal decoder failure (e.g. broken AV1 in Safari): the stream
        // keeps delivering frames — drop them silently, the error is displayed.
        if (this._fatalDecodeError) return;

        if (data.length < 4) {
            console.warn('[StreamView] Video frame too small:', data.length);
            return;
        }

        // ── Stale frame detection (safety net) ───────────────────────────────
        // The video DataChannel is now ordered=true (SCTP reorders internally),
        // so frames normally arrive in send order. This filter remains as a
        // safety net for transports/paths that can still deliver out of order
        // (WS fallback races, historical unordered sessions): an older frame
        // (lower backendTs) decoded after a newer one would overwrite the canvas
        // with old pixels — the "ghosting" bug.
        //
        // We track the monotonic maximum backend timestamp.  Any frame with a
        // lower backendTs than the max is stale and is dropped, EXCEPT for
        // bootstrapping keyframes needed to configure the VideoDecoder for the
        // first time (they arrive early in the stream before _maxBackendTs is
        // meaningful).  Once the decoder is configured, all out-of-order frames
        // of any type are dropped.
        if (backendTs !== undefined && backendTs > 0) {
            if (this._maxBackendTs === undefined || backendTs > this._maxBackendTs) {
                this._maxBackendTs = backendTs;
            } else if (backendTs < this._maxBackendTs) {
                // Out-of-order: frame is older than the newest seen.
                // Recovery keyframes always pass when the reference is invalid —
                // recovery takes priority over anti-ghosting.
                if (isKeyframe && !this._referenceValid) {
                    // Let through to restore the reference
                } else if (this.decoderConfigured || !isKeyframe) {
                    // Decoder is configured or this isn't a keyframe — drop safely
                    this.stats.dropped++;
                    return;
                }
                // Keyframe before decoder config: let through to bootstrap decoder
            }
            // Equal timestamps: pass through (same-ms frames)
        }

        // Gap detection: frameId is provided by WebRtcDataChannel (optional).
        // A non-consecutive frameId means packets were lost — drop deltas until
        // the next keyframe restores the reference picture.
        // Disabled for WSS transport: TCP does not lose frames, so a frameId
        // discontinuity there is a false positive that would freeze the stream.
        if (this._transport !== 'wss' && frameId !== undefined && frameId !== 0) {
            // Forward jump = frames were lost or delayed — drop deltas until the
            // next keyframe restores the reference picture.
            if (this._lastFrameId !== -1 && frameId > this._lastFrameId + 1) {
                console.warn(
                    '[StreamView] Frame gap: expected ' +
                        (this._lastFrameId + 1) +
                        ' got ' +
                        frameId +
                        ' — invalidating reference, requesting IDR',
                );
                this._referenceValid = false;
                if (this._videoWorker) this._videoWorker.postMessage({ type: 'frameloss' });
                this._requestIdr('frame gap');
            }
            if (frameId > this._lastFrameId) {
                this._lastFrameId = frameId;
            } else if (this._lastFrameId !== -1 && !(isKeyframe && !this._referenceValid)) {
                // Late out-of-order frame (SCTP unordered delivery): a newer frame
                // was already submitted, so decoding this one would replay old
                // pixels after newer ones (back-and-forth "future frame" jumps).
                // frameId is assigned in backend send order, so it is authoritative;
                // the backendTs check above misses backend bursts that share one
                // timestamp. Recovery keyframes pass while the reference is invalid.
                this.stats.dropped++;
                return;
            }
        }

        // Track cumulative video bytes for bitrate calculation
        this._totalBytes += data.length;

        // Direct frame processing — no reordering.
        this._processVideoFrame(data, isKeyframe, backendTs);
    }

    /**
     * Process a single video frame (deliver to NAL parser / decoder).
     * Called in monotonically-increasing frameId order.
     */
    _processVideoFrame(data, isKeyframe, backendTs) {
        if (!this._firstFrameProcessed) {
            this._firstFrameProcessed = true;
            console.log(
                '[StreamView] First video frame: isKeyframe=' + isKeyframe,
                'size=' + data.length + ' codec=' + this.videoCodec,
            );
        }

        // Worker mode: hand the frame to the OffscreenCanvas worker for decode
        // + render. Transfer a fresh copy of the frame bytes (zero further copy)
        // so the buffer ownership moves to the worker without aliasing.
        if (this._useWorker) {
            if (this._videoWorker) {
                // Transfer the frame buffer to the worker (zero-copy). Reassembled
                // frames are standalone Uint8Arrays (byteOffset 0, full buffer), so
                // we can hand over data.buffer directly; otherwise copy the view.
                // The caller does not touch the bytes after this point.
                let buf;
                if (data.byteOffset === 0 && data.byteLength === data.buffer.byteLength) {
                    buf = data.buffer;
                } else {
                    buf = data.buffer.slice(data.byteOffset, data.byteOffset + data.byteLength);
                }
                this._videoWorker.postMessage({ type: 'frame', data: buf, isKeyframe, backendTs }, [
                    buf,
                ]);
            }
            return;
        }

        // AV1 pipeline: no NAL units, no SPS/PPS, no Annex B start codes.
        // OBUs are passed directly to the decoder.
        if (this.videoCodec === CODEC_AV1) {
            this.handleAv1Frame(data, isKeyframe);
            return;
        }

        // --- H.264 / HEVC pipeline ---

        // Extract SPS/PPS from the first keyframe if not done yet
        if (!this.nalParser.isReady()) {
            if (isKeyframe) {
                // If we requested an IDR, clear stale pending frames that reference
                // the old (lost) IDR. They would corrupt the decoder if fed with the
                // new SPS/PPS.
                if (this._idrRequested && this.pendingFrames.length > 0) {
                    console.log(
                        '[StreamView] IDR received, clearing ' +
                            this.pendingFrames.length +
                            ' stale pending frames',
                    );
                    this.pendingFrames = [];
                    this._idrRequested = false;
                }

                const ready = this.nalParser.feed(data);
                if (ready) {
                    this.configureDecoder();
                }
            } else {
                // First frame is not a keyframe — this is a problem
                console.warn('[StreamView] First frame is NOT a keyframe! Cannot extract SPS/PPS');

                // Request an IDR if we've buffered too many delta frames without decoder config.
                // Threshold: 30 frames (~0.5s at 60fps) without a keyframe (warn sampled).
                if (this.pendingFrames.length > 30) {
                    if (this.pendingFrames.length % 30 === 0) {
                        console.warn(
                            '[StreamView] No keyframe after ' +
                                this.pendingFrames.length +
                                ' frames, requesting IDR from backend',
                        );
                    }
                    this._requestIdr('no keyframe while buffering');
                }
            }
        }

        // Try to configure decoder if SPS/PPS just became available
        if (!this.decoderConfigured && this.nalParser.isReady()) {
            this.configureDecoder();
        }

        // Submit frame to decoder
        this.decodeFrame(data, isKeyframe, backendTs);
    }

    // --- AV1 pipeline ---

    handleAv1Frame(data, isKeyframe) {
        // On first keyframe, extract the Sequence Header OBU for decoder config
        // and immediately configure the decoder.
        if (!this.decoderConfigured && !this.decoderConfiguring) {
            if (isKeyframe) {
                // Discard deltas buffered BEFORE this keyframe: they have no
                // reference and would be submitted first after configure,
                // throwing "Key frame is required" (seen on Safari).
                if (this.pendingFrames.length > 0) {
                    console.log(
                        '[StreamView] AV1: keyframe received, clearing ' +
                            this.pendingFrames.length +
                            ' stale pending frames',
                    );
                    this.pendingFrames = [];
                }
                this._idrRequested = false;

                // Extract the Sequence Header OBU from the first keyframe (may be
                // absent — the decoder is then configured without a description).
                const seqHeader = findSequenceHeader(data);
                this.configureAv1Decoder(seqHeader || undefined);
            } else {
                // Wait for a keyframe before configuring — buffer until then
                if (this.pendingFrames.length < 120) {
                    this.pendingFrames.push({ data, isKeyframe });
                }

                // Request an IDR if we've buffered too many delta frames
                // (warn sampled: 1 line per 30 frames, not one per frame)
                if (this.pendingFrames.length > 30) {
                    if (this.pendingFrames.length % 30 === 0) {
                        console.warn(
                            '[StreamView] AV1: No keyframe after ' +
                                this.pendingFrames.length +
                                ' frames, requesting IDR',
                        );
                    }
                    this._requestIdr('AV1 no keyframe while buffering');
                }
                return;
            }
        }

        // Submit frame to AV1 decoder (no AVCC conversion needed)
        this.decodeAv1Frame(data, isKeyframe);
    }

    configureAv1Decoder(seqHeaderObu) {
        if (this.decoderConfigured || this.decoderConfiguring) return;
        if (!this.decoder) {
            console.warn('[StreamView] configureAv1Decoder: no decoder, calling setupDecoder()');
            this.setupDecoder();
        }
        this.decoderConfiguring = true;

        let configs = buildAv1DecoderConfigs(seqHeaderObu || null);
        if (this._hdrTonemap) {
            // HDR→SDR tone-map reads the YUV planes back (copyTo); hardware
            // decoders output opaque frames (format=null) that don't support it.
            // Same forcing as the HEVC/H264 path in configureDecoder().
            configs = configs.map((c) => ({ ...c, hardwareAcceleration: 'prefer-software' }));
        }

        const tryCodecs = (index) => {
            if (index >= configs.length) {
                console.error('[StreamView] All AV1 codec configs rejected');
                // Trigger the codec fallback chain (e.g. AV1 HDR → HEVC SDR)
                // instead of dead-ending on an error screen.
                this._requestCodecFallback();
                return;
            }

            const cfg = configs[index];
            VideoDecoder.isConfigSupported(cfg)
                .then((result) => {
                    if (result.supported) {
                        try {
                            this.decoder.configure(cfg);
                            this.decoderConfigured = true;
                            this.decoderConfiguring = false;
                            console.log(
                                '[StreamView] AV1 VideoDecoder configured: codec=' +
                                    cfg.codec +
                                    ', dequeuing ' +
                                    this.pendingFrames.length +
                                    ' pending frames',
                            );
                            // Drain pending frames using AV1 decoder path (not flushPendingFrames
                            // which calls toAvcc() and corrupts OBU data). The first
                            // submitted chunk MUST be a keyframe — skip leading deltas.
                            while (
                                this.pendingFrames.length > 0 &&
                                !this.pendingFrames[0].isKeyframe
                            ) {
                                this.pendingFrames.shift();
                                this.stats.dropped++;
                            }
                            while (this.pendingFrames.length > 0) {
                                const entry = this.pendingFrames.shift();
                                this.decodeAv1Frame(entry.data, entry.isKeyframe);
                            }
                        } catch (e) {
                            console.warn(
                                '[StreamView] AV1 applyConfig failed, trying next:',
                                e.message,
                            );
                            tryCodecs(index + 1);
                        }
                    } else {
                        tryCodecs(index + 1);
                    }
                })
                .catch((err) => {
                    console.warn(
                        '[StreamView] AV1 isConfigSupported error for codec=' + cfg.codec + ':',
                        err.message,
                    );
                    tryCodecs(index + 1);
                });
        };

        tryCodecs(0);
    }

    decodeAv1Frame(data, isKeyframe) {
        if (!this.decoderConfigured) {
            // Buffer until decoder is ready (limit to avoid OOM)
            if (this.pendingFrames.length < 120) {
                this.pendingFrames.push({ data, isKeyframe });
            }
            return;
        }

        // Belt-and-suspenders: decoder was nulled during cleanup
        if (!this.decoder) {
            console.warn('[StreamView] decodeAv1Frame: decoder is null, dropping frame');
            return;
        }

        const timestamp = this.frameCount * 16667;
        this.frameCount++;

        const type = isKeyframe ? 'key' : 'delta';

        // AV1 data is raw OBUs — no Annex B / AVCC conversion. Temporal
        // Delimiter and Padding OBUs must be stripped: the WebCodecs sample
        // format forbids them and Safari/VideoToolbox rejects the frame.
        const obuData = stripNonEssentialObus(data);
        try {
            const chunk = new EncodedVideoChunk({
                type: type,
                timestamp: timestamp,
                duration: 16667,
                data: obuData,
            });
            this._trackChunkSubmit(timestamp);
            this.decoder.decode(chunk);
            this.stats.received++;
        } catch (err) {
            console.error('[StreamView] decodeAv1Frame() error:', err.message, err);
            this.stats.dropped++;
        }
    }

    // =========================================================================
    // Input events
    // =========================================================================

    bindEvents() {
        // Lock keys (Num/Caps/Scroll) are toggles and the Moonlight protocol
        // carries no initial lock state, so the host can't know the client
        // started with e.g. NumLock on. Re-sync once per session on the first
        // real keyboard event (getModifierState requires a KeyboardEvent).
        this._locksSynced = false;
        // Load the physical-key → printed-label map so control combos match the
        // user's layout (AZERTY: KeyA → 'q') instead of the QWERTY position.
        // Chromium-only; Safari/Firefox fall back to e.key/e.code detection.
        this._loadKeyboardLayoutMap();
        // Common events (both modes)
        document.addEventListener('keydown', this._onKeyDown);
        document.addEventListener('keyup', this._onKeyUp);
        // Native paste event: the only permission-free way to read the local
        // clipboard (Ctrl/Cmd+V IS the user's consent). See handlePaste().
        document.addEventListener('paste', this._onPaste);
        // Any pointer gesture is an opportunity to flush a host-clipboard
        // write that was denied for lack of transient activation.
        document.addEventListener('pointerdown', this._onPointerDownFlush, true);
        this.inputEl.addEventListener('wheel', this._onWheel, { passive: false });
        this.streamEl.addEventListener('contextmenu', this._onContextMenu);
        // Touch events (mobile): bound to the WHOLE overlay so the entire
        // screen acts as a laptop trackpad, not just the canvas rectangle.
        this.streamEl.addEventListener('touchstart', this._onTouchStart, { passive: false });
        this.streamEl.addEventListener('touchmove', this._onTouchMove, { passive: false });
        this.streamEl.addEventListener('touchend', this._onTouchEnd, { passive: false });
        this.streamEl.addEventListener('touchcancel', this._onTouchEnd, { passive: false });
        window.addEventListener('beforeunload', this._onBeforeUnload);
        window.addEventListener('pagehide', this._onPageHide);
        window.addEventListener('blur', this._onWindowBlur);
        window.addEventListener('focus', this._onWindowFocus);
        document.addEventListener('visibilitychange', this._onVisibilityChange);
        // All platforms: keep Escape inside the host while in fullscreen.
        document.addEventListener('fullscreenchange', this._onFsChangeLock);

        // Mode-specific events
        if (this._gamingMode) {
            this._bindGamingEvents();
        } else {
            this._setupNormalMouse();
        }
    }

    /** True when the visible surface is the <video> element: webrtc-media
     *  (native RTP) or the HDR sink (MediaStreamTrackGenerator). In both cases
     *  the <canvas> is hidden, so coordinate mapping must use the <video>. */
    _videoIsDisplay() {
        return this._transport === 'webrtc-media' || this._useVideoSink;
    }

    /** Active display element used for absolute-coordinate mapping:
     *  the <video> when it is the visible surface, the <canvas> otherwise. */
    _displayEl() {
        return this._videoIsDisplay() ? this.videoEl : this.canvas;
    }

    /** Displayed media rectangle in client coordinates, accounting for
     *  object-fit: contain (letterbox/pillarbox bars). The <video> element box
     *  fills the whole canvas area, so its real content rect must be derived
     *  from the intrinsic video size; the <canvas> box already matches its
     *  content. Used so the cursor is hidden only over the actual image and
     *  coordinates map to the real picture, not the surrounding black bars. */
    _mediaRect() {
        const r = this._displayEl().getBoundingClientRect();
        const iw = this._videoIsDisplay() ? this.videoEl.videoWidth : this.canvas.width;
        const ih = this._videoIsDisplay() ? this.videoEl.videoHeight : this.canvas.height;
        if (!iw || !ih) return r;
        const scale = Math.min(r.width / iw, r.height / ih);
        const w = iw * scale,
            h = ih * scale;
        return {
            left: r.left + (r.width - w) / 2,
            top: r.top + (r.height - h) / 2,
            width: w,
            height: h,
        };
    }

    _bindGamingEvents() {
        document.addEventListener('pointerlockchange', this._onPointerLockChange);

        // Pre-focus: default cursor until the first move; the first mousemove
        // hides it over the picture (the host cursor tracks the position and is
        // rendered in the video, so showing the local cursor too looks double).
        this.inputEl.style.cursor = 'default';

        // Unified mousemove: absolute tracking when visible (pre-focus),
        // relative movement via pointer lock deltas when focused.
        this._onGamingMouseMove = (e) => {
            if (this._mouseFocused) {
                this.webrtc.send({ type: 'mousemove', dx: e.movementX, dy: e.movementY });
            } else {
                this._lastMouseClientX = e.clientX;
                this._lastMouseClientY = e.clientY;
                const rect = this._mediaRect();
                const rawX = e.clientX - rect.left;
                const rawY = e.clientY - rect.top;
                // Hide the local cursor over the actual picture — the host cursor
                // (which follows this absolute position) is already drawn in the
                // stream, so both showing at once looks like a double cursor.
                // Keep the arrow over the surrounding letterbox bars.
                const inside = rawX >= 0 && rawY >= 0 && rawX <= rect.width && rawY <= rect.height;
                if (!IS_TOUCH_DEVICE) {
                    this.inputEl.style.cursor = inside ? 'none' : 'default';
                }
                // Outside the picture (letterbox bars) → don't move the host cursor.
                if (!inside) return;
                const x = Math.round(Math.max(0, Math.min(rawX, rect.width)));
                const y = Math.round(Math.max(0, Math.min(rawY, rect.height)));
                const refW = Math.round(rect.width);
                const refH = Math.round(rect.height);
                this.webrtc.send({
                    type: 'mousemove',
                    x,
                    y,
                    referenceWidth: refW,
                    referenceHeight: refH,
                });
            }
        };
        this.inputEl.addEventListener('mousemove', this._onGamingMouseMove);

        // Click to capture focus: set host cursor at clicked position, then grab pointer.
        this._onGamingClick = (e) => {
            if (this._mouseFocused) return; // Already focused — click handled by mousedown/mouseup
            e.preventDefault();

            // Send absolute position so the host cursor teleports to the clicked point
            const rect = this._mediaRect();
            const x = Math.round(Math.max(0, Math.min(e.clientX - rect.left, rect.width)));
            const y = Math.round(Math.max(0, Math.min(e.clientY - rect.top, rect.height)));
            const refW = Math.round(rect.width);
            const refH = Math.round(rect.height);
            this.webrtc.send({
                type: 'mousemove',
                x,
                y,
                referenceWidth: refW,
                referenceHeight: refH,
            });
            this.inputEl.requestPointerLock();
        };
        this.inputEl.addEventListener('click', this._onGamingClick);

        // Mouse button events: only send when focused (pre-focus click only captures)
        this._onGamingMouseDown = (e) => {
            if (this._mouseFocused) this.handleMouseDown(e);
        };
        this._onGamingMouseUp = (e) => {
            if (this._mouseFocused) this.handleMouseUp(e);
        };
        this.inputEl.addEventListener('mousedown', this._onGamingMouseDown);
        this.inputEl.addEventListener('mouseup', this._onGamingMouseUp);
    }

    /** Hide the local arrow cursor when a client point sits over the streamed
     *  picture, show it over the letterbox bars. Shared by mousemove, mouseenter
     *  and the window-focus refresh so the decision is always consistent. */
    _updateLocalCursor(clientX, clientY) {
        if (IS_TOUCH_DEVICE || !this.inputEl) return;
        if (typeof clientX !== 'number' || typeof clientY !== 'number') return;
        const rect = this._mediaRect();
        const rawX = clientX - rect.left;
        const rawY = clientY - rect.top;
        const inside = rawX >= 0 && rawY >= 0 && rawX <= rect.width && rawY <= rect.height;
        this.inputEl.style.cursor = inside ? 'none' : 'default';
    }

    /** When the tab/window regains focus, the pointer may already sit over the
     *  streamed picture without emitting a mousemove — the local arrow would
     *  then stay visible on top of the host cursor (double cursor). Re-apply the
     *  hide/show decision from the last known pointer position. */
    _refreshLocalCursorOnFocus() {
        if (this._quitting || IS_TOUCH_DEVICE || !this.inputEl) return;
        // Gaming mode hides the cursor via pointer lock once focused: nothing to do.
        if (this._gamingMode && this._mouseFocused) return;
        // Chrome swallows cursor changes made while the window is unfocused but
        // still caches the value: mousemoves delivered to the unfocused window
        // set 'none' without effect, and every later 'none' is deduplicated, so
        // the local arrow never hides again (permanent double cursor). Force a
        // real value transition — visible cursor now, hide decision re-applied
        // on the next frame — so the update actually reaches the OS cursor.
        this.inputEl.style.cursor = 'default';
        requestAnimationFrame(() => {
            if (this._quitting || !this.inputEl) return;
            this._updateLocalCursor(this._lastMouseClientX, this._lastMouseClientY);
        });
    }

    _setupNormalMouse() {
        // In non-gaming mode, mouse position is sent in absolute coordinates
        // mapped to the host screen. The cursor on the host follows the client
        // cursor position on the video canvas 1:1.
        // The host cursor is visible in the video stream, so the local cursor
        // is hidden — but ONLY over the actual image, not the letterbox bars
        // around it (managed per-move below). On touch devices there is no CSS
        // cursor to hide (trackpad model).

        this._onNormalMouseMove = (e) => {
            // Remember where the pointer is so the cursor decision can be
            // re-applied on window focus (see _refreshLocalCursorOnFocus).
            this._lastMouseClientX = e.clientX;
            this._lastMouseClientY = e.clientY;

            const rect = this._mediaRect();
            // Absolute pixel position within the displayed image
            const rawX = e.clientX - rect.left;
            const rawY = e.clientY - rect.top;

            // Hide the local cursor only when it is over the actual picture;
            // show it over the surrounding black bars.
            const inside = rawX >= 0 && rawY >= 0 && rawX <= rect.width && rawY <= rect.height;
            if (!IS_TOUCH_DEVICE) {
                this.inputEl.style.cursor = inside ? 'none' : 'default';
            }

            // Over the letterbox bars (outside the picture): leave the host cursor
            // where it is — the client cursor is off the stream surface.
            if (!inside) return;

            // Clamp to image bounds to avoid sending out-of-range coordinates
            const x = Math.round(Math.max(0, Math.min(rawX, rect.width)));
            const y = Math.round(Math.max(0, Math.min(rawY, rect.height)));
            const refW = Math.round(rect.width);
            const refH = Math.round(rect.height);

            // Send absolute position. LiSendMousePositionEvent() on the backend
            // will scale (x, y) from the (refW, refH) plane to host screen coords.
            this.webrtc.send({
                type: 'mousemove',
                x: x,
                y: y,
                referenceWidth: refW,
                referenceHeight: refH,
            });
        };

        this._onNormalMouseDown = (e) => {
            this.handleMouseDown(e);
        };

        this._onNormalMouseUp = (e) => {
            this.handleMouseUp(e);
        };

        // Entering the area: decide the cursor straight away from the pointer
        // position (mouseenter carries client coords) instead of flashing the
        // arrow until the first mousemove — this also fixes the double cursor
        // when the pointer re-enters over the picture after regaining focus.
        this._onNormalMouseEnter = (e) => {
            this._lastMouseClientX = e.clientX;
            this._lastMouseClientY = e.clientY;
            this._updateLocalCursor(e.clientX, e.clientY);
        };

        this._onNormalMouseLeave = () => {
            if (!IS_TOUCH_DEVICE) {
                this.inputEl.style.cursor = 'default';
            }
        };

        this.inputEl.addEventListener('mousemove', this._onNormalMouseMove);
        this.inputEl.addEventListener('mousedown', this._onNormalMouseDown);
        this.inputEl.addEventListener('mouseup', this._onNormalMouseUp);
        this.inputEl.addEventListener('mouseenter', this._onNormalMouseEnter);
        this.inputEl.addEventListener('mouseleave', this._onNormalMouseLeave);
    }

    unbindEvents() {
        if (this._gamepadManager) this._gamepadManager.stop();
        document.removeEventListener('keydown', this._onKeyDown);
        document.removeEventListener('keyup', this._onKeyUp);
        document.removeEventListener('paste', this._onPaste);
        document.removeEventListener('pointerdown', this._onPointerDownFlush, true);
        if (this._pendingPasteKey) {
            clearTimeout(this._pendingPasteKey.timer);
            this._pendingPasteKey = null;
        }
        document.removeEventListener('pointerlockchange', this._onPointerLockChange);
        window.removeEventListener('beforeunload', this._onBeforeUnload);
        window.removeEventListener('pagehide', this._onPageHide);
        window.removeEventListener('blur', this._onWindowBlur);
        window.removeEventListener('focus', this._onWindowFocus);
        document.removeEventListener('visibilitychange', this._onVisibilityChange);
        document.removeEventListener('fullscreenchange', this._onFsChangeLock);
        // Release the keyboard lock if still held (e.g. quit while fullscreen).
        if (this._keyboardLocked && navigator.keyboard && navigator.keyboard.unlock) {
            try {
                navigator.keyboard.unlock();
            } catch (e) {}
            this._keyboardLocked = false;
        }
        if (this.streamEl) {
            this.streamEl.removeEventListener('contextmenu', this._onContextMenu);
            // Touch events (mobile) — bound to the whole overlay
            this.streamEl.removeEventListener('touchstart', this._onTouchStart);
            this.streamEl.removeEventListener('touchmove', this._onTouchMove);
            this.streamEl.removeEventListener('touchend', this._onTouchEnd);
            this.streamEl.removeEventListener('touchcancel', this._onTouchEnd);
            this._stopScrollMomentum();
        }
        if (this.inputEl) {
            this.inputEl.removeEventListener('wheel', this._onWheel);

            // Mode-specific listeners
            if (this._gamingMode) {
                if (this._onGamingMouseMove)
                    this.inputEl.removeEventListener('mousemove', this._onGamingMouseMove);
                if (this._onGamingClick)
                    this.inputEl.removeEventListener('click', this._onGamingClick);
                if (this._onGamingMouseDown)
                    this.inputEl.removeEventListener('mousedown', this._onGamingMouseDown);
                if (this._onGamingMouseUp)
                    this.inputEl.removeEventListener('mouseup', this._onGamingMouseUp);
            } else {
                if (this._onNormalMouseMove)
                    this.inputEl.removeEventListener('mousemove', this._onNormalMouseMove);
                if (this._onNormalMouseDown)
                    this.inputEl.removeEventListener('mousedown', this._onNormalMouseDown);
                if (this._onNormalMouseUp)
                    this.inputEl.removeEventListener('mouseup', this._onNormalMouseUp);
                if (this._onNormalMouseEnter)
                    this.inputEl.removeEventListener('mouseenter', this._onNormalMouseEnter);
                if (this._onNormalMouseLeave)
                    this.inputEl.removeEventListener('mouseleave', this._onNormalMouseLeave);
            }
        }
    }

    requestPointerLock() {
        if (!this.pointerLocked && this.inputEl) {
            this.inputEl.requestPointerLock();
        }
    }

    // Filler kept in the hidden capture <textarea> so Backspace always has
    // something to delete (iOS Safari emits no deletion event on an empty
    // field). Non-breaking spaces avoid soft-keyboard auto-capitalize/predict.

    // Maps KeyboardEvent.code (layout-independent physical key position)
    // to Windows Virtual Key codes.
    //
    // This is a fallback/supplement for browsers where e.keyCode may be
    // unreliable (deprecated in some specs). On Chrome/Edge, e.keyCode
    // already returns correct Windows VK codes — this table covers the
    // same keys so codeToWindowsVk(e.code) can be used as a fallback
    // when e.keyCode is 0.
    //
    // IMPORTANT: The VK codes here do NOT include the 0x8000 modifier bit
    // (VK_RAW). MoonlightShim::sendKeyEvent() adds 0x8000 internally.
    static codeToWindowsVk(code) {
        const map = {
            // Letters (A-Z) — VK_A=0x41 through VK_Z=0x5A
            KeyA: 0x41,
            KeyB: 0x42,
            KeyC: 0x43,
            KeyD: 0x44,
            KeyE: 0x45,
            KeyF: 0x46,
            KeyG: 0x47,
            KeyH: 0x48,
            KeyI: 0x49,
            KeyJ: 0x4a,
            KeyK: 0x4b,
            KeyL: 0x4c,
            KeyM: 0x4d,
            KeyN: 0x4e,
            KeyO: 0x4f,
            KeyP: 0x50,
            KeyQ: 0x51,
            KeyR: 0x52,
            KeyS: 0x53,
            KeyT: 0x54,
            KeyU: 0x55,
            KeyV: 0x56,
            KeyW: 0x57,
            KeyX: 0x58,
            KeyY: 0x59,
            KeyZ: 0x5a,
            // Digits — VK_0=0x30 through VK_9=0x39
            Digit1: 0x31,
            Digit2: 0x32,
            Digit3: 0x33,
            Digit4: 0x34,
            Digit5: 0x35,
            Digit6: 0x36,
            Digit7: 0x37,
            Digit8: 0x38,
            Digit9: 0x39,
            Digit0: 0x30,
            // Special keys
            Enter: 0x0d,
            Escape: 0x1b,
            Backspace: 0x08,
            Tab: 0x09,
            Space: 0x20,
            Minus: 0xbd,
            Equal: 0xbb,
            BracketLeft: 0xdb,
            BracketRight: 0xdd,
            Backslash: 0xdc,
            IntlBackslash: 0xe2, // VK_OEM_102 — ISO key; backend applies SS_KBE_FLAG_NON_NORMALIZED
            Semicolon: 0xba,
            Quote: 0xde,
            Backquote: 0xc0,
            Comma: 0xbc,
            Period: 0xbe,
            Slash: 0xbf,
            CapsLock: 0x14,
            // Function keys — VK_F1=0x70 through VK_F24=0x87
            F1: 0x70,
            F2: 0x71,
            F3: 0x72,
            F4: 0x73,
            F5: 0x74,
            F6: 0x75,
            F7: 0x76,
            F8: 0x77,
            F9: 0x78,
            F10: 0x79,
            F11: 0x7a,
            F12: 0x7b,
            F13: 0x7c,
            F14: 0x7d,
            F15: 0x7e,
            F16: 0x7f,
            F17: 0x80,
            F18: 0x81,
            F19: 0x82,
            F20: 0x83,
            F21: 0x84,
            F22: 0x85,
            F23: 0x86,
            F24: 0x87,
            // Navigation cluster
            PrintScreen: 0x2c,
            ScrollLock: 0x91,
            Pause: 0x13,
            Insert: 0x2d,
            Home: 0x24,
            PageUp: 0x21,
            Delete: 0x2e,
            End: 0x23,
            PageDown: 0x22,
            // Arrow keys
            ArrowRight: 0x27,
            ArrowLeft: 0x25,
            ArrowDown: 0x28,
            ArrowUp: 0x26,
            // Numpad
            NumLock: 0x90,
            NumpadDivide: 0x6f,
            NumpadMultiply: 0x6a,
            NumpadSubtract: 0x6d,
            NumpadAdd: 0x6b,
            NumpadEnter: 0x0d,
            Numpad1: 0x61,
            Numpad2: 0x62,
            Numpad3: 0x63,
            Numpad4: 0x64,
            Numpad5: 0x65,
            Numpad6: 0x66,
            Numpad7: 0x67,
            Numpad8: 0x68,
            Numpad9: 0x69,
            Numpad0: 0x60,
            NumpadDecimal: 0x6e,
            // Modifiers (physical position — logical state sent via ctrlKey etc.)
            ControlLeft: 0x11,
            ShiftLeft: 0x10,
            AltLeft: 0x12,
            MetaLeft: 0x5b,
            ControlRight: 0x11,
            ShiftRight: 0x10,
            AltRight: 0x12,
            MetaRight: 0x5c,
            // Context menu
            ContextMenu: 0x5d,
            // International
            IntlRo: 0x73, // JIS \ key — backend applies SS_KBE_FLAG_NON_NORMALIZED
            IntlYen: 0xff, // VK_OEM_AUTO (yen sign)
            Lang1: 0xf2, // VK_HANGUL
            Lang2: 0xf1, // VK_HANJA
            Lang3: 0xf4, // VK_KATAKANA
            Lang4: 0xf3, // VK_HIRAGANA
            Lang5: 0xf5, // VK_ZENKAKU
        };
        return map[code] !== undefined ? map[code] : 0;
    }

    // Cache navigator.keyboard.getLayoutMap() to resolve a physical key code
    // (e.code) to its printed label for the active layout. Used by combo
    // detection so Cmd+Option+Ctrl+{Q,X,Z,M} matches the labelled key on
    // AZERTY/other layouts rather than the QWERTY physical position. Refreshes
    // on layoutchange. No-op where the API is unavailable.
    async _loadKeyboardLayoutMap() {
        this._layoutMap = null;
        const kb = navigator.keyboard;
        if (!kb || typeof kb.getLayoutMap !== 'function') return;
        try {
            this._layoutMap = await kb.getLayoutMap();
            if (typeof kb.addEventListener === 'function') {
                kb.addEventListener('layoutchange', async () => {
                    try {
                        this._layoutMap = await kb.getLayoutMap();
                    } catch {
                        /* keep previous map */
                    }
                });
            }
        } catch {
            this._layoutMap = null;
        }
    }

    // Align the host's toggle-lock state with the client's. The Moonlight
    // protocol has no lock-state field, so we send the client's lock states
    // to the backend, which reads the host's REAL state (when the streamed
    // host is the backend machine) and taps only the locks that differ —
    // blindly tapping here turned locks OFF on hosts that booted with them
    // on. Runs once per session — subsequent presses pass through as normal
    // toggles. getModifierState only works on a real KeyboardEvent.
    _syncLockState(e) {
        this._locksSynced = true;
        if (typeof e.getModifierState !== 'function') return;
        // For the lock key being pressed right now, getModifierState already
        // reflects the post-toggle value while the host will still receive
        // this very keydown as a toggle — send the pre-toggle state so both
        // sides land on the same final value.
        const state = (name) =>
            e.code === name ? !e.getModifierState(name) : e.getModifierState(name);
        this.webrtc.send({
            type: 'locksync',
            numLock: state('NumLock'),
            capsLock: state('CapsLock'),
            scrollLock: state('ScrollLock'),
        });
    }

    handleKeyDown(e) {
        // Ignore all keyboard input while the stream is being shut down
        if (this._quitting) return;
        // Any keystroke is a user gesture: flush a host-clipboard write that
        // was denied for lack of transient activation (Safari/Firefox).
        if (this._pendingClipboardWrite) this._flushPendingClipboard();
        // Soft-keyboard events on the capture element are handled by its own
        // listeners (beforeinput/keydown) — don't double-process here.
        if (e.target === this._kbdCapture) return;

        // Ignore OS auto-repeat (e.repeat): a held key fires a burst of keydown
        // events. Forwarding them floods the host — and on a bad network the user
        // holds the key longer waiting for a frozen frame, so the queued repeats
        // all land at once (e.g. "améliiiiiorer"). Send a single keydown and let
        // the GUEST OS generate typematic repeat while the key stays down.
        if (e.repeat) {
            e.preventDefault();
            return;
        }

        // Sync lock keys to the host once, on the first real keyboard event.
        if (!this._locksSynced) this._syncLockState(e);

        // ── CSS fallback fullscreen Escape ─────────────────────────────────
        // When in CSS fake fullscreen (no native Fullscreen API), Escape
        // exits the CSS fullscreen rather than being forwarded to the host.
        if (this._cssFullscreen && e.key === 'Escape') {
            e.preventDefault();
            this._exitCssFallbackFullscreen();
            return;
        }

        // ── Ctrl/Cmd+V: local→host clipboard paste ─────────────────────────
        // When the backend shares the host clipboard, neither forward nor
        // preventDefault the paste chord: letting the browser run its default
        // action fires the native 'paste' event — the only permission-free
        // way to read the local clipboard. handlePaste() then ships the text
        // and the backend replays the chord AFTER committing the clipboard.
        // If no paste event follows (empty/non-text local clipboard), a short
        // timer forwards the swallowed keydown so host-side Ctrl+V still
        // pastes the HOST's own clipboard.
        if (
            this._clipboardEnabled &&
            (e.ctrlKey || e.metaKey) &&
            !e.altKey &&
            !e.shiftKey &&
            (e.key === 'v' || e.key === 'V')
        ) {
            if (this._pendingPasteKey) clearTimeout(this._pendingPasteKey.timer);
            this._pendingPasteKey = {
                code: e.code,
                keydownMsg: {
                    type: 'keydown',
                    keyCode: StreamView.codeToWindowsVk(e.code) || e.keyCode,
                    code: e.code,
                    key: e.key,
                    ctrlKey: e.ctrlKey,
                    shiftKey: e.shiftKey,
                    altKey: e.altKey,
                    metaKey: e.metaKey,
                },
                // Cmd-based paste (Mac): the host sees Cmd as the Win key, so
                // the backend must wrap the injected V with its own Ctrl.
                injectCtrl: e.metaKey && !e.ctrlKey,
                timer: setTimeout(() => this._sendPendingPasteKey(), 150),
            };
            return;
        }

        // ── Ctrl/Cmd+Alt+{Shift|Ctrl} combos ──
        //   Win: Ctrl+Alt+Shift+{Q,X,Z,M}
        //   Mac: Cmd+Option+Ctrl+{Q,X,Z,M}
        // Mac replaces Shift with Ctrl because Shift combos conflict with
        // system-level macOS keyboard shortcuts.
        const modCtrl = e.ctrlKey || e.metaKey;
        const isMac = /Mac/.test(navigator.platform);
        const modThird = isMac ? e.ctrlKey : e.shiftKey; // Ctrl on Mac, Shift elsewhere

        // ── Escape key ────────────────────────────────────────────────────
        // Prevent the browser from exiting fullscreen or releasing pointer
        // lock. The Fullscreen API defaults to exiting on Escape — we must
        // intercept it here so the key is forwarded to the host session as
        // a normal VK_ESCAPE keypress instead.
        if (e.key === 'Escape') {
            e.preventDefault();
            // In native fullscreen, Escape stays inside the host (Keyboard Lock
            // keeps it from exiting). Remind the user how to actually leave
            // fullscreen, since Escape no longer does it.
            if (document.fullscreenElement) {
                this._showFullscreenExitHint();
            }
            // Fall through to normal keydown handling below — VK_ESCAPE
            // (0x1B) will be sent to the host.
        }

        // ── Combo key detection ───────────────────────────────────────────
        // Dual detection: e.key (layout label) + e.code (physical position).
        //
        // Primary:   e.key — e.g. AZERTY Q key → e.key='q' → matches.
        // Fallback:  e.code — only used when e.key is NOT a simple a-z letter
        //            (AltGr on Windows can turn Q into ä on US-Intl layouts).
        //            This prevents false positives on AZERTY where pressing A
        //            at the physical KeyQ position would otherwise trigger Q.
        if (modCtrl && e.altKey && modThird) {
            // Resolve the printed label of the physical key. On macOS, holding
            // Option mangles e.key into a special char (Option+q → 'œ'), which
            // would force the e.code (QWERTY position) fallback and break AZERTY.
            // The layout map gives the real label (AZERTY: KeyA → 'q').
            const layoutLabel = this._layoutMap?.get(e.code);
            const k = (layoutLabel || e.key).toLowerCase();
            const c = e.code;
            // e.code fallback only when no layout map is available AND AltGr
            // altered e.key to a non-letter. With a layout map, k is reliable.
            const isLetter = /^[a-z]$/.test(k);
            const chk = (letter, code) =>
                k === letter || (!this._layoutMap && !isLetter && c === code);

            // Quit: Ctrl+Alt+Shift+Q (Win) / Cmd+Option+Ctrl+Q (Mac)
            if (chk('q', 'KeyQ')) {
                e.preventDefault();
                this._handleManualQuit();
                return;
            }
            // Fullscreen toggle: Ctrl+Alt+Shift+X (Win) / Cmd+Option+Ctrl+X (Mac)
            if (chk('x', 'KeyX')) {
                e.preventDefault();
                this.toggleFullscreen();
                return;
            }
            // Exit immersive: Ctrl+Alt+Shift+Z (Win) / Cmd+Option+Ctrl+Z (Mac)
            // Frees the mouse, releases the full keyboard lock and leaves
            // fullscreen — the single combo advertised by the overlay.
            if (chk('z', 'KeyZ')) {
                e.preventDefault();
                this._exitImmersive();
                return;
            }
            // Mouse mode toggle: Ctrl+Alt+Shift+M (Win) / Cmd+Option+Ctrl+M (Mac)
            if (chk('m', 'KeyM')) {
                e.preventDefault();
                this.toggleMouseMode();
                return;
            }

            // Block ANY other triple-modifier combo from reaching the host.
            // Ctrl+Alt+Shift (Win) / Cmd+Option+Ctrl (Mac) combos that are NOT
            // mapped above are still streaming-control territory — they must not
            // be forwarded as regular keystrokes to the remote host.
            e.preventDefault();
            return;
        }

        e.preventDefault();
        // Use physical-position-based VK code (like moonlight-qt's SDL scancode→VK).
        // e.code is always layout-independent ("KeyQ" = top-left letter row).
        // e.keyCode may be layout-dependent on some browsers (VK_A for 'a' on
        // AZERTY instead of VK_Q), which breaks Sunshine's layout correction.
        // Fall back to e.keyCode only for unmapped codes.
        const vkCode = StreamView.codeToWindowsVk(e.code) || e.keyCode;
        // Remember the matching keyup so we can release the key if focus is lost
        // before the real keyup fires. Modifier flags are cleared on release.
        this._heldPhysKeys.set(e.code || vkCode, {
            type: 'keyup',
            keyCode: vkCode,
            code: e.code,
            key: e.key,
        });
        this.webrtc.send({
            type: 'keydown',
            keyCode: vkCode,
            // Keep e.code for backend to detect IntlBackslash/IntlRo
            code: e.code,
            key: e.key,
            ctrlKey: e.ctrlKey,
            shiftKey: e.shiftKey,
            altKey: e.altKey,
            metaKey: e.metaKey,
        });
    }

    handleKeyUp(e) {
        if (e.target === this._kbdCapture) return;
        // Clipboard paste chord: the backend injected V down+up itself —
        // swallow the real keyup so the host doesn't get an unmatched release.
        if (this._suppressPasteKeyUpCode && e.code === this._suppressPasteKeyUpCode) {
            this._suppressPasteKeyUpCode = null;
            e.preventDefault();
            return;
        }
        // V released before its 'paste' event arrived: flush the swallowed
        // keydown now so the keyup below doesn't land unmatched on the host.
        if (this._pendingPasteKey && e.code === this._pendingPasteKey.code) {
            this._sendPendingPasteKey();
        }
        e.preventDefault();
        const vkCode = StreamView.codeToWindowsVk(e.code) || e.keyCode;
        this._heldPhysKeys.delete(e.code || vkCode);
        this.webrtc.send({
            type: 'keyup',
            keyCode: vkCode,
            code: e.code,
            key: e.key,
            ctrlKey: e.ctrlKey,
            shiftKey: e.shiftKey,
            altKey: e.altKey,
            metaKey: e.metaKey,
        });
    }

    // Release every physically-held key on the host. Called when the window
    // loses focus or goes hidden: the OS may eat the keyup (Win key → Start
    // menu, Alt+Tab), which would otherwise leave a modifier stuck on the host.
    _releaseAllPhysKeys() {
        if (!this._heldPhysKeys || this._heldPhysKeys.size === 0) return;
        for (const payload of this._heldPhysKeys.values()) {
            this.webrtc.send({
                ...payload,
                ctrlKey: false,
                shiftKey: false,
                altKey: false,
                metaKey: false,
            });
        }
        this._heldPhysKeys.clear();
    }

    // =========================================================================
    // Clipboard sync (see backend ClipboardBridge for the full protocol)
    // =========================================================================

    // Max text shipped per paste — mirrors ClipboardBridge::kMaxTextChars and
    // keeps the JSON message well under the input channel's message limits.
    static kMaxClipboardChars = 262144;

    /** Native 'paste' event following a Ctrl/Cmd+V swallowed by
     *  handleKeyDown. Ships the local clipboard text to the backend, which
     *  commits it to the host clipboard and injects the paste chord. */
    handlePaste(e) {
        // Mobile soft-keyboard textarea: its input-diff listener already
        // turns the paste into a 'textinput' message — don't double-handle.
        if (e.target === this._kbdCapture) return;
        const pending = this._pendingPasteKey;
        if (!pending) return; // not initiated by a swallowed Ctrl/Cmd+V
        e.preventDefault();
        let text = '';
        try {
            text = e.clipboardData ? e.clipboardData.getData('text/plain') : '';
        } catch (err) {
            /* clipboardData unavailable — fall back to a plain keystroke */
        }
        if (!text) {
            // Non-text local clipboard (image/files): forward the swallowed
            // keydown so the host pastes its own clipboard content instead.
            this._sendPendingPasteKey();
            return;
        }
        clearTimeout(pending.timer);
        this._pendingPasteKey = null;
        // The backend injects the complete V down+up — swallow the real keyup.
        this._suppressPasteKeyUpCode = pending.code;
        this.webrtc.send({
            type: 'clipboardpaste',
            text:
                text.length > StreamView.kMaxClipboardChars
                    ? text.slice(0, StreamView.kMaxClipboardChars)
                    : text,
            injectCtrl: pending.injectCtrl,
        });
    }

    /** Forward the Ctrl/Cmd+V keydown that handleKeyDown swallowed while
     *  waiting for a 'paste' event that brought no text (or never fired).
     *  The host then performs a regular paste of its OWN clipboard. */
    _sendPendingPasteKey() {
        const pending = this._pendingPasteKey;
        if (!pending) return;
        clearTimeout(pending.timer);
        this._pendingPasteKey = null;
        // Register for focus-loss auto-release like any forwarded keydown.
        this._heldPhysKeys.set(pending.code || pending.keydownMsg.keyCode, {
            type: 'keyup',
            keyCode: pending.keydownMsg.keyCode,
            code: pending.keydownMsg.code,
            key: pending.keydownMsg.key,
        });
        this.webrtc.send(pending.keydownMsg);
    }

    /** Host clipboard changed: mirror it into the local clipboard. Browsers
     *  may reject the write outside a user gesture (Safari always, Firefox
     *  when the last gesture is >5s old) — keep the text pending and retry
     *  on the next keystroke/pointer gesture. */
    _applyHostClipboard(text) {
        if (!text) return;
        this._pendingClipboardWrite = text;
        this._flushPendingClipboard();
    }

    _flushPendingClipboard() {
        const text = this._pendingClipboardWrite;
        if (text == null) return;
        if (!navigator.clipboard || !navigator.clipboard.writeText) {
            this._pendingClipboardWrite = null; // no async clipboard API — give up
            return;
        }
        navigator.clipboard.writeText(text).then(
            () => {
                if (this._pendingClipboardWrite === text) this._pendingClipboardWrite = null;
            },
            () => {
                /* denied (no activation / unfocused doc) — retry on next gesture */
            },
        );
    }

    handleMouseMove(e) {
        if (!this.pointerLocked) return;
        this.webrtc.send({ type: 'mousemove', dx: e.movementX, dy: e.movementY });
    }

    handleMouseDown(e) {
        this.webrtc.send({ type: 'mousedown', button: e.button + 1 });
    }

    handleMouseUp(e) {
        this.webrtc.send({ type: 'mouseup', button: e.button + 1 });
    }

    handleWheel(e) {
        e.preventDefault();
        // Browser deltaY is positive scrolling down; LiSendHighResScrollEvent
        // expects positive = up. Negate so standard wheels scroll the right way
        // (macOS "natural scrolling" already flips the sign at the OS level).
        this.webrtc.send({ type: 'mousewheel', delta: -e.deltaY });
    }

    // =========================================================================
    // Mobile orientation-based fullscreen (iOS Safari native player / Android)
    // =========================================================================

    /**
     * Set up a matchMedia listener for orientation changes on mobile devices.
     * Landscape → enters native fullscreen on the <video> element (webrtc-media mode).
     * Portrait  → exits fullscreen.
     *
     * Only activates on mobile/tablet platforms (detected via User-Agent in
     * BrowserDetect.js) to avoid triggering on touchscreen laptops (Surface, Chromebooks).
     * Silently ignored when the transport is not webrtc-media or when the video element
     * does not exist (DataChannel SCTP mode).
     */
    // =========================================================================
    // Virtual keyboard (touch devices)
    // =========================================================================

    handlePointerLockChange() {
        this.pointerLocked = document.pointerLockElement === this.inputEl;
        this._mouseFocused = this.pointerLocked;
        if (this.hintEl) {
            this.hintEl.style.display = this.pointerLocked ? 'none' : 'flex';
        }
        // Capturing/releasing the mouse drives both the full keyboard lock and
        // the immersive exit-reminder overlay.
        this._syncKeyboardLock();
        this._updateImmersiveOverlay();
    }

    // =========================================================================
    // Keyboard shortcut actions
    // =========================================================================

    /**
     * Toggle browser fullscreen mode for the streaming view.
     * Uses the Fullscreen API — requestFullscreen / exitFullscreen.
     */
    toggleFullscreen() {
        if (document.fullscreenElement) {
            document.exitFullscreen().catch((err) => {
                console.warn('[StreamView] exitFullscreen failed:', err.message);
            });
        } else if (this._cssFullscreen) {
            this._exitCssFallbackFullscreen();
        } else {
            this._requestFullscreen();
        }
    }

    /**
     * Toggle mouse mode between gaming (relative, pointer lock) and
     * desktop (absolute coordinates, no pointer lock).
     *
     * Gaming mode:   pointer-lock-based relative mouse, click to capture
     * Desktop mode:  absolute mouse coordinates, cursor visible, no capture
     *
     * Re-binds mouse event handlers for the new mode.
     */
    toggleMouseMode() {
        // ── Remove current mouse event listeners ─────────────────────────
        if (this._gamingMode) {
            if (this._onGamingMouseMove)
                this.inputEl.removeEventListener('mousemove', this._onGamingMouseMove);
            if (this._onGamingClick) this.inputEl.removeEventListener('click', this._onGamingClick);
            if (this._onGamingMouseDown)
                this.inputEl.removeEventListener('mousedown', this._onGamingMouseDown);
            if (this._onGamingMouseUp)
                this.inputEl.removeEventListener('mouseup', this._onGamingMouseUp);
        } else {
            if (this._onNormalMouseMove)
                this.inputEl.removeEventListener('mousemove', this._onNormalMouseMove);
            if (this._onNormalMouseDown)
                this.inputEl.removeEventListener('mousedown', this._onNormalMouseDown);
            if (this._onNormalMouseUp)
                this.inputEl.removeEventListener('mouseup', this._onNormalMouseUp);
            if (this._onNormalMouseEnter)
                this.inputEl.removeEventListener('mouseenter', this._onNormalMouseEnter);
            if (this._onNormalMouseLeave)
                this.inputEl.removeEventListener('mouseleave', this._onNormalMouseLeave);
        }

        // ── Exit pointer lock if leaving gaming mode ────────────────────
        if (this._gamingMode && document.pointerLockElement === this.inputEl) {
            document.exitPointerLock();
        }

        // ── Toggle mode ─────────────────────────────────────────────────
        this._gamingMode = !this._gamingMode;

        // ── Bind new mode's event handlers ──────────────────────────────
        if (this._gamingMode) {
            this._bindGamingEvents();
            // Show hint so user knows to click to capture
            if (this.hintEl) this.hintEl.style.display = 'flex';
        } else {
            this._setupNormalMouse();
            // Hide hint — no pointer lock in desktop mode
            if (this.hintEl) this.hintEl.style.display = 'none';
        }

        // Leaving immersive mode hides the exit reminder and drops the full
        // keyboard lock; entering it keeps the overlay hidden until capture.
        this._updateImmersiveOverlay();
        this._syncKeyboardLock();

        console.log(
            '[StreamView] Mouse mode toggled: ' +
                (this._gamingMode ? 'immersive (relative+lock)' : 'desktop (absolute)'),
        );

        Toast.info(
            t('stream.mouseMode', {
                mode: this._gamingMode ? t('stream.mouseModeGaming') : t('stream.mouseModeDesktop'),
            }),
        );
    }

    // =========================================================================
    // Status
    // =========================================================================

    setStatus(state, text) {
        // No-op — status indicators in the header were removed.
        // Connection state is conveyed through the stats overlay
        // (shown after first frame) or the "Waiting..." / "Connecting..."
        // state before the first frame arrives in _updateOverlay().
    }

    // =========================================================================
    // Keyboard shortcuts slide (startup overlay, 5s auto-hide)
    // =========================================================================

    /**
     * Build the shortcuts-slide HTML content, adapting modifier key labels
     * to the current platform (Windows vs macOS).
     */
    _buildShortcutsSlideContent() {
        // Touch devices have no physical keyboard: show a gesture cheat-sheet
        // (the trackpad model) instead of the keyboard-shortcut combos.
        if (IS_TOUCH_DEVICE) {
            this._buildTouchHelpContent();
            return;
        }
        const isMac = /Mac/.test(navigator.platform);
        const modA = isMac ? 'Cmd' : 'Ctrl'; // Primary modifier
        const modB = isMac ? 'Option' : 'Alt'; // Secondary modifier
        const modC = isMac ? 'Ctrl' : 'Shift'; // Tertiary modifier

        // Win order: Shift + Ctrl + Alt + ?
        // Mac order: Ctrl  + Option + Cmd + ?
        const comboWin = [modC, modA, modB];
        const comboMac = [modC, modB, modA];
        const comboMods = isMac ? comboMac : comboWin;

        const rows = [
            [t('stream.scQuit'), ...comboMods, 'Q'],
            [t('stream.scFullscreen'), ...comboMods, 'X'],
            [t('stream.scRelease'), ...comboMods, 'Z'],
            [t('stream.scMouseMode'), ...comboMods, 'M'],
        ];

        let html = '<div class="shortcuts-slide-title">' + t('stream.shortcutsTitle') + '</div>';
        html += '<div class="shortcuts-slide-grid">';
        for (const [action, ...keys] of rows) {
            html += '<div class="shortcut-row">';
            html += '<span class="shortcut-action">' + action + '</span>';
            html += '<span class="shortcut-keys">';
            for (let i = 0; i < keys.length; i++) {
                if (i > 0) html += '<span class="shortcut-plus">+</span>';
                html += '<kbd>' + keys[i] + '</kbd>';
            }
            html += '</span></div>';
        }
        html += '</div>';
        this._shortcutsSlide.innerHTML = html;
    }

    /**
     * Build the touch gesture cheat-sheet (mobile/tablet). Mirrors the
     * trackpad input model implemented in the touch handlers: relative
     * cursor, taps for clicks, multi-finger drags for scroll/zoom/pan.
     */
    _buildTouchHelpContent() {
        // Touch-screen mode changes the 1-finger meaning from a relative
        // trackpad move to a direct, absolute touch — reflect it in the help.
        const rows = this._touchScreen
            ? [
                  [t('stream.tcMoveCursor'), t('stream.tsMoveCursorVal')],
                  [t('stream.tcLeftClick'), t('stream.tsLeftClickVal')],
                  [t('stream.tcRightClick'), t('stream.tcRightClickVal')],
                  [t('stream.tcDrag'), t('stream.tsDragVal')],
                  [t('stream.tcScroll'), t('stream.tcScrollVal')],
                  [t('stream.tcZoom'), t('stream.tcZoomVal')],
                  [t('stream.tcPanZoom'), t('stream.tcPanZoomVal')],
                  [t('stream.tcKeyboard'), t('stream.tcKeyboardVal')],
              ]
            : [
                  [t('stream.tcMoveCursor'), t('stream.tcMoveCursorVal')],
                  [t('stream.tcLeftClick'), t('stream.tcLeftClickVal')],
                  [t('stream.tcRightClick'), t('stream.tcRightClickVal')],
                  [t('stream.tcDrag'), t('stream.tcDragVal')],
                  [t('stream.tcScroll'), t('stream.tcScrollVal')],
                  [t('stream.tcZoom'), t('stream.tcZoomVal')],
                  [t('stream.tcPanZoom'), t('stream.tcPanZoomVal')],
                  [t('stream.tcKeyboard'), t('stream.tcKeyboardVal')],
              ];

        const title = this._touchScreen ? t('stream.touchScreenTitle') : t('stream.touchTitle');
        let html = '<div class="shortcuts-slide-title">' + title + '</div>';
        html += '<div class="shortcuts-slide-grid">';
        for (const [action, gesture] of rows) {
            html += '<div class="shortcut-row">';
            html += '<span class="shortcut-action">' + action + '</span>';
            html += '<span class="shortcut-keys"><kbd class="gesture">' + gesture + '</kbd></span>';
            html += '</div>';
        }
        html += '</div>';
        this._shortcutsSlide.innerHTML = html;
    }

    /**
     * Show the shortcuts/gesture slide and set an auto-hide timer.
     * Safe to call multiple times — resets the timer each call.
     */
    _showShortcutsSlide() {
        // Only shown once per user-initiated launch. Transport relaunches
        // (congestion degradation / fallback chain) build a fresh StreamView
        // with _firstFrameRendered reset, which would otherwise re-pop the
        // slide on every degrade step — app.js sets this flag to suppress it.
        if (!this._shortcutsSlide || this._suppressShortcutsSlide) return;
        this._shortcutsSlide.classList.remove('fading-out');
        this._shortcutsSlide.style.display = '';

        if (this._shortcutsTimeout) {
            clearTimeout(this._shortcutsTimeout);
        }
        // Touch help has more rows to read than the keyboard combos.
        this._shortcutsTimeout = setTimeout(
            () => {
                this._hideShortcutsSlide();
            },
            IS_TOUCH_DEVICE ? 7000 : 4000,
        );
    }

    /**
     * Hide the shortcuts slide with a fast fade-out and clear the auto-hide
     * timer. The element is reused on the next show, so reset its state.
     */
    _hideShortcutsSlide() {
        if (this._shortcutsTimeout) {
            clearTimeout(this._shortcutsTimeout);
            this._shortcutsTimeout = null;
        }
        const slide = this._shortcutsSlide;
        if (!slide || slide.style.display === 'none' || slide.classList.contains('fading-out')) {
            return;
        }
        slide.classList.add('fading-out');
        slide.addEventListener(
            'animationend',
            () => {
                slide.style.display = 'none';
                slide.classList.remove('fading-out');
            },
            { once: true },
        );
    }

    // =========================================================================
    // Startup overlay (3-step connection status, centered)
    // =========================================================================

    /**
     * Advance the startup overlay to the given step and mark previous
     * steps as completed. Step 1 = connecting, step 2 = video starting,
     * step 3 = stream ready. After step 3, call _hideStartupOverlay().
     * @param {number} step - 1-indexed step to activate.
     */
    _updateStartupStep(step) {
        console.log(
            '[StreamView] _updateStartupStep(' +
                step +
                ') overlay=' +
                (this._startupOverlay ? 'present' : 'NULL'),
        );
        if (!this._startupOverlay) return;
        // The overlay defaults to display:none in CSS — reveal it while the
        // stream initializes (hidden again by _hideStartupOverlay after step 3).
        this._startupOverlay.classList.remove('hidden');
        this._startupOverlay.style.display = 'flex';
        const items = this._startupOverlay.querySelectorAll('.startup-step');
        items.forEach((el) => {
            const idx = parseInt(el.getAttribute('data-step'), 10);
            el.classList.remove('active', 'done');
            if (idx < step) {
                el.classList.add('done');
            } else if (idx === step) {
                // Step 3 is the final state — mark as done (green), not active (yellow pulse)
                el.classList.add(step >= 3 ? 'done' : 'active');
            }
        });

        // Step 3 == first frame == stream arrival → fire the boot reveal.
        // Guard against a reveal exception bubbling up and aborting the
        // caller's _hideStartupOverlay() scheduling (overlay stuck on step 2).
        if (step >= 3) {
            try {
                this._playStreamReveal();
            } catch (e) {
                console.warn('[StreamView] _playStreamReveal failed:', e);
            }
        }
    }

    /**
     * Cyberpunk boot reveal, synchronized with the first decoded frame.
     * Plays once: a cyan scan-beam sweeps the screen while a brief RGB glitch
     * and HUD corner brackets converge, then everything fades to leave the
     * live stream. pointer-events:none, so it never blocks the Stop button.
     */
    _playStreamReveal() {
        if (this._revealPlayed || !this._revealEl) return;
        this._revealPlayed = true;
        const el = this._revealEl;
        // Size the reveal to the actual frame rectangle (object-fit: contain
        // letterboxes the image inside the area) so the boot animation covers
        // only the streamed image, not the surrounding black bars.
        this._fitRevealToFrame();
        el.style.display = 'block';
        // Force reflow so the animation always (re)starts from frame 0.
        void el.offsetWidth;
        el.classList.add('playing');
        setTimeout(() => {
            if (!el) return;
            el.classList.remove('playing');
            el.style.display = 'none';
        }, 1000);
    }

    /**
     * Position the reveal overlay to match the letterboxed frame rectangle.
     * object-fit: contain centers the image and leaves black bars; the reveal
     * must align with the image only, not the full canvas area.
     */
    _fitRevealToFrame() {
        const el = this._revealEl;
        if (!el || !this.canvasArea) return;
        const rect = this.canvasArea.getBoundingClientRect();
        const disp = this._displayEl();
        let iw = 0,
            ih = 0;
        if (this._videoIsDisplay() && disp) {
            iw = disp.videoWidth;
            ih = disp.videoHeight;
        } else if (this.canvas) {
            iw = this.canvas.width;
            ih = this.canvas.height;
        }
        if (iw > 0 && ih > 0 && rect.width > 0 && rect.height > 0) {
            // Restrict the reveal to the frame's height only; width stays 100%.
            const fit = Math.min(rect.width / iw, rect.height / ih);
            const imgH = ih * fit;
            el.style.inset = 'auto';
            el.style.left = '0';
            el.style.width = '100%';
            el.style.top = (rect.height - imgH) / 2 + 'px';
            el.style.height = imgH + 'px';
        } else {
            // Fallback: no known frame size → cover the whole area.
            el.style.inset = '0';
            el.style.left = el.style.top = el.style.width = el.style.height = '';
        }
    }

    /**
     * Fade out and hide the startup overlay.
     * Called ~0.5s after the first video frame is decoded (step 3).
     */
    _hideStartupOverlay() {
        const domCount = document.querySelectorAll('#stream-startup-overlay').length;
        const inDom = this._startupOverlay && document.body.contains(this._startupOverlay);
        console.log(
            '[StreamView] _hideStartupOverlay called, overlay=' +
                (this._startupOverlay ? 'present' : 'NULL') +
                ' inDom=' +
                inDom +
                ' domCount=' +
                domCount,
        );
        if (!this._startupOverlay) return;
        this._startupOverlay.classList.add('hidden');
        // Remove from DOM after the CSS transition completes
        setTimeout(() => {
            if (this._startupOverlay) {
                this._startupOverlay.style.display = 'none';
                console.log('[StreamView] _hideStartupOverlay: display=none applied');
            }
        }, 500);
    }

    // =========================================================================
    // Quit / Cleanup
    // =========================================================================

    /**
     * The session was taken over by another device. Show a 2s cyberpunk
     * "connection terminated" transition, then quit. Guarded so the generic
     * onClose disconnect path stays silent.
     */
    _handleTakeover() {
        this._handleForcedExit(t('stream.takenOverTitle'), t('stream.takenOverBody'));
    }

    /**
     * This device's access was revoked by the admin mid-stream. The backend
     * already tore down the relay and the Sunshine session, so exit locally
     * without calling /quit (the session cookie is dead — it would 401).
     */
    _handleRevoked() {
        this._handleForcedExit(t('stream.revokedTitle'), t('stream.revokedBody'));
    }

    /**
     * Backend-initiated exit (take-over / revocation): show the 2s cyberpunk
     * "connection terminated" transition, then quit locally (no backend /quit).
     */
    _handleForcedExit(title, body) {
        if (this._quitting || this._takenOver) return;
        this._takenOver = true;
        this.connected = false;

        // Full-screen glitch overlay (CP2077 style — see stream.css).
        const el = document.createElement('div');
        el.className = 'stream-takeover-overlay';
        el.innerHTML =
            '<div class="takeover-scanlines"></div>' +
            '<div class="takeover-box">' +
            '<div class="takeover-title" data-text="' +
            title +
            '">' +
            title +
            '</div>' +
            '<div class="takeover-sub">' +
            body +
            '</div>' +
            '<div class="takeover-bar"><span></span></div>' +
            '</div>';
        const root = this._rootEl || document.body;
        root.appendChild(el);
        // Trigger the close-in animation on the next frame.
        requestAnimationFrame(() => el.classList.add('is-active'));

        // After the transition, power off the "screen", then quit.
        setTimeout(() => {
            try {
                el.classList.add('is-closing');
            } catch (e) {}
            this._playPowerOff(() => this.quit({ takenOver: true }));
        }, 2000);
    }

    /**
     * User pressed Stop. Play a short cyberpunk "disconnecting" transition
     * (clean-exit variant — signature yellow/cyan, no alarm magenta), then
     * quit normally (backend /quit). Guarded so a double-tap is a no-op.
     */
    _handleManualQuit() {
        if (this._quitting || this._takenOver || this._manualQuitting) return;
        this._manualQuitting = true;
        this.connected = false;

        // Pre-build the apps view in the background during the ~1.8s exit
        // animation, so its content is already there once the animation ends.
        if (this.onQuitStart) {
            try {
                this.onQuitStart();
            } catch (e) {}
        }

        // Full-screen glitch overlay (CP2077 style — see stream.css).
        const el = document.createElement('div');
        el.className = 'stream-takeover-overlay is-quit';
        const title = t('stream.disconnectTitle');
        el.innerHTML =
            '<div class="takeover-scanlines"></div>' +
            '<div class="takeover-box">' +
            '<div class="takeover-title" data-text="' +
            title +
            '">' +
            title +
            '</div>' +
            '<div class="takeover-sub">' +
            t('stream.disconnectBody') +
            '</div>' +
            '<div class="takeover-bar"><span></span></div>' +
            '</div>';
        const root = this._rootEl || document.body;
        root.appendChild(el);
        requestAnimationFrame(() => el.classList.add('is-active'));

        // Shorter than take-over (1.2s deplete) — voluntary, friendly exit.
        // Then power off the "screen" like an old CRT terminal before quitting.
        setTimeout(() => {
            try {
                el.classList.add('is-closing');
            } catch (e) {}
            this._playPowerOff(() => this.quit());
        }, 1200);
    }

    /**
     * CRT power-off transition — collapses the screen into a bright phosphor
     * scan line, then a single dot, like an old terminal monitor losing power.
     * Cyberpunk-tinted (cyan/yellow phosphor over green). Calls `done` once the
     * ~600ms animation completes (guarded so it fires exactly once).
     */
    _playPowerOff(done) {
        let fired = false;
        const finish = () => {
            if (fired) return;
            fired = true;
            try {
                done();
            } catch (e) {}
        };

        const crt = document.createElement('div');
        crt.className = 'crt-poweroff';
        // Scope the phosphor line to the frame area and collapse the real frame
        // (canvas + video) with it, so the streamed image implodes into the
        // line instead of staying fully visible behind an overlay.
        const area = this.canvasArea;
        if (area) {
            area.appendChild(crt);
        } else {
            (this._rootEl || document.body).appendChild(crt);
        }

        crt.addEventListener('animationend', finish, { once: true });
        requestAnimationFrame(() => {
            crt.classList.add('is-active');
            if (area) area.classList.add('crt-collapsing');
        });
        // Fallback in case animationend doesn't fire (e.g. reduced motion).
        setTimeout(finish, 700);
    }

    async quit(opts = {}) {
        // silent: suppress the "Stream end" toast (used by transport fallback
        // relaunch, which shows its own warning toast instead).
        const silent = opts.silent === true;
        // takenOver: our Sunshine session was already reclaimed by another
        // device — do NOT call the backend /quit (it acts on the GLOBAL active
        // relay, which is now the new owner's session: quitting would kill it).
        const takenOver = opts.takenOver === true;
        // retire: seamless quality switching promoted ANOTHER live view — this
        // one steps down quietly: keep the display state (fullscreen) for the
        // successor and never touch the successor's backend slot.
        const retire = opts.retire === true;
        // Guard: prevent re-entrant calls (e.g. from WS onClose -> setTimeout)
        if (this._quitting) return;
        this._quitting = true;

        // Exit fullscreen if active (before unbinding events).
        // Covers both standard Fullscreen API and iOS webkitExitFullscreen.
        // Also exit CSS fallback fullscreen if active.
        if (!retire) {
            this._exitCssFallbackFullscreen();
            this._exitMobileFullscreen();
            if (document.fullscreenElement) {
                document.exitFullscreen().catch(() => {});
            }
        }

        this.stopRenderLoop();
        this.unbindEvents();

        // Tear down the video worker (if active): stop its pipeline, then
        // terminate so the decoder/OffscreenCanvas are released.
        if (this._videoWorker) {
            try {
                this._videoWorker.postMessage({ type: 'stop' });
            } catch (e) {}
            try {
                this._videoWorker.terminate();
            } catch (e) {}
            this._videoWorker = null;
        }

        // Stop tracking output size and release the renderer (GPU resources).
        if (this._resizeObserver) {
            try {
                this._resizeObserver.disconnect();
            } catch (e) {}
            this._resizeObserver = null;
        }
        if (this._renderer) {
            try {
                this._renderer.dispose();
            } catch (e) {}
            this._renderer = null;
        }

        // Clear stats overlay timer
        if (this._overlayInterval) {
            clearInterval(this._overlayInterval);
            this._overlayInterval = null;
        }
        if (this._noVideoTimer) {
            clearTimeout(this._noVideoTimer);
            this._noVideoTimer = null;
        }
        if (this._overlayEl) {
            this._overlayEl.style.display = 'none';
        }

        // Hide shortcuts slide immediately (session is ending)
        this._hideShortcutsSlide();

        // Remove the transient hint (fullscreen-exit tip) and its timer.
        if (this._transientHintTimer) {
            clearTimeout(this._transientHintTimer);
            this._transientHintTimer = null;
        }
        if (this._transientHintEl) {
            this._transientHintEl.remove();
            this._transientHintEl = null;
        }

        // Hide startup overlay if still visible
        this._hideStartupOverlay();

        // Clear ping timer
        if (this._pingInterval) {
            clearInterval(this._pingInterval);
            this._pingInterval = null;
        }
        this._stopMediaStatsPolling();

        // Clear IDR request timer
        if (this._idrTimeout) {
            clearTimeout(this._idrTimeout);
            this._idrTimeout = null;
        }

        // Remove mobile orientation fullscreen listener
        if (this._orientationMql && this._onOrientationChange) {
            this._orientationMql.removeEventListener('change', this._onOrientationChange);
            this._orientationMql = null;
            this._onOrientationChange = null;
        }

        // Clean up fullscreen change listener
        if (this._onFullscreenChange) {
            document.removeEventListener('fullscreenchange', this._onFullscreenChange);
            if (this.videoEl) {
                this.videoEl.removeEventListener('webkitbeginfullscreen', this._onFullscreenChange);
                this.videoEl.removeEventListener('webkitendfullscreen', this._onFullscreenChange);
            }
            this._onFullscreenChange = null;
        }

        // Remove mobile fullscreen button
        if (this._mobileFsBtn) {
            this._mobileFsBtn.remove();
            this._mobileFsBtn = null;
        }

        // Remove virtual keyboard button + capture, drop viewport listeners
        this._clearLongPress();
        if (this._onViewportResize && window.visualViewport) {
            window.visualViewport.removeEventListener('resize', this._onViewportResize);
            window.visualViewport.removeEventListener('scroll', this._onViewportResize);
            this._onViewportResize = null;
        }
        if (this._onWindowScroll) {
            window.removeEventListener('scroll', this._onWindowScroll);
            this._onWindowScroll = null;
        }
        if (this._onDocKeepFocus) {
            document.removeEventListener('touchstart', this._onDocKeepFocus, { capture: true });
            document.removeEventListener('mousedown', this._onDocKeepFocus, { capture: true });
            this._onDocKeepFocus = null;
        }
        if (this._kbdCapture) {
            this._kbdCapture.blur();
            this._kbdCapture.remove();
            this._kbdCapture = null;
        }
        if (this._kbdBtn) {
            this._kbdBtn.remove();
            this._kbdBtn = null;
        }
        if (this._kbToolbar) {
            this._kbToolbar.remove();
            this._kbToolbar = null;
        }
        if (this.streamEl) {
            this.streamEl.style.height = '';
            this.streamEl.style.top = '';
            this.streamEl.style.bottom = '';
        }

        if (document.pointerLockElement === this.inputEl) {
            document.exitPointerLock();
        }

        // CRITICAL: Mark decoder not configured BEFORE nulling it. During the
        // await below the WS is still open and frames may arrive.  Without this,
        // decodeFrame() sees decoderConfigured=true but decoder=null -> crash.
        this.decoderConfigured = false;
        this.nalParser.reset();

        // Close VideoDecoder
        if (this.decoder) {
            try {
                this.decoder.close();
            } catch (e) {
                /* ignore */
            }
            this.decoder = null;
        }

        // Close AudioPipeline
        if (this.audioPipeline) {
            this.audioPipeline.close();
        }

        // Close any pending frames
        for (const frame of this.frameQueue) {
            try {
                frame.close();
            } catch (e) {
                /* ignore */
            }
        }
        this.frameQueue = [];
        this.pendingFrames = [];

        // Mark WebRTC as stopping before calling /quit so that DataChannel
        // "onclose" events from the backend closing its side are not treated
        // as unexpected errors.  Then send the HTTP request while the relay
        // is still reachable.
        this.webrtc.markStopping();

        if (takenOver) {
            // Session already reclaimed — just close our transport locally.
            this.webrtc.close();
        } else {
            try {
                // Scope the backend /quit to THIS view's slot + uniqueid so a
                // dual-stream sibling keeps streaming untouched. Retiring also
                // keeps the Sunshine app session alive (sessions share the
                // running app — a /cancel would kill the successor's stream).
                const quitExtras = { session_slot: this._sessionSlot };
                if (this._slotUniqueId) quitExtras.client_uniqueid = this._slotUniqueId;
                if (retire) quitExtras.keep_host_session = true;
                await BackendClient.quitApp(this.host.uuid, quitExtras);
                this.webrtc.close();
                if (!silent) {
                    await Toast.dismissAll();
                    Toast.success(t('stream.streamEnd'));
                }
            } catch (err) {
                console.warn('[StreamView] Quit failed:', err);
                this.webrtc.close();
            }
        }

        this.destroy();

        // Notify MoonlightApp that streaming ended (restores apps/hosts view).
        if (this.onQuit) {
            const cb = this.onQuit;
            this.onQuit = null; // Fire once
            cb();
        }
    }

    destroy() {
        this._exitCssFallbackFullscreen();
        this._releaseWakeLock();
        this.stopRenderLoop();
        this.unbindEvents();
        this.webrtc.close();

        if (this._pingInterval) {
            clearInterval(this._pingInterval);
            this._pingInterval = null;
        }
        this._stopMediaStatsPolling();

        if (this._idrTimeout) {
            clearTimeout(this._idrTimeout);
            this._idrTimeout = null;
        }

        // Remove mobile orientation fullscreen listener
        if (this._orientationMql && this._onOrientationChange) {
            this._orientationMql.removeEventListener('change', this._onOrientationChange);
            this._orientationMql = null;
            this._onOrientationChange = null;
        }

        // Clean up fullscreen change listener
        if (this._onFullscreenChange) {
            document.removeEventListener('fullscreenchange', this._onFullscreenChange);
            if (this.videoEl) {
                this.videoEl.removeEventListener('webkitbeginfullscreen', this._onFullscreenChange);
                this.videoEl.removeEventListener('webkitendfullscreen', this._onFullscreenChange);
            }
            this._onFullscreenChange = null;
        }

        // Remove mobile fullscreen button
        if (this._mobileFsBtn) {
            this._mobileFsBtn.remove();
            this._mobileFsBtn = null;
        }

        // Remove virtual keyboard button + capture, drop viewport listeners
        this._clearLongPress();
        if (this._onViewportResize && window.visualViewport) {
            window.visualViewport.removeEventListener('resize', this._onViewportResize);
            window.visualViewport.removeEventListener('scroll', this._onViewportResize);
            this._onViewportResize = null;
        }
        if (this._onWindowScroll) {
            window.removeEventListener('scroll', this._onWindowScroll);
            this._onWindowScroll = null;
        }
        if (this._onDocKeepFocus) {
            document.removeEventListener('touchstart', this._onDocKeepFocus, { capture: true });
            document.removeEventListener('mousedown', this._onDocKeepFocus, { capture: true });
            this._onDocKeepFocus = null;
        }
        if (this._kbdCapture) {
            this._kbdCapture.blur();
            this._kbdCapture.remove();
            this._kbdCapture = null;
        }
        if (this._kbdBtn) {
            this._kbdBtn.remove();
            this._kbdBtn = null;
        }
        if (this._kbToolbar) {
            this._kbToolbar.remove();
            this._kbToolbar = null;
        }
        if (this.streamEl) {
            this.streamEl.style.height = '';
            this.streamEl.style.top = '';
            this.streamEl.style.bottom = '';
        }

        if (document.pointerLockElement === this.inputEl) {
            document.exitPointerLock();
        }

        if (this.decoder) {
            try {
                this.decoder.close();
            } catch (e) {
                /* ignore */
            }
            this.decoder = null;
        }
        this.decoderConfigured = false;
        this.nalParser.reset();

        if (this.audioPipeline) {
            this.audioPipeline.close();
        }

        for (const frame of this.frameQueue) {
            try {
                frame.close();
            } catch (e) {
                /* ignore */
            }
        }
        this.frameQueue = [];
        this.pendingFrames = [];

        const el = this._rootEl;
        if (el) el.remove();

        // Restore the underlying app now that the stream overlay is gone.
        document.body.classList.remove('streaming-active');
    }
}

// Mix the keyboard + touch input subsystems into StreamView.prototype. Their
// methods live in separate files for readability but operate on the StreamView
// instance (`this`). Class prototype methods are non-enumerable, so copy them
// explicitly (Object.assign would skip them).
for (const Mixin of [StreamViewKeyboard, StreamViewTouch, StreamViewFullscreen]) {
    for (const name of Object.getOwnPropertyNames(Mixin.prototype)) {
        if (name === 'constructor') continue;
        Object.defineProperty(
            StreamView.prototype,
            name,
            Object.getOwnPropertyDescriptor(Mixin.prototype, name),
        );
    }
}
