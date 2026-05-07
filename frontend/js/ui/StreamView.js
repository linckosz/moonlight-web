/**
 * Fullscreen streaming overlay with input capture.
 * Creates a canvas for future video rendering, captures keyboard/mouse
 * events, and relays them to the backend via WebSocket.
 */
import { WebSocketClient } from '../api/WebSocketClient.js';
import { BackendClient } from '../api/BackendClient.js';
import { Toast } from './Toast.js';

export class StreamView {
    constructor(container, wsUrl, host) {
        this.container = container;
        this.wsUrl = wsUrl;
        this.host = host;
        this.ws = new WebSocketClient(wsUrl);
        this.pointerLocked = false;

        // Bound handlers for add/removeEventListener
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
                <canvas id="stream-canvas" class="stream-canvas"></canvas>
                <div class="stream-click-hint" id="stream-hint">
                    Click to capture mouse &amp; keyboard
                </div>
            </div>
        `;
        document.getElementById('app').appendChild(el);

        // Sizing
        this.canvas = document.getElementById('stream-canvas');
        this.statusEl = document.getElementById('stream-status');
        this.hintEl = document.getElementById('stream-hint');
        this._resizeCanvas();
        window.addEventListener('resize', () => this._resizeCanvas());

        document.getElementById('btn-stream-quit').onclick = () => this.quit();
    }

    _resizeCanvas() {
        const area = document.querySelector('.stream-canvas-area');
        if (!area) return;
        const rect = area.getBoundingClientRect();
        this.canvas.width = rect.width * (window.devicePixelRatio || 1);
        this.canvas.height = rect.height * (window.devicePixelRatio || 1);
        this.canvas.style.width = rect.width + 'px';
        this.canvas.style.height = rect.height + 'px';
    }

    setupWebSocket() {
        this.ws.onOpen = () => this.setStatus('live', 'Live');
        this.ws.onClose = () => {
            this.setStatus('disconnected', 'Disconnected');
            Toast.error('Stream disconnected');
            setTimeout(() => this.quit(), 3000);
        };
        this.ws.onError = () => Toast.error('WebSocket error');
        this.ws.onBinary = (data) => this.handleBinary(data);
        this.ws.connect();
    }

    handleBinary(data) {
        // data[0] = channel (0x01=video, 0x02=audio)
        // data[1:] = raw RTP payload
        // Phase 6 will feed video frames to a decoder
    }

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
        this.canvas.removeEventListener('mousemove', this._onMouseMove);
        this.canvas.removeEventListener('mousedown', this._onMouseDown);
        this.canvas.removeEventListener('mouseup', this._onMouseUp);
        this.canvas.removeEventListener('wheel', this._onWheel);
        this.canvas.removeEventListener('contextmenu', this._onContextMenu);
    }

    requestPointerLock() {
        if (!this.pointerLocked) {
            this.canvas.requestPointerLock();
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
        this.pointerLocked = (document.pointerLockElement === this.canvas);
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

        if (document.pointerLockElement === this.canvas) {
            document.exitPointerLock();
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

        if (document.pointerLockElement === this.canvas) {
            document.exitPointerLock();
        }

        const el = document.getElementById('stream-view');
        if (el) el.remove();

        window.removeEventListener('resize', () => this._resizeCanvas());
    }
}
