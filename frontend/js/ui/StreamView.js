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
    buildAvccDescription,
    getCodecString,
    toAvcc,
    FALLBACK_CODEC_STRINGS
} from '../util/Mp4Muxer.js';

export class StreamView {
    constructor(container, wsUrl, host) {
        this.container = container;
        this.wsUrl = wsUrl;
        this.host = host;
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

        this.render();
        this.setupWebSocket();
        this.bindEvents();
        this.startRenderLoop();
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
            this.decoder.close();
            this.decoder = null;
        }

        this.decoder = new VideoDecoder({
            output: (frame) => this.onDecodedFrame(frame),
            error: (err) => {
                console.error('[StreamView] VideoDecoder error:', err.message, err);
                this.setStatus('error', 'Decoder error');
            }
        });
    }

    configureDecoder() {
        if (this.decoderConfigured || this.decoderConfiguring || !this.nalParser.isReady()) return;
        this.decoderConfiguring = true;

        const sps = this.nalParser.sps;
        const pps = this.nalParser.pps;

        const avcc = buildAvccDescription(sps, pps);
        if (!avcc) {
            console.warn('[StreamView] Failed to build avcC description');
            this.decoderConfiguring = false;
            return;
        }

        const codec = getCodecString(sps);
        if (!codec) {
            console.error('[StreamView] Could not determine codec string');
            this.decoderConfiguring = false;
            this.setStatus('error', 'Unknown codec');
            return;
        }

        // Log info
        console.log('[StreamView] Configuring VideoDecoder: codec=' + codec,
                    'avccLen=' + avcc.length,
                    'spsLen=' + sps.length,
                    'ppsLen=' + pps.length);
        console.log('[StreamView] SPS hex:', Array.from(sps).map(b => b.toString(16).padStart(2, '0')).join(' '));

        if (!VideoDecoder.isConfigSupported) {
            console.error('[StreamView] WebCodecs VideoDecoder not available');
            this.decoderConfiguring = false;
            this.setStatus('error', 'WebCodecs not supported');
            return;
        }

        const applyConfig = (cfg) => {
            try {
                this.decoder.configure(cfg);
                this.decoderConfigured = true;
                this.decoderConfiguring = false;
                console.log('[StreamView] VideoDecoder configured with codec=' + cfg.codec +
                            ', dequeuing ' + this.pendingFrames.length + ' pending frames');
                this.flushPendingFrames();
                return true;
            } catch (e) {
                console.error('[StreamView] decoder.configure() failed:', e);
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
            VideoDecoder.isConfigSupported(cfg).then((result) => {
                if (result.supported) {
                    console.log('[StreamView] Config supported: codec=' + cfg.codec +
                                ' hasDescription=' + !!cfg.description);
                    // applyConfig may fail (e.g., if decoder state is bad) — fall through
                    if (!applyConfig(cfg)) {
                        tryCodecs(configs, index + 1);
                    }
                } else {
                    console.warn('[StreamView] Config rejected: codec=' + cfg.codec +
                                 ', trying next');
                    tryCodecs(configs, index + 1);
                }
            }).catch((err) => {
                console.error('[StreamView] isConfigSupported error for codec=' +
                              cfg.codec + ':', err);
                tryCodecs(configs, index + 1);
            });
        };

        // Build list of configs to try, in priority order.
        // All configs include the avcC description so the decoder knows
        // the exact SPS/PPS, regardless of the codec string used.
        // Data passed to decode() MUST be in AVCC format (4-byte length prefixes)
        // when description is provided — decodeFrame() converts accordingly.
        const configsToTry = [];

        // 1. Primary: original codec string with avcC description
        configsToTry.push({
            codec: codec,
            description: avcc.buffer,
            codedWidth: 1920,
            codedHeight: 1080,
            optimizeForLatency: true
        });

        // 2. Fallback codec strings WITH description
        for (const fbCodec of FALLBACK_CODEC_STRINGS) {
            if (fbCodec === codec) continue;
            configsToTry.push({
                codec: fbCodec,
                description: avcc.buffer,
                codedWidth: 1920,
                codedHeight: 1080,
                optimizeForLatency: true
            });
        }

        tryCodecs(configsToTry, 0);
    }

    flushPendingFrames() {
        while (this.pendingFrames.length > 0) {
            const entry = this.pendingFrames.shift();
            this.decodeFrame(entry.data, entry.isKeyframe);
        }
    }

    decodeFrame(data, isKeyframe) {
        if (!this.decoderConfigured) {
            // Buffer until decoder is ready (limit to avoid OOM)
            if (this.pendingFrames.length < 120) {
                this.pendingFrames.push({ data, isKeyframe });
            }
            return;
        }

        const timestamp = this.frameCount * 16667; // ~60 fps in microseconds
        this.frameCount++;

        const type = isKeyframe ? 'key' : 'delta';

        // Convert Annex B (start codes) to AVCC (4-byte length prefixes).
        // VideoDecoder expects AVCC data when description (avcC) was provided
        // in configure(). See buildAvccDescription() which sets lengthSizeMinusOne=3.
        // When decoderConfigured is true, strip SPS/PPS since they are already
        // in the avcC description -- including them confuses the decoder's
        // keyframe detection (it expects the first NAL to be an IDR slice).
        const avccData = toAvcc(data, this.decoderConfigured);

        try {
            const chunk = new EncodedVideoChunk({
                type: type,
                timestamp: timestamp,
                duration: 16667,
                data: avccData
            });
            this.decoder.decode(chunk);
            this.stats.received++;
        } catch (err) {
            console.error('[StreamView] decode() error:', err.message);
            this.stats.dropped++;
        }
    }

    onDecodedFrame(frame) {
        this.stats.decoded++;

        // Limit queue depth to 3 to keep latency low
        if (this.frameQueue.length >= 3) {
            const old = this.frameQueue.shift();
            old.close();
            this.stats.dropped++;
        }

        this.frameQueue.push(frame);

        // Update status on first frame
        if (!this._firstFrameRendered) {
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
            this.connected = true;
            this.setStatus('connecting', 'Waiting for stream...');
            this.setupDecoder();
        };
        this.ws.onClose = () => {
            this.connected = false;
            this.setStatus('disconnected', 'Disconnected');
            Toast.error('Stream disconnected');
            setTimeout(() => this.quit(), 3000);
        };
        this.ws.onError = () => Toast.error('WebSocket error');
        this.ws.onBinary = (data) => this.handleBinary(data);
        this.ws.connect();
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
        if (data.length < 4) {
            console.warn('[StreamView] Video frame too small:', data.length);
            return;
        }

        // Log first frame details
        if (!this._firstFrameProcessed) {
            this._firstFrameProcessed = true;
            console.log('[StreamView] First video frame: isKeyframe=' + isKeyframe,
                        'size=' + data.length);
        }

        // Extract SPS/PPS from the first keyframe if not done yet
        if (!this.nalParser.isReady()) {
            if (isKeyframe) {
                const ready = this.nalParser.feed(data);
                if (ready) {
                    console.log('[StreamView] SPS/PPS extracted from first keyframe');
                    this.configureDecoder();
                }
            }
        }

        // Try to configure decoder if SPS/PPS just became available
        if (!this.decoderConfigured && this.nalParser.isReady()) {
            this.configureDecoder();
        }

        // Submit frame to decoder
        this.decodeFrame(data, isKeyframe);
    }

    // =========================================================================
    // Input events
    // =========================================================================

    bindEvents() {
        document.addEventListener('keydown', this._onKeyDown);
        document.addEventListener('keyup', this._onKeyUp);
        document.addEventListener('pointerlockchange', this._onPointerLockChange);
        this.canvas.addEventListener('mousemove', this._onMouseMove);
        this.canvas.addEventListener('mousedown', this._onMouseDown);
        this.canvas.addEventListener('mouseup', this._onMouseUp);
        this.canvas.addEventListener('wheel', this._onWheel, { passive: false });
        this.canvas.addEventListener('contextmenu', this._onContextMenu);
        this.canvas.addEventListener('click', () => this.requestPointerLock());
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
        }
    }

    requestPointerLock() {
        if (!this.pointerLocked && this.canvas) {
            this.canvas.requestPointerLock();
        }
    }

    handleKeyDown(e) {
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
        this.stopRenderLoop();
        this.unbindEvents();
        this.ws.close();

        if (document.pointerLockElement === this.canvas) {
            document.exitPointerLock();
        }

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

        try {
            await BackendClient.quitApp(this.host.uuid);
            Toast.success('Stream ended');
        } catch (err) {
            console.warn('[StreamView] Quit failed:', err);
        }

        this.destroy();
    }

    destroy() {
        this.stopRenderLoop();
        this.unbindEvents();
        this.ws.close();

        if (document.pointerLockElement === this.canvas) {
            document.exitPointerLock();
        }

        if (this.decoder) {
            try { this.decoder.close(); } catch (e) { /* ignore */ }
            this.decoder = null;
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
