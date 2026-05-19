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
    CODEC_H264,
    CODEC_HEVC
} from '../util/Mp4Muxer.js';
import {
    findSequenceHeader,
    buildAv1DecoderConfigs,
    CODEC_AV1
} from '../util/Av1Utils.js';

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
    constructor(container, signalingUrl, host, videoCodec, gamingMode = true, upnpEnabled = true, upnpAvailable = true, transport = 'webrtc') {
        this.container = container;
        this.signalingUrl = signalingUrl;
        this.host = host;
        this.videoCodec = videoCodec || 'auto';
        this._transport = transport;
        this._gamingMode = gamingMode;
        this._upnpEnabled = upnpEnabled;
        this._upnpAvailable = upnpAvailable;

        /** Callback invoked after quit() completes cleanup. Used by MoonlightApp
         *  to restore the underlying main view (apps/hosts). */
        this.onQuit = null;

        // ── UPnP status toast ─────────────────────────────────────────────────
        if (this._upnpEnabled) {
            if (this._upnpAvailable) {
                Toast.success('UPnP active — port mapped');
            } else {
                Toast.warning('UPnP not available — connections from outside your LAN will rely on STUN relay (may fail on strict NATs).');
            }
        }
        if (this._transport === 'webrtc-media') {
            this.webrtc = new WebRtcMedia(signalingUrl);
        } else if (this._transport === 'wss') {
            // Legacy WSS mode: direct WebSocket passthrough via StreamRelay,
            // without any WebRTC PeerConnection or DataChannels.
            this.webrtc = new WebRtcDataChannel(signalingUrl, { wssMode: true });
        } else {
            this.webrtc = new WebRtcDataChannel(signalingUrl);
        }
        this.pointerLocked = false;

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

        // ── Stats overlay (Amelioration 2) ───────────────────────────────────
        this._hostRttStats = new SlidingStats(5000);       // backed RTT (ms)
        this._browserRttStats = new SlidingStats(5000);    // browser ping/pong RTT (ms)
        this._decodeLatencyStats = new SlidingStats(5000); // backend decode latency (ms)
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

        // IDR request state: if no keyframe arrives after buffering many frames
        // without decoder config, we ask the backend to request an IDR from Sunshine.
        // This is a safety net for the rare race where the initial IDR is lost.
        this._idrRequested = false;
        this._idrTimeout = null;

        // Decoder recovery guard: prevents re-entrant recovery when the error
        // callback fires during setupDecoder() (called from within recovery).
        this._decoderRecovering = false;
        // Recovery attempt counter + limit: prevents infinite recovery loops if
        // the bitstream is fundamentally corrupted (e.g., persistent packet loss).
        this._recoveryAttempts = 0;
        this.MAX_RECOVERY_ATTEMPTS = 10;

        // Guard flag to prevent re-entrant quit() calls
        this._quitting = false;

        this.render();

        // Hide the "click to capture" hint in normal mode (no pointer lock)
        if (!this._gamingMode && this.hintEl) {
            this.hintEl.style.display = 'none';
        }

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
                ' decoder=' + (this.decoder ? 'exists' : 'null') +
                ' audio.ready=' + (this.audioPipeline ? this.audioPipeline.ready : 'no') +
                ' audio.samples=' + (this.audioPipeline ? this.audioPipeline.getStats().writtenSamples : 0) +
                ' audio.underrun=' + (this.audioPipeline ? this.audioPipeline.getStats().underrunCount : 0) +
                (this._lastFrameColorSpace ? ' cs=' + JSON.stringify(this._lastFrameColorSpace) : ''));
        }, 2000);
    }

    stopDiagnostics() {
        if (this._diagHandle) {
            clearInterval(this._diagHandle);
            this._diagHandle = null;
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
                <div class="stream-status" id="stream-status">
                    <span class="stream-status-dot status-connecting"></span>
                    Connecting...
                </div>
                <div class="stream-codec-badge" id="stream-codec-badge">${this.videoCodec.toUpperCase()}</div>
                <button class="btn stream-quit-btn" id="btn-stream-quit">Stop Streaming</button>
            </div>
            <div class="stream-canvas-area">
                <canvas id="stream-canvas" class="stream-canvas"></canvas>
                <video id="stream-video" class="stream-video" autoplay muted playsinline
                       style="width:100%;height:100%;object-fit:contain;display:none;"></video>
                <div class="stream-click-hint" id="stream-hint">
                    Click to capture mouse & keyboard
                </div>
            </div>
        `;
        document.getElementById('app').appendChild(el);

        this.canvas = document.getElementById('stream-canvas');
        this.ctx = this.canvas.getContext('2d');
        // Set initial canvas size; will be adjusted when first frame arrives
        this.canvas.width = 1920;
        this.canvas.height = 1080;

        // Video element for native RTP media track mode (webrtc-media)
        this.videoEl = document.getElementById('stream-video');
        if (this._transport === 'webrtc-media') {
            this.canvas.style.display = 'none';
            this.videoEl.style.display = 'block';
            // Pass video element to WebRtcMedia for media track rendering
            if (this.webrtc && typeof this.webrtc.setVideoElement === 'function') {
                this.webrtc.setVideoElement(this.videoEl);
            }
        }

        this.statusEl = document.getElementById('stream-status');
        this.hintEl = document.getElementById('stream-hint');

        document.getElementById('btn-stream-quit').onclick = () => this.quit();

        // ── Streaming stats overlay (top-right, semi-transparent) ──────────
        this._overlayEl = document.createElement('div');
        this._overlayEl.id = 'stream-stats-overlay';
        this._overlayEl.style.cssText =
            'position:fixed;top:10px;right:10px;' +
            'background:rgba(0,0,0,0.55);color:#ccc;' +
            'font:11px monospace;' +
            'padding:6px 10px;border-radius:6px;' +
            'z-index:100;pointer-events:none;' +
            'white-space:pre;display:none;';
        this._overlayEl.textContent = 'Waiting...';
        document.getElementById('stream-view').appendChild(this._overlayEl);

        // Start overlay update timer (every 500ms)
        this._overlayInterval = setInterval(() => this._updateOverlay(), 500);
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
                console.log('[StreamView] Decoder OUTPUT callback fired, frame=' +
                    (frame.displayWidth || frame.codedWidth) + 'x' + (frame.displayHeight || frame.codedHeight) +
                    ' format=' + (frame.format || 'null') +
                    ' timestamp=' + frame.timestamp);
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
        this._idrRequested = true;
        if (this.webrtc) {
            console.log('[StreamView] Requesting IDR after decoder error');
            this.webrtc.send({ type: 'requestidr' });
        }

        this.setStatus('connecting', 'Recovering...');

        this._decoderRecovering = false;
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

        const applyConfig = (cfg) => {
            try {
                console.log('[StreamView] Calling decoder.configure() with codec=' + cfg.codec +
                    ' descriptionLen=' + (cfg.description ? cfg.description.byteLength : 0));
                this.decoder.configure(cfg);
                this.decoderConfigured = true;
                this.decoderConfiguring = false;
                console.log('[StreamView] VideoDecoder configured OK with codec=' + cfg.codec +
                            ', state=' + this.decoder.state +
                            ', dequeuing ' + this.pendingFrames.length + ' pending frames');
                this.flushPendingFrames();
                return true;
            } catch (e) {
                console.error('[StreamView] decoder.configure() failed:', e.message, e);
                if (this.decoder) {
                    console.log('[StreamView] Decoder state after failed configure: ' + this.decoder.state);
                }
                this.decoderConfiguring = false;
                return false;
            }
        };

        // Try primary config first (with avcC description and SPS-derived codec)
        const tryCodecs = (configs, index) => {
            if (index >= configs.length) {
                console.error('[StreamView] All codec configs rejected');
                this.decoderConfiguring = false;
                this.setStatus('error', 'Codec not supported by browser');
                return;
            }

            const cfg = configs[index];
            console.log('[StreamView] Testing config[' + index + ']: codec=' + cfg.codec);
            VideoDecoder.isConfigSupported(cfg).then((result) => {
                console.log('[StreamView] isConfigSupported returned: supported=' +
                    result.supported + ' for codec=' + cfg.codec);
                if (result.supported) {
                    console.log('[StreamView] Config supported: codec=' + cfg.codec +
                                ' hasDescription=' + !!cfg.description);
                    if (!applyConfig(cfg)) {
                        console.log('[StreamView] applyConfig failed, trying next config');
                        tryCodecs(configs, index + 1);
                    }
                } else {
                    console.warn('[StreamView] Config NOT supported: codec=' + cfg.codec +
                                 ', trying next');
                    tryCodecs(configs, index + 1);
                }
            }).catch((err) => {
                console.error('[StreamView] isConfigSupported error for codec=' +
                              cfg.codec + ':', err.message, err);
                tryCodecs(configs, index + 1);
            });
        };

        // Build configs: primary + fallbacks, all with codec description (avcC or hvcC).
        // Description data is in AVCC/HEVC format (4-byte length prefixes).
        // decodeFrame() converts Annex B to this format when descriptor is enabled.
        const configsToTry = [];
        const fallbacks = (codecType === CODEC_HEVC)
            ? HEVC_FALLBACK_CODEC_STRINGS
            : H264_FALLBACK_CODEC_STRINGS;

        // Explicit BT.709 limited-range — the H.264 broadcast standard.
        // Some Chrome versions ignore VUI and default to BT.601 full-range,
        // causing color shifts (washed out / oversaturated).
        // Only for H.264: Chrome WebCodecs rejects HEVC configs that include
        // an explicit colorSpace (likely a Chrome bug / incomplete support).
        const isH264 = (codecType === CODEC_H264);
        const bt709 = isH264 ? {
            colorSpace: {
                primaries: 'bt709',
                transfer: 'bt709',
                matrix: 'bt709',
                fullRange: false
            }
        } : {};

        configsToTry.push({
            codec: codec,
            description: desc.buffer,
            codedWidth: 1920,
            codedHeight: 1080,
            optimizeForLatency: true,
            ...bt709
        });

        for (const fbCodec of fallbacks) {
            if (fbCodec === codec) continue;
            configsToTry.push({
                codec: fbCodec,
                description: desc.buffer,
                codedWidth: 1920,
                codedHeight: 1080,
                optimizeForLatency: true,
                ...bt709
            });
        }

        console.log('[StreamView] Trying ' + configsToTry.length + ' codec configs (' +
            (codecType === CODEC_HEVC ? 'HEVC' : 'H.264') + ')');
        tryCodecs(configsToTry, 0);
    }

    flushPendingFrames() {
        console.log('[StreamView] flushPendingFrames: draining ' + this.pendingFrames.length + ' frames');
        while (this.pendingFrames.length > 0) {
            const entry = this.pendingFrames.shift();
            this.decodeFrame(entry.data, entry.isKeyframe);
        }
        console.log('[StreamView] flushPendingFrames done, decoder state=' + this.decoder.state);
    }

    decodeFrame(data, isKeyframe) {
        if (!this.decoderConfigured) {
            // Buffer until decoder is ready (limit to avoid OOM)
            if (this.pendingFrames.length < 120) {
                this.pendingFrames.push({ data, isKeyframe });
                if (this.pendingFrames.length === 1) {
                    console.log('[StreamView] First frame buffered (pending), waiting for decoder config');
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

        const timestamp = this.frameCount * 16667; // ~60 fps in microseconds
        this.frameCount++;

        const type = isKeyframe ? 'key' : 'delta';

        // Convert Annex B (start codes) to AVCC (4-byte length prefixes).
        const avccData = toAvcc(data, this.decoderConfigured, this.nalParser.codec);

        try {
            const chunk = new EncodedVideoChunk({
                type: type,
                timestamp: timestamp,
                duration: 16667,
                data: avccData
            });
            console.log('[StreamView] Decoding frame #' + this.frameCount +
                ' type=' + type + ' avccSize=' + avccData.length +
                ' decoderState=' + this.decoder.state);
            this.decoder.decode(chunk);
            this.stats.received++;
            // Log after every 60th frame
            if (this.frameCount % 60 === 0) {
                console.log('[StreamView] Stats: frameCount=' + this.frameCount +
                    ' received=' + this.stats.received +
                    ' decoded=' + this.stats.decoded +
                    ' rendered=' + this.stats.rendered);
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

        // Latency: compute e2e delay from backend timestamp to now
        if (this._latestBackendTs !== undefined) {
            const latency = performance.now() - this._latestBackendTs;
            if (latency > 0 && latency < 5000) { // ignore outliers
                this._latencySamples.push({ time: performance.now(), latency });
            }
        }

        if (this.stats.decoded <= 3) {
            const cs = frame.colorSpace;
            console.log('[StreamView] onDecodedFrame #' + this.stats.decoded +
                ' displaySize=' + (frame.displayWidth || '?') + 'x' + (frame.displayHeight || '?') +
                ' format=' + (frame.format || '?') +
                ' colorSpace=' + JSON.stringify({
                    primaries: cs?.primaries,
                    transfer: cs?.transfer,
                    matrix: cs?.matrix,
                    fullRange: cs?.fullRange
                }));
        }
        if (this.stats.decoded === 1) {
            const cs = frame.colorSpace;
            this._lastFrameColorSpace = cs ? {
                p: cs.primaries, t: cs.transfer, m: cs.matrix, f: cs.fullRange
            } : null;
        }

        // Limit queue depth to 3 to keep latency low
        if (this.frameQueue.length >= 3) {
            const old = this.frameQueue.shift();
            old.close();
            this.stats.dropped++;
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
            // Show stats overlay
            if (this._overlayEl) this._overlayEl.style.display = '';
        }
    }

    startRenderLoop() {
        if (this.renderRunning) return;
        // Media track mode: video is rendered natively via <video>, no canvas loop needed.
        if (this._transport === 'webrtc-media') return;
        this.renderRunning = true;

        const loop = (now) => {
            if (!this.renderRunning) return;

            // Dequeue and render the latest frame
            while (this.frameQueue.length > 0) {
                const frame = this.frameQueue.shift();

                // Resize canvas to match frame dimensions if needed
                if (frame.displayWidth && frame.displayHeight &&
                    (this.canvas.width !== frame.displayWidth ||
                     this.canvas.height !== frame.displayHeight)) {
                    this.canvas.width = frame.displayWidth;
                    this.canvas.height = frame.displayHeight;
                }

                // Draw frame to canvas
                this.ctx.drawImage(frame, 0, 0,
                    this.canvas.width, this.canvas.height);
                frame.close();
                this.stats.rendered++;

                // Log stats periodically
                if (this.stats.rendered % 60 === 0) {
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

    // =========================================================================
    // WebRTC DataChannel (replaces legacy WebSocket binary transport)
    // =========================================================================

    setupWebRtc() {
        this.webrtc.onOpen = () => {
            if (this._quitting) return;
            this.connected = true;
            this.setStatus('connecting', 'Waiting for stream...');

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
                            if (this._overlayEl) this._overlayEl.style.display = '';
                        }
                    };
                }
            } else {
                // DataChannel mode: set up WebCodecs decoder
                this.setupDecoder();

                // Safety timeout: if no decoder config after 3 seconds, request an IDR.
                this._idrTimeout = setTimeout(() => {
                    if (!this.nalParser.isReady() && !this._idrRequested) {
                        this._idrRequested = true;
                        console.warn('[StreamView] No keyframe after 3s, requesting IDR from backend');
                        this.webrtc.send({ type: 'requestidr' });
                    }
                }, 3000);
            }
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
            this.webrtc.onVideo = (frame, isKeyframe, backendTs) => this.handleVideoFrame(frame, isKeyframe, backendTs);
        }
        // Audio samples (PCM16 stereo interleaved) -> AudioPipeline
        this.webrtc.onAudio = (sample) => this.handleAudioSample(sample);
        this.webrtc.connect();
    }

    // ── Stats overlay (refreshed every 500ms) ────────────────────────────

    _updateOverlay() {
        if (!this._overlayEl) return;

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

        // Build enriched overlay lines
        const lines = [];

        // Line 1: basic stream info
        lines.push(
            (this._resolution || '?') + ' | ' +
            fps + ' fps | ' +
            bitrateMbps.toFixed(1) + ' Mbps | ' +
            codec + ' | ' +
            this._transport
        );

        // Line 2: host RTT (backend ↔ Sunshine)
        if (this._hostRttStats.count > 0) {
            const avg = this._hostRttStats.avg.toFixed(1);
            const min = this._hostRttStats.min.toFixed(1);
            const max = this._hostRttStats.max.toFixed(1);
            lines.push('RTT host: ' + avg + 'ms [' + min + '-' + max + ']');
        }

        // Line 3: browser RTT (browser ↔ backend)
        if (this._browserRttStats.count > 0) {
            const avg = this._browserRttStats.avg.toFixed(1);
            const min = this._browserRttStats.min.toFixed(1);
            const max = this._browserRttStats.max.toFixed(1);
            lines.push('RTT browser: ' + avg + 'ms [' + min + '-' + max + ']');
        }

        // Line 4: decode latency
        if (this._decodeLatencyStats.count > 0) {
            const avg = this._decodeLatencyStats.avg.toFixed(1);
            const min = this._decodeLatencyStats.min.toFixed(1);
            const max = this._decodeLatencyStats.max.toFixed(1);
            lines.push('Decode: ' + avg + 'ms [' + min + '-' + max + ']');
        }

        // Line 5: frame counts
        lines.push('Frames: R:' + this.stats.received +
            ' D:' + this.stats.decoded +
            ' Dr:' + this.stats.dropped);

        this._overlayEl.textContent = lines.join('\n');
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
        }
    }

    handleVideoFrame(data, isKeyframe, backendTs) {
        // Stop processing frames once quit() has started.  The DC may still
        // deliver queued messages during the async HTTP /quit call.
        if (this._quitting) return;

        if (data.length < 4) {
            console.warn('[StreamView] Video frame too small:', data.length);
            return;
        }

        // Track cumulative video bytes for bitrate calculation
        this._totalBytes += data.length;

        // Store the most recent backend timestamp for latency calculation.
        // We use the latest value rather than per-frame matching because
        // VideoDecoder decodes asynchronously — the simplest robust approach
        // is to take the most recently submitted frame's backendTs as a
        // proxy for the next decoded frame.  At 60fps the error is <16ms.
        if (backendTs !== undefined) {
            this._latestBackendTs = backendTs;
        }

        // Log first frame details
        if (!this._firstFrameProcessed) {
            this._firstFrameProcessed = true;
            console.log('[StreamView] First video frame: isKeyframe=' + isKeyframe,
                        'size=' + data.length + ' codec=' + this.videoCodec);
            const hex = Array.from(data.slice(0, Math.min(16, data.length)))
                .map(b => b.toString(16).padStart(2, '0')).join(' ');
            console.log('[StreamView] First 16 bytes:', hex);
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
                if (this.pendingFrames.length > 30 && !this._idrRequested) {
                    this._idrRequested = true;
                    console.warn('[StreamView] No keyframe after ' + this.pendingFrames.length +
                        ' frames, requesting IDR from backend');
                    this.webrtc.send({ type: 'requestidr' });
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
        this.decodeFrame(data, isKeyframe);
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
                if (this.pendingFrames.length > 30 && !this._idrRequested) {
                    this._idrRequested = true;
                    console.warn('[StreamView] AV1: No keyframe after ' +
                        this.pendingFrames.length + ' frames, requesting IDR');
                    this.webrtc.send({ type: 'requestidr' });
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

        // Mode-specific events
        if (this._gamingMode) {
            this._bindGamingEvents();
        } else {
            this._setupNormalMouse();
        }
    }

    _bindGamingEvents() {
        document.addEventListener('pointerlockchange', this._onPointerLockChange);
        this.canvas.addEventListener('mousemove', this._onMouseMove);
        this.canvas.addEventListener('click', () => this.requestPointerLock());
    }

    _setupNormalMouse() {
        // Track absolute mouse position to compute relative deltas
        this._mouseX = -1;
        this._mouseY = -1;
        this._dragCount = 0;

        // Hide cursor by default (like gaming mode), show during drag
        this.canvas.style.cursor = 'none';

        this._onNormalMouseMove = (e) => {
            const rect = this.canvas.getBoundingClientRect();
            const newX = e.clientX - rect.left;
            const newY = e.clientY - rect.top;

            // On first move after re-entering, skip delta (no reference point)
            if (this._mouseX >= 0 && this._mouseY >= 0) {
                const dx = newX - this._mouseX;
                const dy = newY - this._mouseY;
                this.webrtc.send({ type: 'mousemove', dx, dy });
            }

            this._mouseX = newX;
            this._mouseY = newY;
        };

        this._onNormalMouseDown = (e) => {
            this._dragCount++;
            this.canvas.style.cursor = 'default';
            this.handleMouseDown(e);
        };

        this._onNormalMouseUp = (e) => {
            this._dragCount--;
            if (this._dragCount <= 0) {
                this._dragCount = 0;
                this.canvas.style.cursor = 'none';
            }
            this.handleMouseUp(e);
        };

        this._onNormalMouseEnter = () => {
            // Reset tracking when mouse re-enters canvas
            this._mouseX = -1;
            this._mouseY = -1;
        };

        this._onNormalMouseLeave = () => {
            this._dragCount = 0;
            this.canvas.style.cursor = 'default';
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
        if (this.canvas) {
            this.canvas.removeEventListener('mousemove', this._onMouseMove);
            this.canvas.removeEventListener('mousedown', this._onMouseDown);
            this.canvas.removeEventListener('mouseup', this._onMouseUp);
            this.canvas.removeEventListener('wheel', this._onWheel);
            this.canvas.removeEventListener('contextmenu', this._onContextMenu);

            // Normal mouse listeners (only bound in non-gaming mode)
            if (!this._gamingMode) {
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

    handleKeyDown(e) {
        // Quit combo: Shift+Ctrl+Alt+E (Windows/Linux) / Shift+Ctrl+Option+E (Mac)
        // Works in both gaming and normal mode.
        if (e.shiftKey && e.ctrlKey && e.altKey && e.code === 'KeyE') {
            e.preventDefault();
            this.quit();
            return;
        }

        e.preventDefault();
        this.webrtc.send({
            type: 'keydown',
            key: e.key,
            code: e.code,
            keyCode: e.keyCode,
            ctrlKey: e.ctrlKey,
            shiftKey: e.shiftKey,
            altKey: e.altKey,
            metaKey: e.metaKey
        });
    }

    handleKeyUp(e) {
        e.preventDefault();
        this.webrtc.send({
            type: 'keyup',
            key: e.key,
            code: e.code,
            keyCode: e.keyCode,
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

    handlePointerLockChange() {
        this.pointerLocked = (document.pointerLockElement === this.canvas);
        if (this.hintEl) {
            this.hintEl.style.display = this.pointerLocked ? 'none' : 'flex';
        }
    }

    // =========================================================================
    // Status
    // =========================================================================

    setStatus(state, text) {
        if (!this.statusEl) return;
        const dot = this.statusEl.querySelector('.stream-status-dot');
        if (dot) {
            dot.className = 'stream-status-dot status-' + state;
        }
        this.statusEl.childNodes[1].textContent = ' ' + text;
    }

    // =========================================================================
    // Quit / Cleanup
    // =========================================================================

    async quit() {
        // Guard: prevent re-entrant calls (e.g. from WS onClose -> setTimeout)
        if (this._quitting) return;
        this._quitting = true;

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
            Toast.success('Stream ended');
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
