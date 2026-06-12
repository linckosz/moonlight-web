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
    CODEC_HEVC
} from '../util/Mp4Muxer.js';
import {
    findSequenceHeader,
    buildAv1DecoderConfigs,
    CODEC_AV1
} from '../util/Av1Utils.js';

import { IS_TOUCH_DEVICE, IS_MOBILE_OR_TABLET } from '../util/BrowserDetect.js';

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
        this._samples = this._samples.filter(s => s.time > cutoff);
    }
}

export class StreamView {
    constructor(container, signalingUrl, host, videoCodec, gamingMode = true, upnpEnabled = true, upnpAvailable = true, transport = 'webrtc', transportMode = undefined, isRemote = false, showPerformanceStats = true) {
        this.container = container;
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
            this.webrtc = new WebRtcDataChannel(signalingUrl, { wssMode: true, wssFragmented: false });
        } else {
            this.webrtc = new WebRtcDataChannel(signalingUrl);
        }
        this.pointerLocked = false;
        /** Gaming mode focus state: true when pointer lock is active (cursor captured).
         *  false initially (cursor visible, absolute mouse tracking).
         *  Set to true on first click, reset when pointer lock is lost. */
        this._mouseFocused = false;

        // Audio pipeline (PCM16 -> AudioWorklet -> speakers)
        this.audioPipeline = new AudioPipeline();
        this._audioLogged = false;

        // WebCodecs
        this.decoder = null;
        this.decoderConfigured = false;
        this.decoderConfiguring = false;
        this.nalParser = new NalParser();
        this.frameQueue = [];
        this.pendingFrames = [];    // frames buffered before decoder config
        this.frameCount = 0;
        this.renderRunning = false;
        this._rendering = false;     // Guard: prevents overlapping GPU render ops
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
        this._resolution = '';              // "1920x1080" — set once on first frame
        this._codec = this.videoCodec;      // Same as videoCodec
        this._transport = transport;        // "webrtc" or "wss"
        this._totalBytes = 0;               // Cumulative video bytes for bitrate
        this._startTime = performance.now();// Stream start time for bitrate calc
        this._fpsTimestamps = [];           // performance.now() per decoded frame
        this._latencySamples = [];          // { time: perfNow, latency: ms }

        // ── Stats overlay ───────────────────────────────────────────────────
        this._hostRttStats = new SlidingStats(5000);          // backend ↔ Sunshine one-way (ms)
        this._browserRttStats = new SlidingStats(5000);       // browser ↔ backend RTT (ms)
        this._decodeLatencyStats = new SlidingStats(5000);    // backend pipeline latency (ms)
        this._e2eLatencyStats = new SlidingStats(5000);       // Sunshine capture → canvas display (ms)
        this._streamTimeMs = 0;                              // Last known steady_clock ms from backend stats
        this._streamTimeReceiptTime = 0;                     // performance.now() when _streamTimeMs was received
        this._steadyToPerfOffset = null;                     // perfTime = steadyTime - offset (set once)
        this._pingSeq = 0;
        this._pingInterval = null;

        // Forward stats/pong messages from backend to the stats overlay system.
        // Set before setupWebRtc() so it's active when connect() is called.
        this.webrtc.onStats = (msg) => this._handleStatsMessage(msg);

        // Bound handlers
        this._onKeyDown = (e) => this.handleKeyDown(e);
        this._onKeyUp = (e) => this.handleKeyUp(e);
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

        // Touch state
        this._touchActive = false;
        this._touchStartX = 0;
        this._touchStartY = 0;
        this._touchLastX = 0;
        this._touchLastY = 0;
        this._touchStartTime = 0;
        this._touchFingerCount = 0;
        this._touchTapThreshold = 10;        // px max distance for a tap (vs drag)
        this._touchTapTimeThreshold = 300;   // ms max duration for a tap
        this._touchHadTwoFingers = false;     // true if 2 fingers were active during the current touch sequence

        // ── Touch cursor (mobile overlay) ──────────────────────────────────
        /** Overlay cursor diameter in CSS pixels — large enough for mobile
         *  visibility while leaving most of the content uncovered. */
        this._touchCursorSize = 44;
        /** Reference to the DOM overlay element shown on touch. */
        this._touchCursorEl = null;
        /** Auto-hide timer handle for touch cursor. */
        this._touchCursorTimeout = null;
        /** How long (ms) the cursor stays visible after the last touch ends.
         *  The user needs to see where the pointer is between gestures. */
        this._touchCursorHideDelay = 3000;

        // DEBUG: orientation debug overlay state — remove after diagnosis
        this._debugOrientationEl = null;
        this._debugOrientationInterval = null;
        this._debugFullscreenSucceeded = false;
        this._debugFullscreenFailed = false;
        // DEBUG end

        // Mobile fullscreen button state
        this._mobileFsBtn = null;

        // CSS fallback fullscreen (when Fullscreen API fails, e.g. iOS canvas)
        this._cssFullscreen = false;
        this._exitFsBtn = null;

        // Visibility change: when returning from Alt-Tab, force browser to
        // re-composite all layers.  On Chrome Windows, the GPU compositor may
        // cache a corrupt layer (green tint from NV12→RGB bug in Canvas2D).
        // Invalidation via will-change toggle forces a fresh composite.
        this._onVisibilityChange = () => {
            if (document.visibilityState === 'visible' && !this._quitting) {
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
                new Blob(['{}'], { type: 'application/json' }));
        };

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

        // HEVC fallback: set to true when the browser does not support HEVC decoding
        // (e.g. Windows Chrome). The onQuit callback should detect this and re-launch
        // with H.264 forced via MoonlightApp.launchApp(host, app, 'h264').
        this._codecFallbackRequested = false;

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
        this.bindEvents();
        this.startRenderLoop();
        this.startDiagnostics();
        this.initAudioAsync();
    }

    // --- Diagnostics ---------------------------------------------------------

    startDiagnostics() {
        this._diagCount = 0;
        this._diagHandle = setInterval(() => {
            this._diagCount++;
            console.log('[DIAG] #' + this._diagCount +
                ' decoderConfigured=' + this.decoderConfigured +
                ' decoderConfiguring=' + this.decoderConfiguring +
                ' nalParser.isReady=' + this.nalParser.isReady() +
                ' pendingFrames=' + this.pendingFrames.length +
                ' frameQueue=' + this.frameQueue.length +
                ' frameCount=' + this.frameCount +
                ' received=' + this.stats.received +
                ' decoded=' + this.stats.decoded +
                ' rendered=' + this.stats.rendered +
                ' dropped=' + this.stats.dropped +
                ' canvas=' + this.canvas.width + 'x' + this.canvas.height +
                ' decoder=' + (this.decoder ? this.decoder.state : 'null') +
                ' ctx=' + (this.ctx ? 'ok' : 'null') +
                ' recoveryAttempts=' + this._recoveryAttempts +
                ' idrRequested=' + this._idrRequested +
                ' totalBytes=' + this._totalBytes +
                ' audio.ready=' + (this.audioPipeline ? this.audioPipeline.ready : 'no') +
                ' audio.samples=' + (this.audioPipeline ? this.audioPipeline.getStats().writtenSamples : 0) +
                ' audio.underrun=' + (this.audioPipeline ? this.audioPipeline.getStats().underrunCount : 0) +
                (this._lastFrameColorSpace ? ' cs=' + JSON.stringify(this._lastFrameColorSpace) : ''));
        }, 10000);
    }

    stopDiagnostics() {
        if (this._diagHandle) {
            clearInterval(this._diagHandle);
            this._diagHandle = null;
        }
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
                console.log('[PreFlight] HTTPS /api/health OK (status=' +
                    resp.status + ') — server is reachable');
            } else {
                console.warn('[PreFlight] HTTPS /api/health returned ' +
                    resp.status + ' — unexpected status');
            }
        } catch (err) {
            if (err.name === 'AbortError') {
                console.warn('[PreFlight] HTTPS /api/health timed out (5s) — ' +
                    'possible firewall blocking port 443');
            } else {
                console.warn('[PreFlight] HTTPS /api/health failed: "' +
                    err.message + '" — possible DNS or TLS issue');
            }
        }
    }

    // --- Platform / feature detection ---------------------------------------

    /** Log browser platform and WebCodecs feature availability for diagnostics.
     *  Helps identify iOS WebKit limitations (createImageBitmap, AudioWorklet). */
    _logPlatformInfo() {
        const ua = navigator.userAgent || '';
        const isIOS = /iPad|iPhone|iPod/.test(ua) ||
            (/Mac/.test(ua) && 'ontouchend' in document);
        const isSafari = /Safari\//.test(ua) && !/Chrome\//.test(ua);
        const hasVideoDecoder = typeof VideoDecoder !== 'undefined';
        const hasIsConfigSupported = hasVideoDecoder &&
            typeof VideoDecoder.isConfigSupported === 'function';
        const hasCreateImageBitmap = typeof createImageBitmap !== 'undefined';
        const hasAudioWorklet = typeof AudioWorkletNode !== 'undefined';
        const hasCanvas2D = typeof CanvasRenderingContext2D !== 'undefined';
        const hasIsContextLost = hasCanvas2D &&
            typeof CanvasRenderingContext2D.prototype.isContextLost === 'function';

        if (!hasIsContextLost) {
            console.warn('[Platform] CanvasRenderingContext2D.isContextLost() is NOT supported ' +
                '— Safari/WebKit limitation. Context loss detection disabled.');
        }

        // Detect Chrome on Windows — the HEVC NV12 RGBA copyTo path
        // (green-tint workaround for Chrome Windows D3D11 compositor) is
        // only safe on Windows. Chrome macOS/iOS has a stride bug in
        // frame.copyTo({ format: 'RGBA' }) that causes 4x horizontal stretch.
        const _isChromeWin = /Chrome\//.test(ua) && /Windows/.test(ua) && !/Edg\//.test(ua) && !/OPR\//.test(ua);
        this._isChromeWindowsHevc = _isChromeWin;

        console.log('[Platform] iOS=' + isIOS +
            ' Safari=' + isSafari +
            ' VideoDecoder=' + hasVideoDecoder +
            ' isConfigSupported=' + hasIsConfigSupported +
            ' createImageBitmap=' + hasCreateImageBitmap +
            ' AudioWorklet=' + hasAudioWorklet +
            ' Canvas2D=' + hasCanvas2D +
            ' isContextLost=' + hasIsContextLost +
            ' ChromeWinHEVC=' + _isChromeWin +
            ' UA: ' + ua.substring(0, 120));

        if (isIOS || isSafari) {
            console.log('[Platform] Running on Apple platform — ' +
                'createImageBitmap(VideoFrame) requires iOS 17+ / Safari 17+. ' +
                'If the screen is black, the rendering fallback in ' +
                '_drawFrameWithBitmap() should handle this automatically.');
        }

        // HEVC codec capability check for diagnostics
        if (typeof VideoDecoder !== 'undefined' && typeof VideoDecoder.isConfigSupported === 'function') {
            const hevcCodecs = ['hev1.1.6.L153.B0', 'hvc1.1.6.L153.B0', 'hev1.1.2.L153.B0'];
            Promise.all(hevcCodecs.map(c => VideoDecoder.isConfigSupported({
                codec: c,
                codedWidth: 1920,
                codedHeight: 1080
            }))).then(results => {
                hevcCodecs.forEach((c, i) => {
                    console.log('[Platform-HEVC] ' + c + ' isConfigSupported: ' + results[i].supported);
                });
            }).catch(() => {});
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
            if (this.audioPipeline.context &&
                this.audioPipeline.context.state === 'suspended') {
                await this.audioPipeline.resume();
            }
        } else {
            console.warn('[StreamView] AudioPipeline init failed — audio will be silent');
        }
    }

    /**
     * Handle an incoming PCM16 audio sample from the WebRTC DataChannel.
     * @param {Uint8Array} sample - PCM16 stereo interleaved data.
     */
    handleAudioSample(sample) {
        if (this._quitting) return;

        if (!this._audioLogged) {
            console.log('[StreamView] Audio sample received, size=' + sample.length +
                ', writing to pipeline...');
            this._audioLogged = true;
        }

        // Write to AudioPipeline (init may not be complete yet — safe to drop)
        if (this.audioPipeline && this.audioPipeline.ready) {
            this.audioPipeline.write(sample);
        }
    }

    // =========================================================================
    // DOM rendering
    // =========================================================================

    render() {
        const el = document.createElement('div');
        el.id = 'stream-view';
        el.className = 'stream-overlay';
        el.innerHTML = `
            <div class="stream-header">
                <button class="btn stream-quit-btn" id="btn-stream-quit">Stop Streaming</button>
            </div>
            <div class="stream-canvas-area">
                <canvas id="stream-canvas" class="stream-canvas"></canvas>
                <video id="stream-video" class="stream-video" autoplay muted playsinline
                       style="width:100%;height:100%;object-fit:contain;display:none;"></video>
                <div class="stream-touch-cursor" id="stream-touch-cursor"></div>
                <div class="stream-click-hint" id="stream-hint">
                    Click to capture mouse & keyboard
                </div>
            </div>
        `;
        document.getElementById('app').appendChild(el);

        this.canvas = document.getElementById('stream-canvas');
        // Default GPU-accelerated 2D context — matches Mac Chrome behavior.
        // Mac Chrome handles NV12→RGBA correctly via Metal; Windows Chrome
        // should do the same via D3D11.
        // NO desynchronized:true — it bypasses vsync composition on Android
        // (where it is actually honored) and causes visible tearing bands and
        // partially-written buffers. Desktop Chrome ignores it for 2D anyway.
        this.ctx = this.canvas.getContext('2d');
        // Set initial canvas size; will be adjusted when first frame arrives
        this.canvas.width = 1920;
        this.canvas.height = 1080;
        // Prevent browser default touch behaviors (scroll, zoom, pull-to-refresh)
        this.canvas.style.touchAction = 'none';
        // Video element for native RTP media track mode (webrtc-media)
        this.videoEl = document.getElementById('stream-video');
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

        // statusEl kept for backward compatibility — setStatus() is now a no-op
        this.statusEl = null;
        this.hintEl = document.getElementById('stream-hint');
        this._touchCursorEl = document.getElementById('stream-touch-cursor');

        document.getElementById('btn-stream-quit').onclick = () => this.quit();

        // ── Streaming stats overlay (top-center card, elegant styling) ─────
        this._overlayEl = document.createElement('div');
        this._overlayEl.id = 'stream-stats-overlay';
        this._overlayEl.className = 'stream-stats-overlay';
        this._overlayEl.innerHTML = '<div class="stats-waiting">Connecting...</div>';
        document.getElementById('stream-view').appendChild(this._overlayEl);

        // Start overlay update timer (every 500ms)
        this._overlayInterval = setInterval(() => this._updateOverlay(), 500);

        // ── Keyboard shortcuts slide ────────────────────────────────────────
        this._shortcutsSlide = document.createElement('div');
        this._shortcutsSlide.id = 'stream-shortcuts-slide';
        this._shortcutsSlide.className = 'stream-shortcuts-slide';
        this._shortcutsSlide.style.display = 'none';
        this._buildShortcutsSlideContent();
        document.getElementById('stream-view').appendChild(this._shortcutsSlide);

        // ── Startup overlay (centered 3-step status) ───────────────────────
        this._startupOverlay = document.createElement('div');
        this._startupOverlay.id = 'stream-startup-overlay';
        this._startupOverlay.className = 'stream-startup-overlay';
        this._startupOverlay.innerHTML = [
            '<div class="startup-step active" data-step="1">',
            '  <span class="startup-step-dot"></span>',
            '  <span class="startup-step-label">Connecting...</span>',
            '</div>',
            '<div class="startup-step" data-step="2">',
            '  <span class="startup-step-dot"></span>',
            '  <span class="startup-step-label">Starting video stream...</span>',
            '</div>',
            '<div class="startup-step" data-step="3">',
            '  <span class="startup-step-dot"></span>',
            '  <span class="startup-step-label">Stream ready!</span>',
            '</div>'
        ].join('');
        document.getElementById('stream-view').appendChild(this._startupOverlay);

        // DEBUG: orientation debug badge — remove after diagnosis
        if (IS_MOBILE_OR_TABLET) {
            this._debugOrientationEl = document.createElement('div');
            this._debugOrientationEl.id = 'stream-debug-orientation';
            this._debugOrientationEl.style.cssText = [
                'position:fixed',
                'bottom:8px',
                'right:8px',
                'background:rgba(0,0,0,0.65)',
                'color:#fff',
                'font:11px/1.4 monospace',
                'padding:4px 8px',
                'border-radius:4px',
                'z-index:9999',
                'pointer-events:none',
                'user-select:none',
                'white-space:pre-wrap',
            ].join(';');
            document.body.appendChild(this._debugOrientationEl);
            this._updateDebugOrientation();
            this._debugOrientationInterval = setInterval(() => this._updateDebugOrientation(), 200);
        }
        // DEBUG end

        // ── Header fullscreen button (always visible, desktop+mobile) ──────
        this._mobileFsBtn = document.createElement('button');
        this._mobileFsBtn.id = 'btn-stream-fs';
        this._mobileFsBtn.className = 'btn-stream-fs';
        this._mobileFsBtn.textContent = '⛶ Fullscreen';
        this._mobileFsBtn.title = 'Enter fullscreen';
        this._mobileFsBtn.addEventListener('click', (e) => {
            e.stopPropagation();
            this._requestFullscreen();
        });
        const header = document.querySelector('.stream-header');
        if (header) {
            // Insert before the quit button (first child)
            header.insertBefore(this._mobileFsBtn, header.firstChild);
        }
        // ── End header fullscreen button ─────────────────────────────────----
    }

    // =========================================================================
    // WebCodecs VideoDecoder
    // =========================================================================

    setupDecoder() {
        if (this.decoder) {
            console.log('[StreamView] Closing existing decoder');
            try { this.decoder.close(); } catch (e) {}
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
                    console.log('[StreamView] First decoded frame: ' +
                        (frame.displayWidth || frame.codedWidth) + 'x' +
                        (frame.displayHeight || frame.codedHeight) +
                        ' format=' + (frame.format || 'null'));
                }
                this.onDecodedFrame(frame);
            },
            error: (err) => {
                console.error('[StreamView] VideoDecoder error:', err.message, err);
                if (err.code) console.error('[StreamView] Error code:', err.code);
                this._handleDecoderError(err);
            }
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
        // Guard: prevent recovery during quit
        if (this._quitting) return;
        // Guard: prevent re-entrant recovery (error callback may fire during
        // setupDecoder(), which would loop back to this method)
        if (this._decoderRecovering) return;
        // Guard: limit total recovery attempts to avoid infinite loops on
        // a fundamentally broken connection
        this._recoveryAttempts++;
        if (this._recoveryAttempts > this.MAX_RECOVERY_ATTEMPTS) {
            console.error('[StreamView] Max recovery attempts (' + this.MAX_RECOVERY_ATTEMPTS +
                ') reached, giving up');
            this.setStatus('error', 'Max recovery attempts exceeded');
            return;
        }
        this._decoderRecovering = true;

        console.warn('[StreamView] Starting decoder recovery (' + this._recoveryAttempts +
            '/' + this.MAX_RECOVERY_ATTEMPTS + ') from: ' +
            (err ? err.message : 'unknown'));

        // 1. Close the broken decoder
        if (this.decoder) {
            try { this.decoder.close(); } catch (e) { /* ignore */ }
            this.decoder = null;
        }
        this.decoderConfigured = false;
        this.decoderConfiguring = false;
        // Decoder error invalidates the reference: drop deltas until next keyframe.
        this._referenceValid = false;

        // 2. Clear frame queue — frames output by the old decoder are invalid
        //    once the decoder is closed
        for (const frame of this.frameQueue) {
            try { frame.close(); } catch (e) { /* ignore */ }
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
    }

    configureDecoder() {
        if (this._quitting) return;
        if (this.decoderConfigured || this.decoderConfiguring || !this.nalParser.isReady()) {
            console.log('[StreamView] configureDecoder guard blocked: configured=' +
                this.decoderConfigured + ' configuring=' + this.decoderConfiguring +
                ' nalReady=' + this.nalParser.isReady());
            return;
        }
        this.decoderConfiguring = true;
        const codecType = this.nalParser.codec;

        // Log HEVC parser state before configuring
        console.log('[HEVC-CONFIG] nalParser state:' +
            ' codec=' + codecType +
            ' vps.len=' + (this.nalParser.vps?.length || 0) +
            ' sps.len=' + (this.nalParser.sps?.length || 0) +
            ' pps.len=' + (this.nalParser.pps?.length || 0) +
            ' videoCodec=' + this.videoCodec +
            ' _noDescription=' + this._noDescription);

        // Log raw parameter set bytes for first-time diagnostics
        if (this.stats.decoded === 0) {
            if (this.nalParser.vps) {
                console.log('[HEVC-CONFIG] VPS raw bytes:',
                    Array.from(this.nalParser.vps.slice(0, 32)).map(b => b.toString(16).padStart(2, '0')).join(' '));
            }
            if (this.nalParser.sps) {
                console.log('[HEVC-CONFIG] SPS raw bytes:',
                    Array.from(this.nalParser.sps.slice(0, 32)).map(b => b.toString(16).padStart(2, '0')).join(' '));
            }
        }

        console.log('[StreamView] configureDecoder STARTED, codec=' + codecType +
            ', decoder state=' + (this.decoder ? this.decoder.state : 'null'));

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

        console.log('[StreamView] Configuring VideoDecoder: codec=' + codec,
                    'descLen=' + desc.length,
                    'codecType=' + this.nalParser.codec);

        if (!VideoDecoder.isConfigSupported) {
            console.error('[StreamView] WebCodecs VideoDecoder not available');
            this.decoderConfiguring = false;
            this.setStatus('error', 'WebCodecs not supported');
            return;
        }

        const applyConfig = (cfg, noDescription = false) => {
            // Try hardware-accelerated decoder first (GPU decoding on Android)
            const _doConfigure = (config, hwAccel) => {
                const cfgToUse = hwAccel
                    ? { ...config, hardwareAcceleration: 'prefer-hardware' }
                    : config;
                try {
                    console.log('[StreamView] Calling decoder.configure() with codec=' + cfgToUse.codec +
                        ' descriptionLen=' + (cfgToUse.description ? cfgToUse.description.byteLength : 0) +
                        ' noDescription=' + noDescription +
                        ' hwAccel=' + (cfgToUse.hardwareAcceleration || 'default') +
                        ' isChromeWin=' + this._isChromeWindowsHevc +
                        ' nalParser.codec=' + this.nalParser.codec);
                    this.decoder.configure(cfgToUse);
                    this.decoderConfigured = true;
                    this.decoderConfiguring = false;
                    this._noDescription = noDescription;
                    console.log('[HEVC-CONFIG-OK] *** VideoDecoder CONFIGURED *** codec=' + cfgToUse.codec +
                        ' state=' + this.decoder.state +
                        ' noDescription=' + this._noDescription +
                        ' hwAccel=' + (cfgToUse.hardwareAcceleration || 'none') +
                        ' dequeuing ' + this.pendingFrames.length + ' pending frames');
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
                                console.log('[StreamView] Proactive IDR after initial decoder config');
                                this.webrtc.send({ type: 'requestidr' });
                            }
                        }, 250);
                    }

                    return true;
                } catch (e) {
                    console.error('[StreamView] decoder.configure() failed:', e.message, e);
                    if (this.decoder) {
                        console.log('[StreamView] Decoder state after failed configure: ' + this.decoder.state);
                    }
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
                console.warn('[GPU] prefer-hardware rejected (' + hwErr.message + '), falling back to software');
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
            console.log('[HEVC-CONFIG] Testing config[' + index + ']: codec=' + cfg.codec +
                ' hasDescription=' + (cfg.description ? 'yes' : 'no') +
                ' hasColorSpace=' + (cfg.colorSpace ? 'yes' : 'no') +
                ' _noDescription=' + noDescription +
                ' coded=' + (cfg.codedWidth || '?') + 'x' + (cfg.codedHeight || '?'));
            VideoDecoder.isConfigSupported(cfg).then((result) => {
                console.log('[HEVC-CONFIG] isConfigSupported for ' + cfg.codec +
                    ': supported=' + result.supported +
                    ' (desc=' + (cfg.description ? 'yes' : 'no') + ')');
                if (result.supported) {
                    console.log('[StreamView] Config supported: codec=' + cfg.codec +
                                ' hasDescription=' + !!cfg.description);
                    if (!applyConfig(cfg, noDescription)) {
                        console.log('[StreamView] applyConfig failed, trying next config');
                        tryCodecs(configs, index + 1, onExhausted);
                    }
                } else {
                    console.warn('[StreamView] Config NOT supported: codec=' + cfg.codec +
                                 ', trying next');
                    tryCodecs(configs, index + 1, onExhausted);
                }
            }).catch((err) => {
                console.error('[StreamView] isConfigSupported error for codec=' +
                              cfg.codec + ':', err.message, err);
                tryCodecs(configs, index + 1, onExhausted);
            });
        };

        // Shared config fields
        const shared = {
            codedWidth: 1920,
            codedHeight: 1080,
            optimizeForLatency: true
        };

        const bt709 = {
            colorSpace: {
                primaries: 'bt709',
                transfer: 'bt709',
                matrix: 'bt709',
                fullRange: false
            }
        };

        // Build configs: for HEVC, try Annex B (no description) first.
        // Chromium's keyframe validator (AnalyzeAnnexB) only parses start-code
        // format — without Annex B, HEVC keyframes are falsely rejected on
        // Chrome/Edge.  Non-Chromium browsers fall back to AVCC + description.
        const configsToTry = [];
        const fallbacks = (codecType === CODEC_HEVC)
            ? HEVC_FALLBACK_CODEC_STRINGS
            : H264_FALLBACK_CODEC_STRINGS;
        // Chrome WebCodecs has historically rejected HEVC configs that include
        // an explicit colorSpace. We attempt it as the primary config for HEVC
        // too; if Chrome rejects it, the fallback chain skips to the next
        // config (without colorSpace) automatically.
        const colorConfig = {
            codec: codec,
            description: desc.buffer,
            ...shared,
            ...bt709
        };

        // ── HEVC Annex B phase (no description) ──
        // Chromium keyframe validator only handles start codes.  Try Annex B
        // configs first; if all fail, fall back to AVCC with description.
        if (codecType === CODEC_HEVC) {
            const annexBCfgs = [];
            const hev1Primary = codec.replace(/^hvc1/, 'hev1');
            annexBCfgs.push({ codec: hev1Primary, ...shared, ...bt709, _noDescription: true });
            annexBCfgs.push({ codec: hev1Primary, ...shared, _noDescription: true });
            for (const fb of HEVC_ANNEXB_CODEC_STRINGS) {
                if (fb === hev1Primary) continue;
                annexBCfgs.push({ codec: fb, ...shared, ...bt709, _noDescription: true });
                annexBCfgs.push({ codec: fb, ...shared, _noDescription: true });
            }

            console.log('[StreamView] Phase A: ' + annexBCfgs.length +
                ' Annex B configs (no description, hev1)');

            tryCodecs(annexBCfgs, 0, () => {
                console.log('[StreamView] Annex B exhausted, Phase B: AVCC with description');
                this._tryHevcAvccConfigs(codec, desc, fallbacks, shared, bt709);
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
                ...bt709
            });
            // Without colorSpace (Safari iOS does not support colorSpace in configure())
            configsToTry.push({
                codec: fbCodec,
                description: desc.buffer,
                codedWidth: 1920,
                codedHeight: 1080,
                optimizeForLatency: true
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
                codedHeight: 1080
            });
            // Variant B: no optimizeForLatency, no codedWidth/codedHeight
            configsToTry.push({
                codec: codec,
                description: desc.buffer
            });
            // Variant C: use Uint8Array directly (some Safari versions reject
            // ArrayBuffer-based description)
            configsToTry.push({
                codec: codec,
                description: desc,
                codedWidth: 1920,
                codedHeight: 1080,
                optimizeForLatency: true
            });
            // Last resort: bare codec string, no description at all.
            // Safari iOS may auto-detect the H.264 config from the bitstream.
            configsToTry.push({
                codec: codec,
                optimizeForLatency: true
            });
            configsToTry.push({
                codec: codec
            });
        }

        console.log('[StreamView] Trying ' + configsToTry.length + ' codec configs (' +
            (codecType === CODEC_HEVC ? 'HEVC' : 'H.264') + ')');
        tryCodecs(configsToTry, 0, () => {
            // All avc1 configs exhausted — try avc3 (in-band SPS/PPS).
            // Some browsers/devices prefer avc3 over avc1 for hardware decoding.
            console.warn('[GPU] All avc1 configs rejected, trying avc3 (in-band SPS/PPS)');
            const avc3Configs = [
                { codec: 'avc3.42E01E', ...shared, ...bt709, optimizeForLatency: true },
                { codec: 'avc3.42E01E', ...shared, optimizeForLatency: true },
                { codec: 'avc3.42E01E', ...shared }
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
    _tryHevcAvccConfigs(codec, desc, fallbacks, shared, bt709) {
        const cfgs = [];

        cfgs.push({ codec, description: desc.buffer, ...shared, ...bt709 });
        cfgs.push({ codec, description: desc.buffer, ...shared });

        for (const fb of fallbacks) {
            if (fb === codec) continue;
            cfgs.push({ codec: fb, description: desc.buffer, ...shared, ...bt709 });
            cfgs.push({ codec: fb, description: desc.buffer, ...shared });
        }

        cfgs.push({ codec, description: desc, ...shared });
        cfgs.push({ codec, description: desc.buffer, optimizeForLatency: true });
        cfgs.push({ codec, description: desc.buffer, codedWidth: 1920, codedHeight: 1080 });
        cfgs.push({ codec, description: desc.buffer });
        cfgs.push({ codec, ...shared });
        cfgs.push({ codec, optimizeForLatency: true });

        console.log('[StreamView] Phase B: ' + cfgs.length + ' AVCC configs (with desc)');

        const tryNext = (idx) => {
            if (idx >= cfgs.length) {
                console.warn('[StreamView] Phase B exhausted, H.264 fallback');
                this.decoderConfiguring = false;
                this._handleHevcFallback();
                return;
            }
            const cfg = cfgs[idx];
            console.log('[StreamView] Phase B[' + idx + ']: ' + cfg.codec +
                ' desc=' + (cfg.description ? 'yes' : 'no'));
            VideoDecoder.isConfigSupported(cfg).then((r) => {
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
            }).catch((err) => {
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
        this.decoderConfiguring = false;
        this._codecFallbackRequested = true;

        console.warn('[StreamView] HEVC fallback triggered — requesting H.264 re-launch');

        // Send a diagnostic message via the DataChannel for backend logging.
        // This is best-effort; the backend does not need to act on it.
        try {
            if (this.webrtc && typeof this.webrtc.send === 'function') {
                this.webrtc.send({
                    type: 'codec_fallback',
                    from: 'hevc',
                    to: 'h264'
                });
            }
        } catch (e) {
            // Ignore send errors — the stream is about to be torn down
        }

        // Quit the stream. MoonlightApp._onStreamingQuit() will detect
        // _codecFallbackRequested and re-launch with H.264.
        this.quit();
    }

    flushPendingFrames() {
        console.log('[StreamView] flushPendingFrames: draining ' + this.pendingFrames.length + ' frames');
        // After decoder configure, a keyframe MUST be the first frame fed.
        // Delta frames can arrive before the keyframe (SCTP unordered delivery
        // over high-latency links, e.g. remote UPnP). Feeding a delta first
        // produces green/garbage output — the green-image bug.
        if (this.pendingFrames.length > 1 && !this.pendingFrames[0].isKeyframe) {
            const keyIdx = this.pendingFrames.findIndex(e => e.isKeyframe);
            if (keyIdx > 0) {
                console.log('[StreamView] flushPendingFrames: moving keyframe from index ' +
                    keyIdx + ' to front (delta arrived first due to SCTP reordering)');
                const [keyframe] = this.pendingFrames.splice(keyIdx, 1);
                this.pendingFrames.unshift(keyframe);
            }
        }
        while (this.pendingFrames.length > 0) {
            const entry = this.pendingFrames.shift();
            this.decodeFrame(entry.data, entry.isKeyframe, entry.backendTs);
        }
        console.log('[StreamView] flushPendingFrames done, decoder state=' + this.decoder.state);
    }

    decodeFrame(data, isKeyframe, backendTs) {
        if (!this.decoderConfigured) {
            // Buffer until decoder is ready (limit to avoid OOM)
            if (this.pendingFrames.length < 120) {
                this.pendingFrames.push({ data, isKeyframe, backendTs });
                if (this.pendingFrames.length === 1) {
                    console.log('[StreamView] First frame buffered (pending), waiting for decoder config, webrtc.video=' +
                    (this.webrtc && this.webrtc.dataChannels && this.webrtc.dataChannels.video ?
                        this.webrtc.dataChannels.video.readyState : '?'));
                }
                if (this.pendingFrames.length % 30 === 0) {
                    console.log('[StreamView] Buffered frames: ' + this.pendingFrames.length);
                }
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
                console.warn('[StreamView] Decode queue stalled >' + this.QUEUE_RESET_MS +
                    'ms (queueSize=' + this.decoder.decodeQueueSize + ') — resetting decoder');
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
        let timestamp = (backendTs !== undefined && backendTs > 0)
            ? backendTs * 1000
            : this.frameCount * 16667;
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
            console.error('[StreamView] EMPTY AVCC DATA for ' + type + ' frame #' + this.frameCount +
                ' — stripParams=' + this.decoderConfigured +
                ' codec=' + this.nalParser.codec +
                ' inputSize=' + data.length);
            // Check if all NALs were stripped: split and log types
            const nals = splitNals(data);
            const types = nals.map(n => {
                if (n.length >= 2 && this.nalParser.codec === CODEC_HEVC)
                    return (n[0] >> 1) & 0x3F;
                return n.length >= 1 ? (n[0] & 0x1F) : -1;
            });
            console.error('[StreamView] NAL types in empty-avcc frame:', types.join(', '));
        }

        try {
            const chunk = new EncodedVideoChunk({
                type: type,
                timestamp: timestamp,
                duration: 16667,
                data: avccData
            });
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

    onDecodedFrame(frame) {
        this.stats.decoded++;

        // A successful decode means we've recovered — reset the counter
        this._recoveryAttempts = 0;

        // FPS tracking: record decode timestamp (2s sliding window in _updateOverlay)
        this._fpsTimestamps.push(performance.now());

        // ── End-to-end latency ──────────────────────────────────────────────
        // Total time from Sunshine capture to browser decode+display.
        // Backend sends captureSteadyMs (= firstFrameArrivalSteadyMs + presTimeUs/1000)
        // in the steady_clock domain. The offset between steady_clock and
        // performance.now() is established on the first video frame (fallback)
        // and refined by periodic stats messages.
        //   capturePerfTime = captureSteadyMs - _steadyToPerfOffset
        //   e2eLatency = performance.now() - capturePerfTime
        if (this._latestBackendTs !== undefined && this._steadyToPerfOffset !== null) {
            const capturePerfTime = this._latestBackendTs - this._steadyToPerfOffset;
            const latency = performance.now() - capturePerfTime;
            if (latency > 0 && latency < 5000) { // ignore outliers
                this._e2eLatencyStats.addSample(latency);
            }
        }

        if (this.stats.decoded <= 3) {
            const cs = frame.colorSpace;
            console.log('[HEVC-FRAME-' + this.stats.decoded + ']' +
                ' display=' + (frame.displayWidth || '?') + 'x' + (frame.displayHeight || '?') +
                ' coded=' + (frame.codedWidth || '?') + 'x' + (frame.codedHeight || '?') +
                ' format=' + (frame.format || '?') +
                ' visibleRect=' + JSON.stringify(frame.visibleRect || 'none') +
                ' colorSpace=' + JSON.stringify({
                    primaries: cs?.primaries,
                    transfer: cs?.transfer,
                    matrix: cs?.matrix,
                    fullRange: cs?.fullRange
                }) +
                ' timestamp=' + frame.timestamp +
                ' duration=' + frame.duration);
        }
        if (this.stats.decoded === 1) {
            const cs = frame.colorSpace;
            this._lastFrameColorSpace = cs ? {
                p: cs.primaries, t: cs.transfer, m: cs.matrix, f: cs.fullRange
            } : null;
        }

        // Limit queue depth to prevent unbounded memory growth.
        // Drop the NEW frame instead of closing an old one — the old frame
        // may still be rendering in _drawFrameWithBitmap() (async + not awaited).
        // Closing it mid-render zeroes its NV12 buffer → green screen on Chrome.
        if (this.frameQueue.length >= 3) {
            frame.close();
            this.stats.dropped++;
            return;
        }

        this.frameQueue.push(frame);

        // Update status on first frame
        if (!this._firstFrameRendered) {
            console.log('[StreamView] FIRST DECODED FRAME! Setting status to Live');
            // Capture resolution from first frame
            const w = frame.displayWidth || frame.codedWidth || 0;
            const h = frame.displayHeight || frame.codedHeight || 0;
            if (w > 0) this._resolution = w + '×' + h;
            this.setStatus('live', 'Live');
            this._firstFrameRendered = true;
            // Show stats overlay (only if enabled in settings)
            if (this._overlayEl && this._showPerfStats) this._overlayEl.style.display = '';
            // Mark startup step 3 ("Stream ready!") and hide overlay after 1.5s
            this._updateStartupStep(3);
            setTimeout(() => this._hideStartupOverlay(), 1500);
            // Show keyboard shortcuts slide (5s auto-hide)
            this._showShortcutsSlide();

        }
    }

    startRenderLoop() {
        if (this.renderRunning) return;
        // Media track mode: video is rendered natively via <video>, no canvas loop needed.
        if (this._transport === 'webrtc-media') return;
        this.renderRunning = true;

        const loop = (now) => {
            if (!this.renderRunning) return;

            // Detect Canvas2D context loss (GPU driver crash, Alt-Tab on some GPUs).
            // When lost, the canvas is permanently blank — we must recreate the context.
            // Safari/WebKit does not support CanvasRenderingContext2D.isContextLost().
            // Feature-detect the method before calling it to avoid TypeError.
            const ctxLost = typeof this.ctx.isContextLost === 'function'
                ? this.ctx.isContextLost()
                : false;
            if (this.ctx && ctxLost) {
                console.warn('[StreamView] Canvas2D context lost, recreating...');
                this.ctx = this.canvas.getContext('2d');
                // Force re-composite all overlay layers by briefly toggling transform
                const header = document.querySelector('.stream-header');
                if (header) {
                    header.style.transform = 'translateZ(0)';
                    requestAnimationFrame(() => { header.style.transform = ''; });
                }
            }

            // ── Render ONE frame per rAF to serialize GPU access ─────────
            // HEVC NV12 on Chrome Windows fails when two VideoFrame objects
            // are accessed concurrently (createImageBitmap / copyTo race).
            // Processing one frame per rAF (~60fps) with a guard prevents
            // overlapping GPU operations. When frames arrive in bursts
            // (DC mode), we drop intermediate frames to keep latency low.
            if (this._rendering) {
                // Previous render still in flight — skip this rAF tick.
                // Drop intermediate frames to prevent queue buildup.
                if (this.frameQueue.length > 1) {
                    while (this.frameQueue.length > 1) {
                        const old = this.frameQueue.shift();
                        old.close();
                        this.stats.dropped++;
                    }
                }
                requestAnimationFrame(loop);
                return;
            }

            if (this.frameQueue.length > 1) {
                // Drop all but the latest frame — only render the freshest
                while (this.frameQueue.length > 1) {
                    const old = this.frameQueue.shift();
                    old.close();
                    this.stats.dropped++;
                }
            }

            if (this.frameQueue.length === 1) {
                this._rendering = true;
                const frame = this.frameQueue.shift();

                // Resize canvas to match frame dimensions if needed.
                const canvasWBefore = this.canvas.width;
                const canvasHBefore = this.canvas.height;
                if (frame.displayWidth && frame.displayHeight &&
                    (this.canvas.width !== frame.displayWidth ||
                     this.canvas.height !== frame.displayHeight)) {
                    this.canvas.width = frame.displayWidth;
                    this.canvas.height = frame.displayHeight;
                }

                if (this.stats.rendered < 5) {
                    console.log('[debug] renderLoop frame#' + this.stats.rendered +
                        ' display=' + (frame.displayWidth || '?') + 'x' + (frame.displayHeight || '?') +
                        ' coded=' + (frame.codedWidth || '?') + 'x' + (frame.codedHeight || '?') +
                        ' format=' + (frame.format || '?') +
                        ' canvasBefore=' + canvasWBefore + 'x' + canvasHBefore +
                        ' canvasAfter=' + this.canvas.width + 'x' + this.canvas.height +
                        ' codec=' + this.videoCodec);
                }

                // Fire-and-forget with guard: mark rendering complete when done
                this._drawFrameWithBitmap(frame).finally(() => {
                    this._rendering = false;
                });

                // Log stats periodically (every ~5s at 60fps)
                if (this.stats.rendered % 300 === 0) {
                    console.log('[StreamView] Stats:',
                        'received=' + this.stats.received,
                        'decoded=' + this.stats.decoded,
                        'rendered=' + this.stats.rendered,
                        'dropped=' + this.stats.dropped,
                        'queue=' + this.frameQueue.length);
                }
            }

            requestAnimationFrame(loop);
        };

        requestAnimationFrame(loop);
    }

    stopRenderLoop() {
        this.renderRunning = false;
    }

    /**
     * Draw a VideoFrame to canvas.
     *
     * Platform-specific workarounds for browser bugs:
     *
     *   Windows Chrome HEVC NV12:
     *     GPU D3D11 compositor bug → green tint with createImageBitmap.
     *     Fix: CPU-side copyTo(RGBA) + putImageData bypasses GPU pipeline.
     *
     *   macOS/iOS Chrome HEVC NV12:
     *     Chromium createImageBitmap stride bug → 4x horizontal stretch.
     *     Fix: drawImage(VideoFrame) first (GPU compositor via Metal works).
     *
     *   All other cases (H.264, AV1, Safari, Edge):
     *     Standard createImageBitmap path (fastest, GPU→GPU).
     *
     * Async: returns immediately; the frame is always closed, even on error.
     */
    async _drawFrameWithBitmap(frame) {
        // Chrome Windows only: the bitmap-primary path works around a D3D11
        // compositor ghosting bug. On Android/iOS/Mac it just adds a costly
        // per-frame GPU copy — those platforms use the standard path below.
        const isHevcNv12 = this.videoCodec === CODEC_HEVC && frame.format === 'NV12' &&
            this._isChromeWindowsHevc;
        const dbgFirstFew = this.stats.rendered < 5;

        if (dbgFirstFew) {
            console.log('[debug] _drawFrameWithBitmap entry' +
                ' codec=' + this.videoCodec +
                ' format=' + (frame.format || '?') +
                ' displaySize=' + (frame.displayWidth || '?') + 'x' + (frame.displayHeight || '?') +
                ' codedSize=' + (frame.codedWidth || '?') + 'x' + (frame.codedHeight || '?') +
                ' isHevcNv12=' + isHevcNv12 +
                ' nalParser.codec=' + this.nalParser.codec);
        }

        // ── HEVC NV12: createImageBitmap(VideoFrame) as PRIMARY path ──────
        //
        // ROOT CAUSE of persistent ghosting after canvas-level fixes:
        //
        // The previous primary path used frame.copyTo(buf, {format:'RGBA'})
        // to convert NV12→RGBA on CPU, then createImageBitmap(ImageData) to
        // upload back to GPU.  If Chrome silently ignores the 'RGBA' format
        // hint and writes NV12 (1.5 bytes/pixel) into a 4-byte/pixel buffer,
        // the ImageData has alpha=0 on ~3/4 of pixels.  Even with
        // globalCompositeOperation='copy', zero-alpha pixels may not fully
        // overwrite the destination — and getImageData(1x1) can't detect this.
        //
        // Fix: use createImageBitmap(VideoFrame) as PRIMARY.  This keeps the
        // NV12→RGBA conversion on the GPU (no CPU round-trip, no buffer size
        // mismatch), and the resulting RGBA bitmap has alpha=255 everywhere.
        // The 'copy' composite mode then correctly overwrites all pixels.
        //
        // Fallback chain:
        //   1. createImageBitmap(VideoFrame) → drawImage(bitmap, 'copy')
        //   2. ctx.drawImage(VideoFrame, 'copy')          (some Safari)
        //   3. copyTo(RGBA) → ImageData → putImageData     (last resort)

        // ── HEVC NV12: render with 'copy' composite mode ─────────────────
        // globalCompositeOperation='copy' replaces ALL destination pixels,
        // regardless of alpha — no blending at all.
        // The standard path below handles non-HEVC without 'copy' mode.
        if (isHevcNv12) {
            // ── PRIMARY: createImageBitmap(VideoFrame) BEFORE touching canvas.
            // The await must happen before any canvas mutation: painting black
            // then awaiting let the compositor present an all-black canvas
            // (visible black flashes on Android where the bitmap is slow).
            // No black pre-fill is needed — 'copy' mode replaces every pixel.
            let bitmap = null;
            try {
                bitmap = await createImageBitmap(frame);
            } catch (e) {
                if (dbgFirstFew) {
                    console.warn('[HEVC-RENDER] createImageBitmap(VideoFrame) failed: ' +
                        e.message + ' — trying ctx.drawImage(VideoFrame)');
                }
            }

            this.ctx.save();
            this.ctx.globalCompositeOperation = 'copy';

            let success = false;
            if (bitmap) {
                this.ctx.drawImage(bitmap, 0, 0,
                    this.canvas.width, this.canvas.height);
                bitmap.close();
                success = true;
                if (dbgFirstFew) {
                    console.log('[HEVC-RENDER] createImageBitmap(VideoFrame) SUCCESS, frame#' +
                        this.stats.rendered);
                }
            }

            // ── FALLBACK 1: ctx.drawImage(VideoFrame) direct ──────────────
            if (!success) {
                try {
                    this.ctx.drawImage(frame, 0, 0,
                        this.canvas.width, this.canvas.height);
                    success = true;
                    if (dbgFirstFew) {
                        console.log('[HEVC-RENDER] ctx.drawImage(VideoFrame) SUCCESS, frame#' +
                            this.stats.rendered);
                    }
                } catch (e) {
                    console.warn('[HEVC-RENDER] ctx.drawImage(VideoFrame) failed: ' + e.message);
                }
            }

            // ── FALLBACK 2: copyTo(RGBA) → ImageData → putImageData ──────
            // CPU-side NV12→RGBA conversion.  Last resort when GPU paths fail.
            if (!success) {
                try {
                    const w = frame.displayWidth || frame.codedWidth || 0;
                    const h = frame.displayHeight || frame.codedHeight || 0;
                    if (w > 0 && h > 0) {
                        const size = w * h * 4;
                        const buf = new ArrayBuffer(size);
                        await frame.copyTo(buf, { format: 'RGBA' });
                        const imageData = new ImageData(
                            new Uint8ClampedArray(buf, 0, size), w, h);

                        // Diagnostic: sample RGBA pixels before rendering
                        if (dbgFirstFew) {
                            const midX = Math.floor(w / 2);
                            const midY = Math.floor(h / 2);
                            const off = (midY * w + midX) * 4;
                            const rgba = imageData.data;
                            console.log('[RGBA-DEBUG] copyTo sample center: R=' +
                                rgba[off] + ' G=' + rgba[off+1] + ' B=' + rgba[off+2] +
                                ' A=' + rgba[off+3]);
                            let alphaZeroCount = 0;
                            for (let i = 3; i < Math.min(size, 4096); i += 4) {
                                if (rgba[i] === 0) alphaZeroCount++;
                            }
                            console.log('[RGBA-DEBUG] copyTo alpha=0 count (1K pixels): ' +
                                alphaZeroCount + '/' + Math.min(Math.floor(size/4), 1024));
                        }

                        this.ctx.putImageData(imageData, 0, 0);
                        success = true;
                        if (dbgFirstFew) {
                            console.log('[HEVC-RENDER] copyTo(RGBA)+putImageData SUCCESS, frame#' +
                                this.stats.rendered);
                        }
                    }
                } catch (e) {
                    console.error('[HEVC-RENDER] All render paths failed:', e.message);
                }
            }

            this.ctx.restore();

            // Force GPU sync after render — only needed on the first frames to
            // flush stale compositor caches; per-frame readback costs ~1-3ms.
            if (this.stats.rendered < 30) {
                this.ctx.getImageData(0, 0, 1, 1);
            }

            // Diagnostic
            if (dbgFirstFew) {
                const w = frame.displayWidth || frame.codedWidth || 0;
                const h = frame.displayHeight || frame.codedHeight || 0;
                if (w > 0 && h > 0) {
                    const pixels = this.ctx.getImageData(
                        Math.floor(w / 2), Math.floor(h / 2), 1, 1).data;
                    console.log('[RGBA-DEBUG] Canvas center after render: R=' +
                        pixels[0] + ' G=' + pixels[1] + ' B=' + pixels[2] + ' A=' + pixels[3] +
                        ' (frame#' + this.stats.rendered + ')');
                }
            }

            frame.close();
            this.stats.rendered++;
            return;
        }

        // ── Standard path (H.264 / AV1 / non-NV12 / Safari) ──────────────
        let rendered = false;

        // Try direct drawImage(VideoFrame) first for ALL codecs: it skips the
        // per-frame await createImageBitmap (GPU copy + microtask gap) — the
        // main fluidity cost on Android/iOS. createImageBitmap remains the
        // fallback for browsers where direct VideoFrame drawing fails.
        {
            try {
                if (this.canvas && this.ctx) {
                    if (this.stats.rendered < 6) {
                        console.log('[GPU] direct drawImage(VideoFrame), frame#' +
                            this.stats.rendered);
                    }
                    this.ctx.drawImage(frame, 0, 0,
                        this.canvas.width, this.canvas.height);
                    rendered = true;
                }
            } catch (e) {
                console.warn('[GPU] drawImage(VideoFrame) failed: ' + e.message +
                    ' — falling back to createImageBitmap');
            }
        }

        if (!rendered) {
            try {
                const bitmap = await createImageBitmap(frame);
                if (this.stats.rendered < 5) {
                    console.log('[debug] PATH: createImageBitmap' +
                        ' bitmapSize=' + bitmap.width + 'x' + bitmap.height +
                        ' canvasSize=' + (this.canvas?.width || '?') + 'x' + (this.canvas?.height || '?'));
                }
                if (this.canvas && this.ctx) {
                    this.ctx.drawImage(bitmap, 0, 0,
                        this.canvas.width, this.canvas.height);
                }
                bitmap.close();
                rendered = true;
            } catch (e) {
                console.warn('[StreamView] createImageBitmap failed: ' + e.message +
                    ' — trying drawImage(VideoFrame) fallback');
                if (this.canvas && this.ctx) {
                    try {
                        this.ctx.drawImage(frame, 0, 0,
                            this.canvas.width, this.canvas.height);
                        rendered = true;
                    } catch (e2) {
                        console.warn('[StreamView] drawImage(VideoFrame) failed: ' +
                            e2.message + ' — trying copyTo RGBA readback');
                        try {
                            const w = frame.displayWidth || frame.codedWidth || 0;
                            const h = frame.displayHeight || frame.codedHeight || 0;
                            if (w > 0 && h > 0) {
                                const size = w * h * 4;
                                const buf = new ArrayBuffer(size);
                                await frame.copyTo(buf, { format: 'RGBA' });
                                const imageData = new ImageData(
                                    new Uint8ClampedArray(buf, 0, size), w, h);
                                this.ctx.putImageData(imageData, 0, 0);
                                rendered = true;
                            }
                        } catch (e3) {
                            console.error('[StreamView] All rendering paths failed:', e3.message);
                        }
                    }
                }
            }
        }
        frame.close();
        this.stats.rendered++;
    }

    // =========================================================================
    // WebRTC DataChannel (replaces legacy WebSocket binary transport)
    // =========================================================================

    setupWebRtc() {
        this.webrtc.onOpen = () => {
            if (this._quitting) return;
            this.connected = true;
            this.setStatus('connecting', 'Waiting for stream...');
            this._updateStartupStep(2);

            // Start ping timer for browser-side RTT measurement.
            // Sends a ping every 2s on the input DC; backend echoes back a pong.
            if (this._pingInterval) clearInterval(this._pingInterval);
            this._pingInterval = setInterval(() => {
                if (this._quitting) return;
                const seq = this._pingSeq++;
                this.webrtc.send({ type: 'ping', seq, ts: performance.now() });
            }, 2000);

            if (this._transport === 'webrtc-media') {
                // Media track mode: video arrives natively via <video> element.
                // No WebCodecs decoder needed. The first video frame triggers
                // the 'live' status via the video element's playing event.
                if (this.videoEl) {
                    this.videoEl.onplaying = () => {
                        if (!this._firstFrameRendered) {
                            this._firstFrameRendered = true;
                            const w = this.videoEl.videoWidth || 0;
                            const h = this.videoEl.videoHeight || 0;
                            if (w > 0) this._resolution = w + '×' + h;
                            this.setStatus('live', 'Live');
                            if (this._overlayEl && this._showPerfStats) this._overlayEl.style.display = '';
                            // Mark startup step 3 ("Stream prêt !") and hide overlay after 1.5s
                            this._updateStartupStep(3);
                            setTimeout(() => this._hideStartupOverlay(), 1500);
                            // Show keyboard shortcuts slide (5s auto-hide)
                            this._showShortcutsSlide();

                        }
                    };
                }
            } else {
                // DataChannel mode: set up WebCodecs decoder
                this.setupDecoder();

                // Safety timeout: if no decoder config after 3 seconds, request an IDR.
                this._idrTimeout = setTimeout(() => {
                    if (!this.nalParser.isReady()) {
                        console.warn('[StreamView] No keyframe after 3s, requesting IDR from backend');
                        this._requestIdr('no keyframe after 3s');
                    }
                }, 3000);
            }

            // Mobile fullscreen/button init (all transport modes)
            this._initMobileFullscreen();
        };
        this.webrtc.onClose = () => {
            if (this._quitting) return;
            this.connected = false;
            this.setStatus('disconnected', 'Disconnected');
            Toast.error('Stream disconnected');
            setTimeout(() => this.quit(), 3000);
        };
        this.webrtc.onError = (err) => {
            if (this._quitting) return;
            console.error('[StreamView] WebRTC error:', err.message);
            Toast.error('WebRTC connection error');
        };
        // Video frames: DataChannel mode uses WebCodecs callbacks
        if (this._transport !== 'webrtc-media') {
            this.webrtc.onVideo = (frame, isKeyframe, backendTs, frameId) => this.handleVideoFrame(frame, isKeyframe, backendTs, frameId);
            // Frame loss from reassembly: invalidate reference until next keyframe.
            // IDR is already requested by WebRtcDataChannel (throttled) — don't double it.
            this.webrtc.onFrameLoss = (frameId, wasKeyframe) => {
                this._referenceValid = false;
            };
        }
        // Audio samples (PCM16 stereo interleaved) -> AudioPipeline
        this.webrtc.onAudio = (sample) => this.handleAudioSample(sample);
        this.webrtc.connect();
    }

    // ── Stats overlay (refreshed every 500ms) ────────────────────────────

    _updateOverlay() {
        if (!this._overlayEl) return;

        // Hide entire overlay when performance stats are disabled in settings
        if (!this._showPerfStats) {
            this._overlayEl.style.display = 'none';
            return;
        }

        // Before first frame: show minimal waiting state
        if (!this._firstFrameRendered) {
            this._overlayEl.innerHTML = '<div class="stats-waiting">Connecting...</div>';
            this._overlayEl.style.display = '';
            return;
        }

        const now = performance.now();
        const elapsed = (now - this._startTime) / 1000;
        let fps = 0;
        let bitrateMbps = 0;

        // FPS: decoded frames in the last 2 seconds
        const cutoff = now - 2000;
        this._fpsTimestamps = this._fpsTimestamps.filter(t => t > cutoff);
        fps = Math.round(this._fpsTimestamps.length / 2);

        // Bitrate: total bytes / elapsed seconds
        if (elapsed > 0.5) {
            bitrateMbps = ((this._totalBytes * 8) / elapsed) / 1e6;
        }

        const codec = this.videoCodec === 'auto' ? 'h264' : this.videoCodec;

        // Build styled overlay HTML — each stat on its own line with a descriptive label
        let html = '<div class="stats-body">';

        // Resolution
        html += '<div class="stats-row">' +
            '<span class="stats-label">Resolution:</span>' +
            '<span class="stats-value">' + (this._resolution || '?') + '</span>' +
            '</div>';

        // Framerate
        html += '<div class="stats-row">' +
            '<span class="stats-label">Framerate:</span>' +
            '<span class="stats-value">' + fps + ' fps</span>' +
            '</div>';

        // Bitrate
        html += '<div class="stats-row">' +
            '<span class="stats-label">Bitrate:</span>' +
            '<span class="stats-value">' + bitrateMbps.toFixed(1) + ' Mbps</span>' +
            '</div>';

        // Codec
        html += '<div class="stats-row">' +
            '<span class="stats-label">Codec:</span>' +
            '<span class="stats-value">' + codec.toUpperCase() + '</span>' +
            '</div>';

        // Transport
        html += '<div class="stats-row">' +
            '<span class="stats-label">Transport:</span>' +
            '<span class="stats-value">' + this._transportMode + '</span>' +
            '</div>';

        // End-to-end latency
        const avgLatency = this._e2eLatencyStats.count > 0
            ? this._e2eLatencyStats.avg.toFixed(1) + 'ms'
            : '--';
        html += '<div class="stats-row stats-latency-row">' +
            '<span class="stats-label">Latency:</span>' +
            '<span class="stats-value stats-latency">' + avgLatency + '</span>' +
            '</div>';

        html += '</div>';

        this._overlayEl.innerHTML = html;
    }

    // ── Stats message handler (ping/pong + periodic backend stats) ─────────

    _handleStatsMessage(msg) {
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
            // Track steady_clock reference for end-to-end latency calculation.
            // Refine the clock domain offset when a stats message arrives.
            // The offset may already be estimated from the first video frame;
            // stats provide a more accurate measurement (no pipeline guesswork).
            if (msg.streamTimeMs !== undefined && msg.streamTimeMs >= 0) {
                this._streamTimeMs = msg.streamTimeMs;
                this._streamTimeReceiptTime = performance.now();
                // Refine offset: steadyTime ≈ streamTimeMs + RTT/2 at receive
                const rtt = this._browserRttStats.count > 0 ? this._browserRttStats.avg : 0;
                const refinedOffset = msg.streamTimeMs - this._streamTimeReceiptTime + rtt / 2;
                if (this._steadyToPerfOffset === null) {
                    this._steadyToPerfOffset = refinedOffset;
                    console.log('[StreamView] Clock offset (stats): steadyToPerfOffset=' +
                        Math.round(this._steadyToPerfOffset) +
                        ' streamTimeMs=' + msg.streamTimeMs +
                        ' perf=' + Math.round(this._streamTimeReceiptTime) +
                        ' rtt=' + rtt.toFixed(1));
                } else {
                    // Blend new measurement with existing offset (80% old, 20% new)
                    // to smooth out jitter while adapting to clock drift.
                    this._steadyToPerfOffset =
                        this._steadyToPerfOffset * 0.8 + refinedOffset * 0.2;
                }
            }
        }
    }

    handleVideoFrame(data, isKeyframe, backendTs, frameId = 0) {
        // Stop processing frames once quit() has started.  The DC may still
        // deliver queued messages during the async HTTP /quit call.
        if (this._quitting) return;

        if (data.length < 4) {
            console.warn('[StreamView] Video frame too small:', data.length);
            return;
        }

        // ── Stale frame detection (SCTP unordered delivery) ──────────────────
        // WebRTC DataChannel uses ordered=false, maxRetransmits=0 for video,
        // which allows SCTP chunks to arrive out of order.  The chunk reassembly
        // in WebRtcDataChannel emits frames as soon as they complete, regardless
        // of their original frameId sequence.  When an older frame (lower
        // backendTs) completes reassembly after a newer frame (higher backendTs)
        // has already been decoded and rendered, the old pixels overwrite the
        // current canvas content — this is the "ghosting" bug.
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
            // Only forward jumps are gaps. A frameId <= lastFrameId is a late
            // out-of-order frame (already filtered by the backendTs check above
            // in most cases) — never a loss signal.
            if (this._lastFrameId !== -1 && frameId > this._lastFrameId + 1) {
                console.warn('[StreamView] Frame gap: expected ' + (this._lastFrameId + 1) +
                    ' got ' + frameId + ' — invalidating reference, requesting IDR');
                this._referenceValid = false;
                this._requestIdr('frame gap');
            }
            if (frameId > this._lastFrameId) this._lastFrameId = frameId;
        }

        // Track cumulative video bytes for bitrate calculation
        this._totalBytes += data.length;

        // Store the most recent backend timestamp for latency calculation.
        if (backendTs !== undefined && backendTs > 0) {
            this._latestBackendTs = backendTs;

            // Establish the steady_clock → performance.now() offset as early as
            // possible — no need to wait for the periodic stats message.
            // The pipeline latency from capture to browser-receive is unknown
            // (encode + LAN + SCTP), but ~30ms is a reasonable LAN floor.
            // This offset is refined when the first stats message arrives.
            if (this._steadyToPerfOffset === null) {
                const pipelineFloor = 30; // ms — conservative LAN pipeline minimum
                this._steadyToPerfOffset = backendTs - performance.now() - pipelineFloor;
                console.log('[StreamView] Clock offset (frame-based): steadyToPerfOffset=' +
                    Math.round(this._steadyToPerfOffset) +
                    ' backendTs=' + backendTs +
                    ' perf=' + Math.round(performance.now()));
            }
        }

        // Direct frame processing — no reordering.
        this._processVideoFrame(data, isKeyframe, backendTs);
    }

    /**
     * Process a single video frame (deliver to NAL parser / decoder).
     * Called in monotonically-increasing frameId order.
     */
    _processVideoFrame(data, isKeyframe, backendTs) {
        // Log first frame details
        if (!this._firstFrameProcessed) {
            this._firstFrameProcessed = true;
            console.log('[StreamView] First video frame: isKeyframe=' + isKeyframe,
                        'size=' + data.length + ' codec=' + this.videoCodec);
            const hex = Array.from(data.slice(0, Math.min(16, data.length)))
                .map(b => b.toString(16).padStart(2, '0')).join(' ');
            console.log('[StreamView] First 16 bytes:', hex);

            // Debug: log all NAL types in the first frame (HEVC-aware)
            const nals = splitNals(data);
            const nalInfo = nals.map(n => {
                if (n.length >= 2) {
                    const hevcType = (n[0] >> 1) & 0x3F;
                    const h264Type = n[0] & 0x1F;
                    return (hevcType === 32 || hevcType === 33 || hevcType === 34 || (hevcType >= 16 && hevcType <= 21))
                        ? 'H:' + hevcType : 'A:' + h264Type;
                }
                return 'len=' + n.length;
            });
            console.log('[StreamView] First frame NAL types (H=HEVC type / A=H264 type):', nalInfo.join(', '));
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
                    console.log('[StreamView] IDR received, clearing ' +
                        this.pendingFrames.length + ' stale pending frames');
                    this.pendingFrames = [];
                    this._idrRequested = false;
                }

                console.log('[StreamView] Feeding first keyframe to NalParser...');
                const ready = this.nalParser.feed(data);
                console.log('[StreamView] NalParser.feed() returned: ready=' + ready +
                    ' sps=' + (this.nalParser.sps ? this.nalParser.sps.length : 'null') +
                    ' pps=' + (this.nalParser.pps ? this.nalParser.pps.length : 'null'));
                if (ready) {
                    console.log('[StreamView] SPS/PPS extracted from first keyframe');
                    console.log('[StreamView] SPS first byte (NAL type): 0x' +
                        this.nalParser.sps[0].toString(16) + ' type=' + (this.nalParser.sps[0] & 0x1F));
                    console.log('[StreamView] PPS first byte (NAL type): 0x' +
                        this.nalParser.pps[0].toString(16) + ' type=' + (this.nalParser.pps[0] & 0x1F));
                    this.configureDecoder();
                } else {
                    // Feed didn't find SPS/PPS — log what it found instead
                    const nals = splitNals(data);
                    console.log('[StreamView] NalParser found ' + nals.length + ' NALs, types:',
                        nals.map(n => '0x' + n[0].toString(16) + '(type=' + (n[0] & 0x1F) + ')').join(', '));
                }
            } else {
                // First frame is not a keyframe — this is a problem
                console.warn('[StreamView] First frame is NOT a keyframe! Cannot extract SPS/PPS');

                // Request an IDR if we've buffered too many delta frames without decoder config.
                // Threshold: 30 frames (~0.5s at 60fps) without a keyframe.
                if (this.pendingFrames.length > 30) {
                    console.warn('[StreamView] No keyframe after ' + this.pendingFrames.length +
                        ' frames, requesting IDR from backend');
                    this._requestIdr('no keyframe while buffering');
                }
            }
        }

        // Try to configure decoder if SPS/PPS just became available
        if (!this.decoderConfigured && this.nalParser.isReady()) {
            console.log('[StreamView] Calling configureDecoder from second check, configuring=' +
                this.decoderConfiguring);
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
                // If we requested an IDR, clear stale pending frames
                if (this._idrRequested && this.pendingFrames.length > 0) {
                    console.log('[StreamView] AV1: IDR received, clearing ' +
                        this.pendingFrames.length + ' stale pending frames');
                    this.pendingFrames = [];
                    this._idrRequested = false;
                }

                console.log('[StreamView] AV1: extracting Sequence Header OBU from first keyframe');
                const seqHeader = findSequenceHeader(data);
                if (seqHeader) {
                    console.log('[StreamView] AV1: Sequence Header OBU found, size=' + seqHeader.length);
                } else {
                    console.log('[StreamView] AV1: no Sequence Header OBU found, configuring without description');
                }
                this.configureAv1Decoder(seqHeader || undefined);
            } else {
                // Wait for a keyframe before configuring — buffer until then
                if (this.pendingFrames.length < 120) {
                    this.pendingFrames.push({ data, isKeyframe });
                }

                // Request an IDR if we've buffered too many delta frames
                if (this.pendingFrames.length > 30) {
                    console.warn('[StreamView] AV1: No keyframe after ' +
                        this.pendingFrames.length + ' frames, requesting IDR');
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

        console.log('[StreamView] configureAv1Decoder STARTED, seqHeader=' +
            (seqHeaderObu ? seqHeaderObu.length + ' bytes' : 'none'));

        const configs = buildAv1DecoderConfigs(seqHeaderObu || null);

        const tryCodecs = (index) => {
            if (index >= configs.length) {
                console.error('[StreamView] All AV1 codec configs rejected');
                this.decoderConfiguring = false;
                this.setStatus('error', 'AV1 codec not supported by browser');
                return;
            }

            const cfg = configs[index];
            console.log('[StreamView] Testing AV1 config[' + index + ']: codec=' + cfg.codec +
                ' desc=' + (cfg.description ? cfg.description.byteLength + ' bytes' : 'none'));

            VideoDecoder.isConfigSupported(cfg).then((result) => {
                if (result.supported) {
                    console.log('[StreamView] AV1 config supported: codec=' + cfg.codec);
                    try {
                        this.decoder.configure(cfg);
                        this.decoderConfigured = true;
                        this.decoderConfiguring = false;
                        console.log('[StreamView] AV1 VideoDecoder configured OK with codec=' + cfg.codec +
                            ', dequeuing ' + this.pendingFrames.length + ' pending frames');
                        // Drain pending frames using AV1 decoder path (not flushPendingFrames
                        // which calls toAvcc() and corrupts OBU data).
                        while (this.pendingFrames.length > 0) {
                            const entry = this.pendingFrames.shift();
                            this.decodeAv1Frame(entry.data, entry.isKeyframe);
                        }
                    } catch (e) {
                        console.warn('[StreamView] AV1 applyConfig failed, trying next:', e.message);
                        tryCodecs(index + 1);
                    }
                } else {
                    console.log('[StreamView] AV1 config NOT supported: codec=' + cfg.codec + ', trying next');
                    tryCodecs(index + 1);
                }
            }).catch((err) => {
                console.warn('[StreamView] AV1 isConfigSupported error for codec=' + cfg.codec + ':', err.message);
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

        // AV1 data is raw OBUs — no conversion needed (no Annex B, no AVCC).
        // The VideoDecoder parses OBU framing directly.
        try {
            const chunk = new EncodedVideoChunk({
                type: type,
                timestamp: timestamp,
                duration: 16667,
                data: data
            });
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
        // Common events (both modes)
        document.addEventListener('keydown', this._onKeyDown);
        document.addEventListener('keyup', this._onKeyUp);
        this.canvas.addEventListener('wheel', this._onWheel, { passive: false });
        this.canvas.addEventListener('contextmenu', this._onContextMenu);
        // Touch events (mobile)
        this.canvas.addEventListener('touchstart', this._onTouchStart, { passive: false });
        this.canvas.addEventListener('touchmove', this._onTouchMove, { passive: false });
        this.canvas.addEventListener('touchend', this._onTouchEnd, { passive: false });
        window.addEventListener('beforeunload', this._onBeforeUnload);
        document.addEventListener('visibilitychange', this._onVisibilityChange);

        // Mode-specific events
        if (this._gamingMode) {
            this._bindGamingEvents();
        } else {
            this._setupNormalMouse();
        }
    }

    _bindGamingEvents() {
        document.addEventListener('pointerlockchange', this._onPointerLockChange);

        // Pre-focus: cursor visible (browser default — pointer lock hides it natively)
        this.canvas.style.cursor = '';

        // Unified mousemove: absolute tracking when visible (pre-focus),
        // relative movement via pointer lock deltas when focused.
        this._onGamingMouseMove = (e) => {
            if (this._mouseFocused) {
                this.webrtc.send({ type: 'mousemove', dx: e.movementX, dy: e.movementY });
            } else {
                const rect = this.canvas.getBoundingClientRect();
                const x = Math.round(Math.max(0, Math.min(e.clientX - rect.left, rect.width)));
                const y = Math.round(Math.max(0, Math.min(e.clientY - rect.top, rect.height)));
                const refW = Math.round(rect.width);
                const refH = Math.round(rect.height);
                this.webrtc.send({
                    type: 'mousemove', x, y,
                    referenceWidth: refW, referenceHeight: refH
                });
            }
        };
        this.canvas.addEventListener('mousemove', this._onGamingMouseMove);

        // Click to capture focus: set host cursor at clicked position, then grab pointer.
        this._onGamingClick = (e) => {
            if (this._mouseFocused) return;  // Already focused — click handled by mousedown/mouseup
            e.preventDefault();

            // Send absolute position so the host cursor teleports to the clicked point
            const rect = this.canvas.getBoundingClientRect();
            const x = Math.round(Math.max(0, Math.min(e.clientX - rect.left, rect.width)));
            const y = Math.round(Math.max(0, Math.min(e.clientY - rect.top, rect.height)));
            const refW = Math.round(rect.width);
            const refH = Math.round(rect.height);
            this.webrtc.send({
                type: 'mousemove', x, y,
                referenceWidth: refW, referenceHeight: refH
            });
            this.canvas.requestPointerLock();
        };
        this.canvas.addEventListener('click', this._onGamingClick);

        // Mouse button events: only send when focused (pre-focus click only captures)
        this._onGamingMouseDown = (e) => {
            if (this._mouseFocused) this.handleMouseDown(e);
        };
        this._onGamingMouseUp = (e) => {
            if (this._mouseFocused) this.handleMouseUp(e);
        };
        this.canvas.addEventListener('mousedown', this._onGamingMouseDown);
        this.canvas.addEventListener('mouseup', this._onGamingMouseUp);
    }

    _setupNormalMouse() {
        // In non-gaming mode, mouse position is sent in absolute coordinates
        // mapped to the host screen. The cursor on the host follows the client
        // cursor position on the video canvas 1:1.
        // Hide local cursor — the host cursor is visible in the video stream.
        // On touch devices, the overlay cursor (#stream-touch-cursor) provides
        // visual feedback — keep the CSS cursor at default.
        if (!IS_TOUCH_DEVICE) {
            this.canvas.style.cursor = 'none';
        }

        this._onNormalMouseMove = (e) => {
            const rect = this.canvas.getBoundingClientRect();
            // Absolute pixel position within the canvas element
            const rawX = e.clientX - rect.left;
            const rawY = e.clientY - rect.top;

            // Clamp to canvas bounds to avoid sending out-of-range coordinates
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
                referenceHeight: refH
            });
        };

        this._onNormalMouseDown = (e) => {
            this.handleMouseDown(e);
        };

        this._onNormalMouseUp = (e) => {
            this.handleMouseUp(e);
        };

        this._onNormalMouseEnter = () => {
            if (!IS_TOUCH_DEVICE) {
                this.canvas.style.cursor = 'none';
            }
        };

        this._onNormalMouseLeave = () => {
            if (!IS_TOUCH_DEVICE) {
                this.canvas.style.cursor = 'default';
            }
        };

        this.canvas.addEventListener('mousemove', this._onNormalMouseMove);
        this.canvas.addEventListener('mousedown', this._onNormalMouseDown);
        this.canvas.addEventListener('mouseup', this._onNormalMouseUp);
        this.canvas.addEventListener('mouseenter', this._onNormalMouseEnter);
        this.canvas.addEventListener('mouseleave', this._onNormalMouseLeave);
    }

    unbindEvents() {
        document.removeEventListener('keydown', this._onKeyDown);
        document.removeEventListener('keyup', this._onKeyUp);
        document.removeEventListener('pointerlockchange', this._onPointerLockChange);
        window.removeEventListener('beforeunload', this._onBeforeUnload);
        document.removeEventListener('visibilitychange', this._onVisibilityChange);
        if (this.canvas) {
            this.canvas.removeEventListener('mousemove', this._onMouseMove);
            this.canvas.removeEventListener('mousedown', this._onMouseDown);
            this.canvas.removeEventListener('mouseup', this._onMouseUp);
            this.canvas.removeEventListener('wheel', this._onWheel);
            this.canvas.removeEventListener('contextmenu', this._onContextMenu);
            // Touch events (mobile)
            this.canvas.removeEventListener('touchstart', this._onTouchStart);
            this.canvas.removeEventListener('touchmove', this._onTouchMove);
            this.canvas.removeEventListener('touchend', this._onTouchEnd);

            // Mode-specific listeners
            if (this._gamingMode) {
                if (this._onGamingMouseMove)
                    this.canvas.removeEventListener('mousemove', this._onGamingMouseMove);
                if (this._onGamingClick)
                    this.canvas.removeEventListener('click', this._onGamingClick);
                if (this._onGamingMouseDown)
                    this.canvas.removeEventListener('mousedown', this._onGamingMouseDown);
                if (this._onGamingMouseUp)
                    this.canvas.removeEventListener('mouseup', this._onGamingMouseUp);
            } else {
                if (this._onNormalMouseMove)
                    this.canvas.removeEventListener('mousemove', this._onNormalMouseMove);
                if (this._onNormalMouseDown)
                    this.canvas.removeEventListener('mousedown', this._onNormalMouseDown);
                if (this._onNormalMouseUp)
                    this.canvas.removeEventListener('mouseup', this._onNormalMouseUp);
                if (this._onNormalMouseEnter)
                    this.canvas.removeEventListener('mouseenter', this._onNormalMouseEnter);
                if (this._onNormalMouseLeave)
                    this.canvas.removeEventListener('mouseleave', this._onNormalMouseLeave);
            }
        }
    }

    requestPointerLock() {
        if (!this.pointerLocked && this.canvas) {
            this.canvas.requestPointerLock();
        }
    }

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
            'KeyA': 0x41, 'KeyB': 0x42, 'KeyC': 0x43, 'KeyD': 0x44,
            'KeyE': 0x45, 'KeyF': 0x46, 'KeyG': 0x47, 'KeyH': 0x48,
            'KeyI': 0x49, 'KeyJ': 0x4A, 'KeyK': 0x4B, 'KeyL': 0x4C,
            'KeyM': 0x4D, 'KeyN': 0x4E, 'KeyO': 0x4F, 'KeyP': 0x50,
            'KeyQ': 0x51, 'KeyR': 0x52, 'KeyS': 0x53, 'KeyT': 0x54,
            'KeyU': 0x55, 'KeyV': 0x56, 'KeyW': 0x57, 'KeyX': 0x58,
            'KeyY': 0x59, 'KeyZ': 0x5A,
            // Digits — VK_0=0x30 through VK_9=0x39
            'Digit1': 0x31, 'Digit2': 0x32, 'Digit3': 0x33, 'Digit4': 0x34,
            'Digit5': 0x35, 'Digit6': 0x36, 'Digit7': 0x37, 'Digit8': 0x38,
            'Digit9': 0x39, 'Digit0': 0x30,
            // Special keys
            'Enter': 0x0D, 'Escape': 0x1B, 'Backspace': 0x08, 'Tab': 0x09,
            'Space': 0x20,
            'Minus': 0xBD, 'Equal': 0xBB,
            'BracketLeft': 0xDB, 'BracketRight': 0xDD, 'Backslash': 0xDC,
            'IntlBackslash': 0xE2,  // VK_OEM_102 — ISO key; backend applies SS_KBE_FLAG_NON_NORMALIZED
            'Semicolon': 0xBA, 'Quote': 0xDE, 'Backquote': 0xC0,
            'Comma': 0xBC, 'Period': 0xBE, 'Slash': 0xBF,
            'CapsLock': 0x14,
            // Function keys — VK_F1=0x70 through VK_F24=0x87
            'F1': 0x70, 'F2': 0x71, 'F3': 0x72, 'F4': 0x73,
            'F5': 0x74, 'F6': 0x75, 'F7': 0x76, 'F8': 0x77,
            'F9': 0x78, 'F10': 0x79, 'F11': 0x7A, 'F12': 0x7B,
            'F13': 0x7C, 'F14': 0x7D, 'F15': 0x7E, 'F16': 0x7F,
            'F17': 0x80, 'F18': 0x81, 'F19': 0x82, 'F20': 0x83,
            'F21': 0x84, 'F22': 0x85, 'F23': 0x86, 'F24': 0x87,
            // Navigation cluster
            'PrintScreen': 0x2C, 'ScrollLock': 0x91, 'Pause': 0x13,
            'Insert': 0x2D, 'Home': 0x24, 'PageUp': 0x21,
            'Delete': 0x2E, 'End': 0x23, 'PageDown': 0x22,
            // Arrow keys
            'ArrowRight': 0x27, 'ArrowLeft': 0x25, 'ArrowDown': 0x28, 'ArrowUp': 0x26,
            // Numpad
            'NumLock': 0x90, 'NumpadDivide': 0x6F, 'NumpadMultiply': 0x6A,
            'NumpadSubtract': 0x6D, 'NumpadAdd': 0x6B, 'NumpadEnter': 0x0D,
            'Numpad1': 0x61, 'Numpad2': 0x62, 'Numpad3': 0x63,
            'Numpad4': 0x64, 'Numpad5': 0x65, 'Numpad6': 0x66,
            'Numpad7': 0x67, 'Numpad8': 0x68, 'Numpad9': 0x69,
            'Numpad0': 0x60, 'NumpadDecimal': 0x6E,
            // Modifiers (physical position — logical state sent via ctrlKey etc.)
            'ControlLeft': 0x11, 'ShiftLeft': 0x10, 'AltLeft': 0x12, 'MetaLeft': 0x5B,
            'ControlRight': 0x11, 'ShiftRight': 0x10, 'AltRight': 0x12, 'MetaRight': 0x5C,
            // Context menu
            'ContextMenu': 0x5D,
            // International
            'IntlRo': 0x73,         // JIS \ key — backend applies SS_KBE_FLAG_NON_NORMALIZED
            'IntlYen': 0xFF,        // VK_OEM_AUTO (yen sign)
            'Lang1': 0xF2,          // VK_HANGUL
            'Lang2': 0xF1,          // VK_HANJA
            'Lang3': 0xF4,          // VK_KATAKANA
            'Lang4': 0xF3,          // VK_HIRAGANA
            'Lang5': 0xF5,          // VK_ZENKAKU
        };
        return map[code] !== undefined ? map[code] : 0;
    }

    handleKeyDown(e) {
        // Ignore all keyboard input while the stream is being shut down
        if (this._quitting) return;

        // ── CSS fallback fullscreen Escape ─────────────────────────────────
        // When in CSS fake fullscreen (no native Fullscreen API), Escape
        // exits the CSS fullscreen rather than being forwarded to the host.
        if (this._cssFullscreen && e.key === 'Escape') {
            e.preventDefault();
            this._exitCssFallbackFullscreen();
            return;
        }

        // ── Ctrl/Cmd+Alt+{Shift|Ctrl} combos ──
        //   Win: Ctrl+Alt+Shift+{Q,X,Z,M}
        //   Mac: Cmd+Option+Ctrl+{Q,X,Z,M}
        // Mac replaces Shift with Ctrl because Shift combos conflict with
        // system-level macOS keyboard shortcuts.
        const modCtrl = e.ctrlKey || e.metaKey;
        const isMac = /Mac/.test(navigator.platform);
        const modThird = isMac ? e.ctrlKey : e.shiftKey;  // Ctrl on Mac, Shift elsewhere

        // Debug: log potential combo key events so we can see exactly what
        // modifiers and key values the browser reports. Only logs when at
        // least Ctrl+Alt (or Meta+Alt) are pressed — avoids spam.
        // TODO: remove after debugging shortcut regression (2026-06-03).
        if (modCtrl && e.altKey) {
            console.log('[StreamView] Combo candidate:', {
                key: e.key,
                code: e.code,
                ctrlKey: e.ctrlKey,
                altKey: e.altKey,
                shiftKey: e.shiftKey,
                metaKey: e.metaKey,
                platform: navigator.platform,
                isMac: isMac,
                modThird: modThird,
                modThirdExpected: isMac ? 'ctrlKey' : 'shiftKey'
            });
        }

        // ── Escape key ────────────────────────────────────────────────────
        // Prevent the browser from exiting fullscreen or releasing pointer
        // lock. The Fullscreen API defaults to exiting on Escape — we must
        // intercept it here so the key is forwarded to the host session as
        // a normal VK_ESCAPE keypress instead.
        if (e.key === 'Escape') {
            e.preventDefault();
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
            const k = e.key.toLowerCase();
            const c = e.code;
            // e.code fallback only if AltGr altered e.key to a non-letter
            const isLetter = /^[a-z]$/.test(k);
            const chk = (letter, code) => k === letter || (!isLetter && c === code);

            // Quit: Ctrl+Alt+Shift+Q (Win) / Cmd+Option+Ctrl+Q (Mac)
            if (chk('q', 'KeyQ')) {
                e.preventDefault();
                this.quit();
                return;
            }
            // Fullscreen toggle: Ctrl+Alt+Shift+X (Win) / Cmd+Option+Ctrl+X (Mac)
            if (chk('x', 'KeyX')) {
                e.preventDefault();
                this.toggleFullscreen();
                return;
            }
            // Unfocus: Ctrl+Alt+Shift+Z (Win) / Cmd+Option+Ctrl+Z (Mac)
            // Releases the mouse cursor back to the OS (gaming mode only).
            if (chk('z', 'KeyZ')) {
                e.preventDefault();
                if (document.pointerLockElement === this.canvas) {
                    document.exitPointerLock();
                }
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
        this.webrtc.send({
            type: 'keydown',
            keyCode: vkCode,
            // Keep e.code for backend to detect IntlBackslash/IntlRo
            code: e.code,
            key: e.key,
            ctrlKey: e.ctrlKey,
            shiftKey: e.shiftKey,
            altKey: e.altKey,
            metaKey: e.metaKey
        });
    }

    handleKeyUp(e) {
        e.preventDefault();
        const vkCode = StreamView.codeToWindowsVk(e.code) || e.keyCode;
        this.webrtc.send({
            type: 'keyup',
            keyCode: vkCode,
            code: e.code,
            key: e.key,
            ctrlKey: e.ctrlKey,
            shiftKey: e.shiftKey,
            altKey: e.altKey,
            metaKey: e.metaKey
        });
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
        this.webrtc.send({ type: 'mousewheel', delta: e.deltaY });
    }

    // =========================================================================
    // Touch cursor overlay
    // =========================================================================

    /**
     * Position and show the touch cursor overlay at the given client coordinates.
     * The overlay provides visual feedback of where the finger is pointing,
     * which is essential on mobile where the CSS cursor is hidden.
     *
     * Coordinates are converted from client space to the canvas-area parent
     * space (the cursor element is positioned relative to .stream-canvas-area).
     * Cancels any pending auto-hide timer so the cursor stays visible during
     * an active touch sequence.
     *
     * @param {number} clientX - Pointer clientX from the TouchEvent.
     * @param {number} clientY - Pointer clientY from the TouchEvent.
     */
    _showTouchCursor(clientX, clientY) {
        if (!this._touchCursorEl || !this.canvas) return;

        const rect = this.canvas.getBoundingClientRect();
        // Center the cursor on the touch point (offset by half its size)
        const x = clientX - rect.left - this._touchCursorSize / 2;
        const y = clientY - rect.top - this._touchCursorSize / 2;

        this._touchCursorEl.style.transform = `translate(${x}px, ${y}px)`;
        this._touchCursorEl.style.display = 'block';

        // Cancel any pending auto-hide so the cursor stays during active touch
        if (this._touchCursorTimeout) {
            clearTimeout(this._touchCursorTimeout);
            this._touchCursorTimeout = null;
        }
    }

    /**
     * Schedule hiding the touch cursor after the configured delay.
     * Called when the user lifts all fingers — the cursor stays visible
     * at the last known position for a moment so the user can see where
     * the pointer ended up between gestures.
     *
     * If a new touch starts before the timeout fires, _showTouchCursor()
     * cancels this timer and the cursor remains visible.
     */
    _scheduleHideTouchCursor() {
        if (!this._touchCursorEl) return;
        if (this._touchCursorTimeout) {
            clearTimeout(this._touchCursorTimeout);
        }
        this._touchCursorTimeout = setTimeout(() => {
            if (this._touchCursorEl) {
                this._touchCursorEl.style.display = 'none';
            }
            this._touchCursorTimeout = null;
        }, this._touchCursorHideDelay);
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
    _initMobileFullscreen() {
        // Mobile or tablet only (excludes touchscreen laptops via User-Agent detection)
        if (!IS_MOBILE_OR_TABLET) return;

        // Orientation change listener
        this._orientationMql = window.matchMedia('(orientation: landscape)');
        this._onOrientationChange = (e) => {
            if (e.matches) {
                this._requestFullscreen();
            } else {
                this._exitMobileFullscreen();
            }
            this._updateMobileFsButtonVisibility();
        };
        this._orientationMql.addEventListener('change', this._onOrientationChange);

        // Fullscreen change listener (for button visibility)
        this._onFullscreenChange = () => {
            this._updateMobileFsButtonVisibility();
        };
        document.addEventListener('fullscreenchange', this._onFullscreenChange);
        if (this.videoEl) {
            this.videoEl.addEventListener('webkitbeginfullscreen', this._onFullscreenChange);
            this.videoEl.addEventListener('webkitendfullscreen', this._onFullscreenChange);
        }

        // If the constructor was called from a user click (app launch), the user
        // gesture is still active — webkitEnterFullscreen() may succeed on iOS.
        // Only auto-fullscreen if already in landscape; in portrait, let the user
        // rotate first (orientation change listener handles landscape→FS).
        if (window.matchMedia('(orientation: landscape)').matches) {
            this._requestMobileFullscreen();
        }

        // Initial button state
        this._updateMobileFsButtonVisibility();
    }

    /**
     * Enter fullscreen on the <video> element.
     *
     * Priority order:
     *   1. webkitEnterFullscreen() — native video player fullscreen (iOS Safari).
     *      Only works from a user-gesture-initiated call chain. Orientation change
     *      events are NOT user gestures, so this silently fails on rotation.
     *   2. requestFullscreen() — standard Fullscreen API (Android Chrome, others).
     */
    _requestMobileFullscreen() {
        // iOS Safari / Chrome iOS: native video player fullscreen
        if (this.videoEl && typeof this.videoEl.webkitEnterFullscreen === 'function') {
            try {
                this.videoEl.webkitEnterFullscreen();
                this._debugFullscreenSucceeded = true;
                this._debugFullscreenFailed = false;
            } catch (e) {
                // Silently ignored — rejected when not from a user gesture
                // (orientation change events are not user-initiated on iOS).
                this._debugFullscreenFailed = true;
                this._debugFullscreenSucceeded = false;
            }
            this._updateMobileFsButtonVisibility();
            return;
        }

        // Standard Fullscreen API on the document element
        // Works for Android Chrome, DataChannel mode, etc.
        if (document.documentElement.requestFullscreen) {
            this._debugFullscreenSucceeded = true; // assume success until catch
            document.documentElement.requestFullscreen()
                .then(() => {
                    this._debugFullscreenSucceeded = true;
                    this._debugFullscreenFailed = false;
                    this._updateMobileFsButtonVisibility();
                })
                .catch((err) => {
                    console.warn('[StreamView] requestFullscreen failed:', err.message);
                    this._debugFullscreenFailed = true;
                    this._debugFullscreenSucceeded = false;
                    this._updateMobileFsButtonVisibility();

                    // Fallback: try video element fullscreen
                    if (this.videoEl && this.videoEl.requestFullscreen) {
                        return this.videoEl.requestFullscreen()
                            .then(() => {
                                this._debugFullscreenSucceeded = true;
                                this._debugFullscreenFailed = false;
                                this._updateMobileFsButtonVisibility();
                            })
                            .catch(() => {
                                this._debugFullscreenFailed = true;
                                this._debugFullscreenSucceeded = false;
                                this._updateMobileFsButtonVisibility();
                            });
                    }
                });
        }
    }

    /**
     * Request fullscreen for any transport mode, with CSS fallback on failure.
     *
     * Priority:
     *   1. webkitEnterFullscreen() — native video player fullscreen (iOS Safari, video mode only).
     *   2. document.documentElement.requestFullscreen() — standard Fullscreen API.
     *   3. _enterCssFallbackFullscreen() — fake fullscreen via CSS when API fails
     *      (iOS Safari/Chrome with canvas mode where Fullscreen API is unavailable).
     */
    _requestFullscreen() {
        if (this._cssFullscreen) return;

        // ── Mode <video> (webrtc-media) — native video fullscreen ────────────
        if (this.videoEl && typeof this.videoEl.webkitEnterFullscreen === 'function') {
            try {
                this.videoEl.webkitEnterFullscreen();
                return;
            } catch (e) {
                // Silently ignored — not from a user gesture or not supported
            }
        }

        // ── Standard Fullscreen API ──────────────────────────────────────────
        if (document.documentElement.requestFullscreen) {
            document.documentElement.requestFullscreen()
                .then(() => {
                    // Success — button visibility handled by fullscreenchange
                })
                .catch((err) => {
                    console.warn('[StreamView] requestFullscreen failed:', err.message);
                    this._enterCssFallbackFullscreen();
                });
            return;
        }

        // ── No fullscreen API available — CSS fallback ───────────────────────
        this._enterCssFallbackFullscreen();
    }

    /**
     * Enter CSS-based "fake" fullscreen when the Fullscreen API is unavailable
     * (iOS Safari canvas mode). Hides the header, stretches canvas fullscreen,
     * adds a small "Exit fullscreen" overlay button.
     */
    _enterCssFallbackFullscreen() {
        if (this._cssFullscreen || this._quitting) return;
        this._cssFullscreen = true;

        // Add CSS class to the stream-view container
        const streamView = document.getElementById('stream-view');
        if (streamView) streamView.classList.add('stream-css-fs');

        // Hide the header fullscreen button
        if (this._mobileFsBtn) this._mobileFsBtn.style.display = 'none';

        // Create exit button
        if (!this._exitFsBtn) {
            this._exitFsBtn = document.createElement('button');
            this._exitFsBtn.className = 'stream-fs-exit-btn';
            this._exitFsBtn.textContent = 'Exit FS';
            this._exitFsBtn.addEventListener('click', (e) => {
                e.stopPropagation();
                this._exitCssFallbackFullscreen();
            });
            document.body.appendChild(this._exitFsBtn);
        }
    }

    /**
     * Exit CSS fallback fullscreen. Restores header, canvas area, removes
     * exit button.
     */
    _exitCssFallbackFullscreen() {
        if (!this._cssFullscreen) return;
        this._cssFullscreen = false;

        // Remove CSS class from the stream-view container
        const streamView = document.getElementById('stream-view');
        if (streamView) streamView.classList.remove('stream-css-fs');

        // Restore header fullscreen button visibility
        if (this._mobileFsBtn) this._mobileFsBtn.style.display = '';

        // Remove exit button
        if (this._exitFsBtn) {
            this._exitFsBtn.remove();
            this._exitFsBtn = null;
        }
    }

    /**
     * Exit fullscreen on mobile.
     *
     * Tries both APIs:
     *   1. webkitExitFullscreen() — exits native video player fullscreen (iOS Safari).
     *   2. document.exitFullscreen() — standard Fullscreen API exit.
     *   3. _exitCssFallbackFullscreen() — CSS fallback exit.
     */
    _exitMobileFullscreen() {
        // Also clean up CSS fallback fullscreen if active
        this._exitCssFallbackFullscreen();

        // iOS Safari: exit native video player fullscreen
        if (this.videoEl && typeof this.videoEl.webkitExitFullscreen === 'function') {
            try {
                this.videoEl.webkitExitFullscreen();
            } catch (e) { /* silently ignored */ }
        }

        // Standard Fullscreen API exit
        if (document.fullscreenElement) {
            document.exitFullscreen().catch(() => {});
        }
    }

    /**
     * Show/hide the header fullscreen button based on current state.
     *
     * Visible when: NOT in fullscreen, NOT in CSS fallback fullscreen,
     * AND in landscape orientation. Hidden when fullscreen is active,
     * CSS fallback is active, or portrait mode (mobile users should
     * rotate to landscape to enter fullscreen).
     * Always visible on desktop (no orientation constraint).
     */
    _updateMobileFsButtonVisibility() {
        if (!this._mobileFsBtn) return;

        const inFullscreen = !!document.fullscreenElement ||
            (this.videoEl && this.videoEl.webkitDisplayingFullscreen) ||
            this._cssFullscreen;

        // Hide in portrait orientation — mobile users must rotate to landscape
        // before entering fullscreen. Desktop (no matchMedia) passes through.
        const isPortrait = window.matchMedia &&
            window.matchMedia('(orientation: portrait)').matches;

        this._mobileFsBtn.style.display = (inFullscreen || isPortrait) ? 'none' : '';
    }

    // =========================================================================
    // Touch input (mobile) — trackpad-like behaviour
    // =========================================================================

    /**
     * Handle touch start on the streaming canvas.
     *
     * Behaviour:
     *   1 finger → track mouse position for drag/tap detection.
     *   2 fingers → immediate right click (mousedown+mouseup button=3).
     */
    handleTouchStart(e) {
        e.preventDefault();
        const prevCount = this._touchFingerCount;
        const newCount = e.touches.length;
        this._touchFingerCount = newCount;
        const touch = e.touches[0];
        this._touchStartX = touch.clientX;
        this._touchStartY = touch.clientY;
        this._touchLastX = touch.clientX;
        this._touchLastY = touch.clientY;
        this._touchStartTime = performance.now();
        this._touchActive = true;

        // Show touch cursor at finger position immediately
        this._showTouchCursor(touch.clientX, touch.clientY);

        // Two fingers only when starting simultaneously (prevCount === 0) → right click.
        // If the second finger is added mid-drag (prevCount === 1 → 2), we just switch
        // to scroll mode without firing a click.
        if (newCount === 2 && prevCount === 0) {
            this._touchHadTwoFingers = true;
            this.webrtc.send({ type: 'mousedown', button: 3 });
            this.webrtc.send({ type: 'mouseup', button: 3 });
            // Clear start time so touchend doesn't also fire a left click
            this._touchStartTime = 0;
            // Store average Y so first two-finger touchmove delta is correct
            const t0 = e.touches[0], t1 = e.touches[1];
            this._touchLastY = (t0.clientY + t1.clientY) / 2;
        } else if (newCount === 2 && prevCount === 1) {
            // Second finger added mid-drag — mark to prevent tap on final finger lift
            this._touchHadTwoFingers = true;
        }
    }

    /**
     * Handle touch move (drag).
     *
     * 1 finger → absolute mouse move (like desktop mouse, mapped to canvas).
     * 2 fingers → vertical scroll via mousewheel delta.
     */
    handleTouchMove(e) {
        e.preventDefault();
        if (!this._touchActive) return;

        const count = e.touches.length;
        this._touchFingerCount = count;

        if (count === 1) {
            // Single finger: absolute mouse move (trackpad mode)
            const touch = e.touches[0];
            const rect = this.canvas.getBoundingClientRect();
            const x = Math.round(Math.max(0, Math.min(touch.clientX - rect.left, rect.width)));
            const y = Math.round(Math.max(0, Math.min(touch.clientY - rect.top, rect.height)));
            const refW = Math.round(rect.width);
            const refH = Math.round(rect.height);

            this.webrtc.send({
                type: 'mousemove',
                x: x,
                y: y,
                referenceWidth: refW,
                referenceHeight: refH
            });

            // Move touch cursor to follow the finger
            this._showTouchCursor(touch.clientX, touch.clientY);
            this._touchLastX = touch.clientX;
            this._touchLastY = touch.clientY;
        } else if (count === 2) {
            // Two finger vertical swipe → scroll
            const t1 = e.touches[0];
            const t2 = e.touches[1];
            const avgY = (t1.clientY + t2.clientY) / 2;
            const deltaY = this._touchLastY ? avgY - this._touchLastY : 0;

            if (Math.abs(deltaY) > 1) {
                this.webrtc.send({ type: 'mousewheel', delta: deltaY });
            }

            // Track average Y for next delta
            this._touchLastX = t1.clientX;
            this._touchLastY = avgY;
        }
    }

    /**
     * Handle touch end.
     *
     * 1→0 finger transition with minimal movement → left click (tap).
     */
    handleTouchEnd(e) {
        e.preventDefault();

        // Update finger count before processing
        this._touchFingerCount = e.touches.length;

        // Tap detection: 1→0 finger transition, no two-finger gesture seen, no significant drag
        if (e.touches.length === 0 &&
            !this._touchHadTwoFingers &&
            e.changedTouches.length > 0) {
            const touch = e.changedTouches[0];
            const dx = touch.clientX - this._touchStartX;
            const dy = touch.clientY - this._touchStartY;
            const dist = Math.sqrt(dx * dx + dy * dy);
            const elapsed = performance.now() - this._touchStartTime;

            // Tap: minimal movement + short duration → left click
            if (dist < this._touchTapThreshold &&
                elapsed < this._touchTapTimeThreshold &&
                this._touchStartTime > 0) {
                this.webrtc.send({ type: 'mousedown', button: 1 });
                this.webrtc.send({ type: 'mouseup', button: 1 });
            }
        }

        // Reset state when all fingers are lifted
        if (e.touches.length === 0) {
            this._touchActive = false;
            this._touchHadTwoFingers = false;
            this._touchFingerCount = 0;
            // Keep cursor visible at last position, hide after delay
            this._scheduleHideTouchCursor();
        }
    }

    handlePointerLockChange() {
        this.pointerLocked = (document.pointerLockElement === this.canvas);
        this._mouseFocused = this.pointerLocked;
        if (this.hintEl) {
            this.hintEl.style.display = this.pointerLocked ? 'none' : 'flex';
        }
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
            document.exitFullscreen().catch(err => {
                console.warn('[StreamView] exitFullscreen failed:', err.message);
            });
        } else {
            // DEBUG: track fullscreen success/failure for debug badge
            this._debugFullscreenSucceeded = true; // assume success until catch // DEBUG
            document.documentElement.requestFullscreen()
                .then(() => {                                                       // DEBUG
                    this._debugFullscreenSucceeded = true;                           // DEBUG
                    this._debugFullscreenFailed = false;                             // DEBUG
                })                                                                   // DEBUG
                .catch(err => {                                                      // DEBUG
                    console.warn('[StreamView] requestFullscreen failed:', err.message);
                    this._debugFullscreenFailed = true;                              // DEBUG
                    this._debugFullscreenSucceeded = false;                          // DEBUG
                });                                                                  // DEBUG
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
                this.canvas.removeEventListener('mousemove', this._onGamingMouseMove);
            if (this._onGamingClick)
                this.canvas.removeEventListener('click', this._onGamingClick);
            if (this._onGamingMouseDown)
                this.canvas.removeEventListener('mousedown', this._onGamingMouseDown);
            if (this._onGamingMouseUp)
                this.canvas.removeEventListener('mouseup', this._onGamingMouseUp);
        } else {
            if (this._onNormalMouseMove)
                this.canvas.removeEventListener('mousemove', this._onNormalMouseMove);
            if (this._onNormalMouseDown)
                this.canvas.removeEventListener('mousedown', this._onNormalMouseDown);
            if (this._onNormalMouseUp)
                this.canvas.removeEventListener('mouseup', this._onNormalMouseUp);
            if (this._onNormalMouseEnter)
                this.canvas.removeEventListener('mouseenter', this._onNormalMouseEnter);
            if (this._onNormalMouseLeave)
                this.canvas.removeEventListener('mouseleave', this._onNormalMouseLeave);
        }

        // ── Exit pointer lock if leaving gaming mode ────────────────────
        if (this._gamingMode && document.pointerLockElement === this.canvas) {
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

        console.log('[StreamView] Mouse mode toggled: ' +
            (this._gamingMode ? 'gaming (relative+lock)' : 'desktop (absolute)'));

        Toast.info('Mouse mode: ' + (this._gamingMode ? 'Gaming' : 'Desktop'));
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
        const isMac = /Mac/.test(navigator.platform);
        const modA = isMac ? 'Cmd' : 'Ctrl';          // Primary modifier
        const modB = isMac ? 'Option' : 'Alt';         // Secondary modifier
        const modC = isMac ? 'Ctrl' : 'Shift';         // Tertiary modifier

        // Win order: Shift + Ctrl + Alt + ?
        // Mac order: Ctrl  + Option + Cmd + ?
        const comboWin  = [modC, modA, modB];
        const comboMac  = [modC, modB, modA];
        const comboMods = isMac ? comboMac : comboWin;

        const rows = [
            ['Quit session',             ...comboMods, 'Q'],
            ['Fullscreen',               ...comboMods, 'X'],
            ['Release mouse/keyboard',   ...comboMods, 'Z'],
            ['Change mouse mode',        ...comboMods, 'M'],
        ];

        let html = '<div class="shortcuts-slide-title">Keyboard shortcuts</div>';
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
     * Show the shortcuts slide and set a 5-second auto-hide timer.
     * Safe to call multiple times — resets the timer each call.
     */
    _showShortcutsSlide() {
        if (!this._shortcutsSlide) return;
        this._shortcutsSlide.style.display = '';

        if (this._shortcutsTimeout) {
            clearTimeout(this._shortcutsTimeout);
        }
        this._shortcutsTimeout = setTimeout(() => {
            this._hideShortcutsSlide();
        }, 5000);
    }

    /**
     * Immediately hide the shortcuts slide and clear the auto-hide timer.
     */
    _hideShortcutsSlide() {
        if (this._shortcutsTimeout) {
            clearTimeout(this._shortcutsTimeout);
            this._shortcutsTimeout = null;
        }
        if (this._shortcutsSlide) {
            this._shortcutsSlide.style.display = 'none';
        }
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
        if (!this._startupOverlay) return;
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
    }

    /**
     * Fade out and hide the startup overlay.
     * Called ~1.5s after the first video frame is decoded (step 3).
     */
    _hideStartupOverlay() {
        if (!this._startupOverlay) return;
        this._startupOverlay.classList.add('hidden');
        // Remove from DOM after the CSS transition completes
        setTimeout(() => {
            if (this._startupOverlay) {
                this._startupOverlay.style.display = 'none';
            }
        }, 500);
    }

    // DEBUG: orientation debug badge — real-time update (called every 200ms)
    _updateDebugOrientation() {
        if (!this._debugOrientationEl) return;
        const mql = window.matchMedia('(orientation: landscape)');
        const orient = mql.matches ? 'landscape' : 'portrait';
        const screenType = screen?.orientation?.type || 'N/A';
        const fsOk = this._debugFullscreenSucceeded ? 'OK'
            : this._debugFullscreenFailed ? 'FAIL' : '--';
        this._debugOrientationEl.textContent =
            'orient: ' + orient +
            '\nmql:    ' + mql.matches +
            '\nscreen: ' + screenType +
            '\nfs:     ' + fsOk;
    }
    // DEBUG end

    // =========================================================================
    // Quit / Cleanup
    // =========================================================================

    async quit() {
        // Guard: prevent re-entrant calls (e.g. from WS onClose -> setTimeout)
        if (this._quitting) return;
        this._quitting = true;

        // Exit fullscreen if active (before unbinding events).
        // Covers both standard Fullscreen API and iOS webkitExitFullscreen.
        // Also exit CSS fallback fullscreen if active.
        this._exitCssFallbackFullscreen();
        this._exitMobileFullscreen();
        if (document.fullscreenElement) {
            document.exitFullscreen().catch(() => {});
        }

        this.stopRenderLoop();
        this.stopDiagnostics();
        this.unbindEvents();

        // Clear stats overlay timer
        if (this._overlayInterval) {
            clearInterval(this._overlayInterval);
            this._overlayInterval = null;
        }
        if (this._overlayEl) {
            this._overlayEl.style.display = 'none';
        }

        // Hide shortcuts slide immediately (session is ending)
        this._hideShortcutsSlide();

        // Hide startup overlay if still visible
        this._hideStartupOverlay();

        // Clear ping timer
        if (this._pingInterval) {
            clearInterval(this._pingInterval);
            this._pingInterval = null;
        }

        // Clear IDR request timer
        if (this._idrTimeout) {
            clearTimeout(this._idrTimeout);
            this._idrTimeout = null;
        }

        // Clear touch cursor timer and hide the overlay
        if (this._touchCursorTimeout) {
            clearTimeout(this._touchCursorTimeout);
            this._touchCursorTimeout = null;
        }
        if (this._touchCursorEl) {
            this._touchCursorEl.style.display = 'none';
        }

        // Remove mobile orientation fullscreen listener
        if (this._orientationMql && this._onOrientationChange) {
            this._orientationMql.removeEventListener('change', this._onOrientationChange);
            this._orientationMql = null;
            this._onOrientationChange = null;
        }

        // DEBUG: clean up orientation debug overlay
        if (this._debugOrientationInterval) {
            clearInterval(this._debugOrientationInterval);
            this._debugOrientationInterval = null;
        }
        if (this._debugOrientationEl) {
            this._debugOrientationEl.remove();
            this._debugOrientationEl = null;
        }
        // DEBUG end

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

        if (document.pointerLockElement === this.canvas) {
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
            } catch (e) { /* ignore */ }
            this.decoder = null;
        }

        // Close AudioPipeline
        if (this.audioPipeline) {
            this.audioPipeline.close();
        }

        // Close any pending frames
        for (const frame of this.frameQueue) {
            try { frame.close(); } catch (e) { /* ignore */ }
        }
        this.frameQueue = [];
        this.pendingFrames = [];

        // Mark WebRTC as stopping before calling /quit so that DataChannel
        // "onclose" events from the backend closing its side are not treated
        // as unexpected errors.  Then send the HTTP request while the relay
        // is still reachable.
        this.webrtc.markStopping();

        try {
            await BackendClient.quitApp(this.host.uuid);
            this.webrtc.close();
            await Toast.dismissAll();
            Toast.success('Stream end');
        } catch (err) {
            console.warn('[StreamView] Quit failed:', err);
            this.webrtc.close();
        }

        this.destroy();

        // Notify MoonlightApp that streaming ended (restores apps/hosts view).
        if (this.onQuit) {
            const cb = this.onQuit;
            this.onQuit = null;  // Fire once
            cb();
        }
    }

    destroy() {
        this._exitCssFallbackFullscreen();
        this.stopRenderLoop();
        this.stopDiagnostics();
        this.unbindEvents();
        this.webrtc.close();

        if (this._pingInterval) {
            clearInterval(this._pingInterval);
            this._pingInterval = null;
        }

        if (this._idrTimeout) {
            clearTimeout(this._idrTimeout);
            this._idrTimeout = null;
        }

        if (this._touchCursorTimeout) {
            clearTimeout(this._touchCursorTimeout);
            this._touchCursorTimeout = null;
        }

        // Remove mobile orientation fullscreen listener
        if (this._orientationMql && this._onOrientationChange) {
            this._orientationMql.removeEventListener('change', this._onOrientationChange);
            this._orientationMql = null;
            this._onOrientationChange = null;
        }

        // DEBUG: clean up orientation debug overlay
        if (this._debugOrientationInterval) {
            clearInterval(this._debugOrientationInterval);
            this._debugOrientationInterval = null;
        }
        if (this._debugOrientationEl) {
            this._debugOrientationEl.remove();
            this._debugOrientationEl = null;
        }
        // DEBUG end

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

        if (document.pointerLockElement === this.canvas) {
            document.exitPointerLock();
        }

        if (this.decoder) {
            try { this.decoder.close(); } catch (e) { /* ignore */ }
            this.decoder = null;
        }
        this.decoderConfigured = false;
        this.nalParser.reset();

        if (this.audioPipeline) {
            this.audioPipeline.close();
        }

        for (const frame of this.frameQueue) {
            try { frame.close(); } catch (e) { /* ignore */ }
        }
        this.frameQueue = [];
        this.pendingFrames = [];

        const el = document.getElementById('stream-view');
        if (el) el.remove();
    }
}
