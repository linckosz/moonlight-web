/**
 * WebSocket wrapper for Moonlight streaming.
 * Handles auto-reconnect and binary/text message dispatch.
 */
export class WebSocketClient {
    constructor(url) {
        this.url = url;
        this.ws = null;
        this.connected = false;
        this.reconnectDelay = 2000;
        this.reconnectTimer = null;

        // Callbacks — set by the caller
        this.onOpen = null;
        this.onClose = null;
        this.onError = null;
        this.onText = null;
        this.onBinary = null;
    }

    connect() {
        if (this.ws) this.ws.close();

        console.log('[WS] Connecting to', this.url);
        this.ws = new WebSocket(this.url);
        this.ws.binaryType = 'arraybuffer';

        this.ws.onopen = (evt) => {
            this.connected = true;
            console.log('[WS] Connected');
            this.onOpen && this.onOpen(evt);
        };

        this.ws.onclose = (evt) => {
            this.connected = false;
            console.log('[WS] Disconnected: code=', evt.code, 'reason=', evt.reason,
                        'wasClean=', evt.wasClean, 'readyState=', this.ws ? this.ws.readyState : 'N/A');
            this.onClose && this.onClose(evt);
        };

        this.ws.onerror = (err) => {
            console.error('[WS] Error event — readyState=', this.ws ? this.ws.readyState : 'N/A',
                          'connected=', this.connected, 'error=', err.type);
            this.onError && this.onError(err);
        };

        this.ws.onmessage = (evt) => {
            if (evt.data instanceof ArrayBuffer) {
                this.onBinary && this.onBinary(new Uint8Array(evt.data));
            } else {
                this.onText && this.onText(evt.data);
            }
        };
    }

    send(obj) {
        if (this.ws && this.connected) {
            this.ws.send(JSON.stringify(obj));
        }
    }

    close() {
        if (this.reconnectTimer) {
            clearTimeout(this.reconnectTimer);
            this.reconnectTimer = null;
        }
        if (this.ws) {
            this.ws.onopen = null;
            this.ws.onclose = null;
            this.ws.onerror = null;
            this.ws.onmessage = null;
            this.ws.close();
            this.ws = null;
        }
        this.connected = false;
    }
}
