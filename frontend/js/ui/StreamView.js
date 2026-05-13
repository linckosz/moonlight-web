/**
 * Fullscreen streaming overlay with WebCodecs video decoding and Canvas rendering.
 *
 * Receives raw H.264 Annex B frames over WebSocket from the backend relay,
 * decodes them via WebCodecs VideoDecoder, and renders to a <canvas> element.
 * Captures keyboard/mouse events and sends them back as JSON over the same WS.
 */
import { WebSocketClient } from '../api/WebSocketClient.js';
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
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

export class StreamView {
    constructor(container, wsUrl, host, videoCodec, gamingMode = true) {
        this.container = container;
        this.wsUrl = wsUrl;
        this.host = host;
        this.videoCodec = videoCodec || 'auto';
        this._gamingMode = gamingMode;
        this.ws = new WebSocketClient(wsUrl);
        this.pointerLocked = false;

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

        // Bound handlers
        this._onKeyDown = (e) => this.handleKeyDown(e);
        this._onKeyUp = (e) => this.handleKeyUp(e);
        this._onMouseMove = (e) => this.handleMouseMove(e);
        this._onMouseDown = (e) => this.handleMouseDown(e);
        this._onMouseUp = (e) => this.handleMouseUp(e);
        this._onWheel = (e) => this.handleWheel(e);
        this._onPointerLockChange = () => this.handlePointerLockChange();
        this._onContextMenu = (e) => e.preventDefault();

        // Guard flag to prevent re-entrant quit() calls
        this._quitting = false;

        this.render();

        // Hide the "click to capture" hint in normal mode (no pointer lock)
        if (!this._gamingMode && this.hintEl) {
            this.hintEl.style.display = 'none';
        }

        this.setupWebSocket();
        this.bindEvents();
        this.startRenderLoop();
        this.startDiagnostics();
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
                ' decoder=' + (this.decoder ? 'exists' : 'null'));
        }, 2000);
    }

    stopDiagnostics() {
        if (this._diagHandle) {
            clearInterval(this._diagHandle);
            this._diagHandle = null;
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
        this.statusEl = document.getElementById('stream-status');
        this.hintEl = document.getElementById('stream-hint');

        document.getElementById('btn-stream-quit').onclick = () => this.quit();
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
                this.setStatus('error', 'Decoder error');
            }
        });
        console.log('[StreamView] VideoDecoder created, state=' + this.decoder.state);
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

        configsToTry.push({
            codec: codec,
            description: desc.buffer,
            codedWidth: 1920,
            codedHeight: 1080,
            optimizeForLatency: true
        });

        for (const fbCodec of fallbacks) {
            if (fbCodec === codec) continue;
            configsToTry.push({
                codec: fbCodec,
                description: desc.buffer,
                codedWidth: 1920,
                codedHeight: 1080,
                optimizeForLatency: true
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
        }
    }

    onDecodedFrame(frame) {
        this.stats.decoded++;

        if (this.stats.decoded <= 3) {
            console.log('[StreamView] onDecodedFrame #' + this.stats.decoded +
                ' displaySize=' + (frame.displayWidth || '?') + 'x' + (frame.displayHeight || '?') +
                ' format=' + frame.format);
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
            this.setStatus('live', 'Live');
            this._firstFrameRendered = true;
        }
    }

    startRenderLoop() {
        if (this.renderRunning) return;
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
    // WebSocket
    // =========================================================================

    setupWebSocket() {
        this.ws.onOpen = () => {
            if (this._quitting) return;
            this.connected = true;
            this.setStatus('connecting', 'Waiting for stream...');
            this.setupDecoder();
        };
        this.ws.onClose = () => {
            if (this._quitting) return;
            this.connected = false;
            this.setStatus('disconnected', 'Disconnected');
            Toast.error('Stream disconnected');
            setTimeout(() => this.quit(), 3000);
        };
        this.ws.onError = () => {
            if (this._quitting) return;
            Toast.error('WebSocket error');
        };
        this.ws.onBinary = (data) => this.handleBinary(data);
        this.ws.onText = (msg) => this.handleWSText(msg);
        this.ws.connect();
    }

    handleWSText(msg) {
        try {
            const obj = JSON.parse(msg);
            if (obj.type === 'debug_hex') {
                console.log('[Backend] First frame payload hex:', obj.payload);
            }
        } catch (e) {
            // Not JSON, ignore
        }
    }

    handleBinary(data) {
        const bytes = new Uint8Array(data);
        const channel = bytes[0];
        const flags = bytes[1];
        const payload = bytes.slice(2);

        if (!this._firstMsgLogged) {
            this._firstMsgLogged = true;
            console.log('[StreamView] First binary message: channel=0x' + channel.toString(16),
                        'flags=0x' + flags.toString(16),
                        'payloadSize=' + payload.length);
        }

        if (channel === 0x01) {
            const isKeyframe = (flags & 0x01) !== 0;
            this.handleVideoFrame(payload, isKeyframe);
        } else if (channel === 0x02) {
            // Audio channel — PCM data, will be handled in Phase 6
            if (!this._audioLogged) {
                console.log('[StreamView] Audio sample received, size=' + payload.length);
                this._audioLogged = true;
            }
        }
    }

    handleVideoFrame(data, isKeyframe) {
        // Stop processing frames once quit() has started.  The WS may still
        // deliver queued messages during the async HTTP /quit call.
        if (this._quitting) return;

        if (data.length < 4) {
            console.warn('[StreamView] Video frame too small:', data.length);
            return;
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

        // --- H.264 / HEVC pipeline (existing) ---

        // Extract SPS/PPS from the first keyframe if not done yet
        if (!this.nalParser.isReady()) {
            if (isKeyframe) {
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
                this.ws.send({ type: 'mousemove', dx, dy });
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
        this.ws.send({
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
        this.ws.send({
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
        this.ws.send({ type: 'mousemove', dx: e.movementX, dy: e.movementY });
    }

    handleMouseDown(e) {
        this.ws.send({ type: 'mousedown', button: e.button + 1 });
    }

    handleMouseUp(e) {
        this.ws.send({ type: 'mouseup', button: e.button + 1 });
    }

    handleWheel(e) {
        e.preventDefault();
        this.ws.send({ type: 'mousewheel', delta: e.deltaY });
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

        // Close any pending frames
        for (const frame of this.frameQueue) {
            try { frame.close(); } catch (e) { /* ignore */ }
        }
        this.frameQueue = [];
        this.pendingFrames = [];

        // Send HTTP /quit BEFORE closing the WebSocket. This avoids a race
        // where the backend's WS disconnect handler nullifies g_ActiveRelay
        // before the HTTP /quit handler can stop the relay.
        try {
            await BackendClient.quitApp(this.host.uuid);
            Toast.success('Stream ended');
        } catch (err) {
            console.warn('[StreamView] Quit failed:', err);
        }

        // Close WS only after HTTP quit has completed
        this.ws.close();
        this.destroy();
    }

    destroy() {
        this.stopRenderLoop();
        this.stopDiagnostics();
        this.unbindEvents();
        this.ws.close();

        if (document.pointerLockElement === this.canvas) {
            document.exitPointerLock();
        }

        if (this.decoder) {
            try { this.decoder.close(); } catch (e) { /* ignore */ }
            this.decoder = null;
        }
        this.decoderConfigured = false;
        this.nalParser.reset();

        for (const frame of this.frameQueue) {
            try { frame.close(); } catch (e) { /* ignore */ }
        }
        this.frameQueue = [];
        this.pendingFrames = [];

        const el = document.getElementById('stream-view');
        if (el) el.remove();
    }
}
