/**
 * Fullscreen streaming overlay with input capture and MSE video rendering.
 * Creates a <video> element fed via MediaSource Extensions with H.264 fMP4
 * segments built by Mp4Muxer. Captures keyboard/mouse events and relays them
 * to the backend via WebSocket.
 */
import { WebSocketClient } from '../api/WebSocketClient.js';
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';
import { Mp4Muxer } from '../util/Mp4Muxer.js';

export class StreamView {
    constructor(container, wsUrl, host) {
        this.container = container;
        this.wsUrl = wsUrl;
        this.host = host;
        this.ws = new WebSocketClient(wsUrl);
        this.pointerLocked = false;

        // Video
        this.muxer = new Mp4Muxer(1920, 1080, 60);
        this.mediaSource = null;
        this.sourceBuffer = null;
        this.frameQueue = [];
        this.frameCount = 0;
        this.connected = false;

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
    }

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
                <video id="stream-video" autoplay muted playsinline
                       style="width:100%;height:100%;object-fit:contain;background:#000;"></video>
                <div class="stream-click-hint" id="stream-hint">
                    Click to capture mouse &amp; keyboard
                </div>
            </div>
        `;
        document.getElementById('app').appendChild(el);

        this.video = document.getElementById('stream-video');
        this.statusEl = document.getElementById('stream-status');
        this.hintEl = document.getElementById('stream-hint');

        this.setupMediaSource();

        document.getElementById('btn-stream-quit').onclick = () => this.quit();
    }

    setupMediaSource() {
        const mimeCodec = 'video/mp4; codecs="avc1.64002A"';
        console.log('[StreamView] Checking MSE support for', mimeCodec, ':', MediaSource.isTypeSupported(mimeCodec));

        if (!MediaSource.isTypeSupported(mimeCodec)) {
            console.warn('[StreamView] MSE codec not supported, trying fallback');
            const fallbacks = [
                'video/mp4; codecs="avc1.42E01E"',
                'video/mp4; codecs="avc1.4D401E"',
                'video/mp4; codecs="avc1.640028"',
                'video/mp4; codecs="avc1.42C01E"',
            ];
            let found = false;
            for (const fb of fallbacks) {
                console.log('[StreamView]   trying fallback:', fb, MediaSource.isTypeSupported(fb));
            }
            this.setStatus('error', 'Codec unsupported');
            return;
        }

        console.log('[StreamView] Creating MediaSource...');
        this.mediaSource = new MediaSource();
        this.video.src = URL.createObjectURL(this.mediaSource);
        console.log('[StreamView] MediaSource created, URL:', this.video.src);

        this.mediaSource.addEventListener('sourceopen', () => {
            console.log('[StreamView] MediaSource sourceopen fired, readyState=', this.mediaSource.readyState);
            try {
                this.sourceBuffer = this.mediaSource.addSourceBuffer(mimeCodec);
                this.sourceBuffer.mode = 'segments';
                console.log('[StreamView] SourceBuffer added, mode=', this.sourceBuffer.mode);

                this.sourceBuffer.addEventListener('updateend', () => {
                    this._dequeueFrame();
                });

                this.sourceBuffer.addEventListener('error', (e) => {
                    console.error('[StreamView] SourceBuffer error:', e, this.sourceBuffer);
                });

                this.sourceBuffer.addEventListener('abort', () => {
                    console.warn('[StreamView] SourceBuffer abort');
                });

                this._dequeueFrame();
            } catch (err) {
                console.error('[StreamView] Failed to add source buffer:', err.message);
                this.setStatus('error', 'Media setup failed: ' + err.message);
            }
        });

        this.mediaSource.addEventListener('sourceclose', () => {
            console.log('[StreamView] MediaSource sourceclose');
        });

        this.mediaSource.addEventListener('sourceended', () => {
            console.log('[StreamView] MediaSource sourceended');
        });
    }

    _dequeueFrame() {
        if (!this.sourceBuffer) return;
        if (this.sourceBuffer.updating) return;
        if (this.frameQueue.length === 0) return;

        const entry = this.frameQueue.shift();

        try {
            if (entry.init) {
                console.log('[StreamView] Appending init segment, size=', entry.init.length || entry.init.byteLength);
                this.sourceBuffer.appendBuffer(entry.init);
                return;
            }

            if (entry.media) {
                this.sourceBuffer.appendBuffer(entry.media);
                this.frameCount++;
                if (this.frameCount === 1) {
                    console.log('[StreamView] First media segment appended');
                    this.setStatus('live', 'Live');
                }
                if (this.frameCount % 60 === 0) {
                    console.log('[StreamView] Frames appended:', this.frameCount, 'queue depth:', this.frameQueue.length);
                }
            }
        } catch (err) {
            console.error('[StreamView] appendBuffer error:', err.message);
            setTimeout(() => this._dequeueFrame(), 10);
        }
    }

    setupWebSocket() {
        this.ws.onOpen = () => {
            this.connected = true;
            this.setStatus('connecting', 'Waiting for stream...');
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
        const flags = bytes[1];    // bit0 = isKeyframe (video only)
        const payload = bytes.slice(2);

        if (!this._firstFrameReceived) {
            this._firstFrameReceived = true;
            console.log('[StreamView] First binary message: channel=0x' + channel.toString(16),
                        'flags=0x' + flags.toString(16), 'payloadSize=' + payload.length);
        }

        if (channel === 0x01) {
            const isKeyframe = (flags & 0x01) !== 0;
            this._handleVideoFrame(payload, isKeyframe);
        } else if (channel === 0x02) {
            if (!this._audioLogged) {
                console.log('[StreamView] Audio sample received, size=', payload.length);
                this._audioLogged = true;
            }
        }
    }

    _handleVideoFrame(data, isKeyframe) {
        if (data.length < 4) {
            console.warn('[StreamView] Video frame too small:', data.length);
            return;
        }

        if (!this._firstFrameProcessed) {
            this._firstFrameProcessed = true;
            console.log('[StreamView] First video frame: isKeyframe=', isKeyframe, 'size=', data.length);
            // Log first 40 bytes in hex
            const hex = Array.from(data.slice(0, 40))
                .map(b => b.toString(16).padStart(2, '0')).join(' ');
            console.log('[StreamView] First 40 bytes:', hex);
        }

        try {
            const result = this.muxer.processFrame(data, isKeyframe);

            if (result.init) {
                console.log('[StreamView] Got init segment, size=', result.init.length);
                this.frameQueue.push({ init: result.init, media: null });
            }
            if (result.media) {
                this.frameQueue.push({ init: null, media: result.media });
            }

            while (this.frameQueue.length > 60) {
                this.frameQueue.shift();
            }

            this._dequeueFrame();
        } catch (err) {
            console.error('[StreamView] Frame processing error:', err.message, err.stack);
        }
    }

    bindEvents() {
        document.addEventListener('keydown', this._onKeyDown);
        document.addEventListener('keyup', this._onKeyUp);
        document.addEventListener('pointerlockchange', this._onPointerLockChange);
        this.video.addEventListener('mousemove', this._onMouseMove);
        this.video.addEventListener('mousedown', this._onMouseDown);
        this.video.addEventListener('mouseup', this._onMouseUp);
        this.video.addEventListener('wheel', this._onWheel, { passive: false });
        this.video.addEventListener('contextmenu', this._onContextMenu);
        this.video.addEventListener('click', () => this.requestPointerLock());
    }

    unbindEvents() {
        document.removeEventListener('keydown', this._onKeyDown);
        document.removeEventListener('keyup', this._onKeyUp);
        document.removeEventListener('pointerlockchange', this._onPointerLockChange);
        this.video.removeEventListener('mousemove', this._onMouseMove);
        this.video.removeEventListener('mousedown', this._onMouseDown);
        this.video.removeEventListener('mouseup', this._onMouseUp);
        this.video.removeEventListener('wheel', this._onWheel);
        this.video.removeEventListener('contextmenu', this._onContextMenu);
    }

    requestPointerLock() {
        if (!this.pointerLocked) {
            this.video.requestPointerLock();
        }
    }

    // --- Input handlers ---

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
        this.pointerLocked = (document.pointerLockElement === this.video);
        if (this.pointerLocked) {
            this.hintEl.style.display = 'none';
        } else {
            this.hintEl.style.display = 'flex';
        }
    }

    // --- Status ---

    setStatus(state, text) {
        const dot = this.statusEl.querySelector('.stream-status-dot');
        dot.className = 'stream-status-dot status-' + state;
        this.statusEl.childNodes[1].textContent = ' ' + text;
    }

    // --- Quit ---

    async quit() {
        this.unbindEvents();
        this.ws.close();

        if (document.pointerLockElement === this.video) {
            document.exitPointerLock();
        }

        // Close MediaSource
        if (this.mediaSource && this.mediaSource.readyState === 'open') {
            try {
                this.mediaSource.endOfStream();
            } catch (e) { /* ignore */ }
        }
        if (this.video.src) {
            URL.revokeObjectURL(this.video.src);
        }

        try {
            await BackendClient.quitApp(this.host.uuid);
            Toast.success('Stream ended');
        } catch (err) {
            console.warn('[StreamView] Quit failed:', err);
        }

        this.destroy();
    }

    destroy() {
        this.unbindEvents();
        this.ws.close();

        if (document.pointerLockElement === this.video) {
            document.exitPointerLock();
        }

        if (this.mediaSource && this.mediaSource.readyState === 'open') {
            try { this.mediaSource.endOfStream(); } catch (e) { /* ignore */ }
        }
        if (this.video && this.video.src) {
            URL.revokeObjectURL(this.video.src);
        }

        const el = document.getElementById('stream-view');
        if (el) el.remove();
    }
}
