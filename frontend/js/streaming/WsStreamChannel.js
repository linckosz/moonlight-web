/**
 * WebSocket Stream Channel — legacy WSS transport for Moonlight streaming.
 *
 * Replaces WebRtcDataChannel when the transport mode is "wss" (StreamRelay).
 * Provides the same callback interface (onVideo, onAudio, send, close, etc.)
 * over a plain WebSocket connection.
 *
 * Protocol (StreamRelay backend):
 *   Binary: [channel:1][flags:1][payload:N]
 *     channel=0x01 → video, channel=0x02 → audio
 *     flags bit 0  → isKeyframe (video only)
 *   Text: JSON input commands from browser to server
 */
export class WsStreamChannel {
    constructor(url) {
        this.url = url;
        this.ws = null;
        this.connected = false;

        // Callbacks — set by the caller
        this.onOpen = null;
        this.onClose = null;
        this.onError = null;
        this.onVideo = null;      // (frame: Uint8Array, isKeyframe: boolean)
        this.onAudio = null;      // (sample: Uint8Array)

        // Stats
        this.stats = { framesReceived: 0, audioSamples: 0 };

        // Guard
        this._stopping = false;
        this._logCount = 0;
    }

    connect() {
        if (this._stopping) return;

        console.log('[WsStream] Connecting to:', this.url);
        this.ws = new WebSocket(this.url);

        this.ws.binaryType = 'arraybuffer';

        this.ws.onopen = () => {
            console.log('[WsStream] Connected');
            this.connected = true;
            if (this.onOpen) this.onOpen();
        };

        this.ws.onmessage = (evt) => {
            if (this._stopping) return;

            if (evt.data instanceof ArrayBuffer) {
                this._handleBinary(new Uint8Array(evt.data));
            } else if (typeof evt.data === 'string') {
                // Server-to-client text messages (e.g. debug_hex) — ignore
                if (this._logCount < 3) {
                    console.log('[WsStream] Text message:', evt.data.substring(0, 100));
                    this._logCount++;
                }
            }
        };

        this.ws.onerror = (err) => {
            console.error('[WsStream] Error:', err);
            if (!this._stopping && this.onError) {
                this.onError(new Error('WebSocket error'));
            }
        };

        this.ws.onclose = (evt) => {
            console.log('[WsStream] Closed: code=' + evt.code + ' reason=' + evt.reason);
            this.connected = false;
            if (!this._stopping && this.onClose) this.onClose();
        };
    }

    /** Send JSON input command to the server. */
    send(obj) {
        if (this.ws && this.ws.readyState === WebSocket.OPEN) {
            this.ws.send(JSON.stringify(obj));
        }
    }

    markStopping() {
        this._stopping = true;
    }

    close() {
        if (this._stopping) return;
        this._stopping = true;
        console.log('[WsStream] Closing...');
        if (this.ws) {
            this.ws.onopen = null;
            this.ws.onmessage = null;
            this.ws.onerror = null;
            this.ws.onclose = null;
            if (this.ws.readyState === WebSocket.OPEN ||
                this.ws.readyState === WebSocket.CONNECTING) {
                try { this.ws.close(); } catch (e) { /* ignore */ }
            }
            this.ws = null;
        }
        this.connected = false;
        console.log('[WsStream] Closed');
    }

    // =========================================================================
    // Binary protocol handler
    // =========================================================================
    //
    // StreamRelay binary format:
    //   [channel:1][flags:1][payload:N]
    //
    //   channel 0x01 = video (payload is raw H.264/HEVC Annex B)
    //   channel 0x02 = audio (payload is raw PCM16 stereo interleaved)
    //
    //   flags (video only):
    //     bit 0 = 1 → keyframe (IDR), 0 → delta frame (P-frame)

    _handleBinary(data) {
        if (data.length < 2) {
            console.warn('[WsStream] Binary message too small:', data.length);
            return;
        }

        const channel = data[0];
        const flags = data[1];
        const payload = data.slice(2);

        if (channel === 0x01) {
            // Video frame
            const isKeyframe = (flags & 0x01) !== 0;
            this.stats.framesReceived++;

            if (this.stats.framesReceived <= 3 || this.stats.framesReceived % 60 === 0) {
                console.log('[WsStream] Video frame #' + this.stats.framesReceived +
                    ' size=' + payload.length + ' keyframe=' + isKeyframe);
            }

            if (this.onVideo) {
                this.onVideo(payload, isKeyframe);
            }
        } else if (channel === 0x02) {
            // Audio sample
            this.stats.audioSamples++;

            if (this.stats.audioSamples <= 3) {
                console.log('[WsStream] Audio sample #' + this.stats.audioSamples +
                    ' size=' + payload.length);
            }

            if (this.onAudio) {
                this.onAudio(payload);
            }
        } else {
            if (this._logCount < 3) {
                console.warn('[WsStream] Unknown channel:', channel);
                this._logCount++;
            }
        }
    }
}
