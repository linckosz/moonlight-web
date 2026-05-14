/**
 * WebRTC DataChannel wrapper for Moonlight streaming.
 *
 * Replaces the legacy WebSocket binary transport.  Uses the browser's native
 * RTCPeerConnection API to establish 3 DataChannels (video, audio, input)
 * with the backend's libdatachannel PeerConnection.
 *
 * Signaling flow (via a temporary WebSocket):
 *   1. Connect to signaling WS (URL given by backend /start response).
 *   2. Receive SDP offer from backend via signaling WS.
 *   3. Create RTCPeerConnection, set remote description, generate answer.
 *   4. Send SDP answer back via signaling WS.
 *   5. Exchange ICE candidates bidirectionally.
 *   6. Once DataChannels open, close signaling WS, start data transfer.
 *
 * Fragmentation protocol (backend sends video/audio in chunks):
 *   Header: [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4]
 *   Max payload per chunk: 14000 bytes (under SCTP 16KB limit).
 *
 * Reassembly:
 *   - Buffers chunks per frame_id in a Map.
 *   - Completes frame when all chunks received (total_chunks == chunk_index + 1).
 *   - I-frames (keyframes): all chunks required, otherwise DROP.
 *   - P-frames (delta): missing chunks filled with 0x00.
 *   - Frames older than 500ms are dropped (cleanup timer).
 */
export class WebRtcDataChannel {
    /**
     * @param {string} signalingUrl - URL of the backend's SignalingServer WS.
     * @param {object} [options]
     * @param {object[]} [options.iceServers] - Optional ICE servers (STUN/TURN).
     */
    constructor(signalingUrl, options = {}) {
        this.signalingUrl = signalingUrl;
        this.signalingWs = null;
        this.pc = null;
        this.dataChannels = { video: null, audio: null, input: null };
        this.connected = false;

        // ICE config
        this.iceServers = options.iceServers || [
            { urls: 'stun:stun.l.google.com:19302' }
        ];

        // Callbacks — set by the caller
        this.onOpen = null;       // All DataChannels open
        this.onClose = null;      // Disconnected / error
        this.onError = null;      // Error event
        this.onVideo = null;      // (frame: Uint8Array, isKeyframe: boolean)
        this.onAudio = null;      // (sample: Uint8Array)

        // Stats
        this.stats = { framesReceived: 0, chunksReceived: 0, framesDropped: 0, framesAssembled: 0 };

        // Reassembly buffers: Map<frame_id, { chunks: Uint8Array[], total: number, keyframe: boolean, firstChunkTime: number }>
        this._reassembly = new Map();
        this._cleanupTimer = null;

        // Logging
        this._logCount = 0;
        this._frameLogCount = 0;

        // Guard
        this._stopping = false;

        // ICE state tracking — used to distinguish premature WS close
        // (before ICE) from graceful WS close after ICE connected but
        // before DataChannels open (which can recover).
        this._iceConnected = false;

        // Grace period timer: when the signaling WS closes after ICE is
        // connected but DataChannels haven't opened yet, we wait this long
        // for SCTP to complete before treating it as an error.
        this._wsCloseTimer = null;
        this.WS_GRACE_PERIOD_MS = 10000;  // 10 seconds

        // Constants (must match backend)
        this.FRAG_HEADER_SIZE = 13;
        this.CLEANUP_INTERVAL_MS = 500;
        this.FRAME_TIMEOUT_MS = 500;

        // Channel labels (must match backend)
        this.DC_VIDEO_LABEL = 'video';
        this.DC_AUDIO_LABEL = 'audio';
        this.DC_INPUT_LABEL = 'input';
    }

    /**
     * Start the WebRTC connection: connect signaling WS, create PeerConnection,
     * wait for offer from backend, answer, exchange ICE.
     */
    connect() {
        if (this._stopping) return;

        console.log('[WebRTC] Connecting to signaling:', this.signalingUrl);
        this.signalingWs = new WebSocket(this.signalingUrl);

        this.signalingWs.onopen = () => {
            console.log('[WebRTC] Signaling WS connected, waiting for SDP offer...');
            this._createPeerConnection();
        };

        this.signalingWs.onmessage = (evt) => {
            if (this._stopping) return;
            try {
                const msg = JSON.parse(evt.data);
                this._handleSignalingMessage(msg);
            } catch (e) {
                console.warn('[WebRTC] Invalid signaling message:', e.message);
            }
        };

        this.signalingWs.onerror = (err) => {
            console.error('[WebRTC] Signaling WS error:', err);
            if (!this._stopping) {
                this._onError('Signaling WebSocket error');
            }
        };

        this.signalingWs.onclose = (evt) => {
            console.log('[WebRTC] Signaling WS closed: code=' + evt.code, 'reason=' + evt.reason);

            // If already connected (DCs open) or stopping, WS close is expected
            if (this.connected || this._stopping) return;

            if (this._iceConnected) {
                // ICE was connected, so the WebRTC transport is established.
                // The WS may have been closed by the backend (e.g. transient
                // PC state glitch on first launch) even though SCTP/DataChannels
                // are still coming up.  Give them a grace period to open before
                // treating the WS close as a fatal error.
                console.log('[WebRTC] WS closed after ICE connected — waiting ' +
                    (this.WS_GRACE_PERIOD_MS / 1000) + 's for DataChannels...');
                this._wsCloseTimer = setTimeout(() => {
                    if (!this.connected && !this._stopping) {
                        this._onError('Timed out waiting for DataChannels after WS closed');
                    }
                }, this.WS_GRACE_PERIOD_MS);
            } else {
                // WS closed before ICE even connected — real failure
                this._onError('Signaling closed before WebRTC connection established');
            }
        };

        // Start cleanup timer
        this._cleanupTimer = setInterval(() => this._cleanupStaleFrames(), this.CLEANUP_INTERVAL_MS);
    }

    /** Send JSON input message over the input DataChannel. */
    send(obj) {
        if (this.dataChannels.input && this.dataChannels.input.readyState === 'open') {
            this.dataChannels.input.send(JSON.stringify(obj));
        } else {
            // Input DataChannel not ready yet — silently drop
            if (this._logCount < 5) {
                console.warn('[WebRTC] Input DC not open, dropping message:', obj.type);
                this._logCount++;
            }
        }
    }

    /** Cancel any scheduled WS close timer. */
    _clearWsCloseTimer() {
        if (this._wsCloseTimer) {
            clearTimeout(this._wsCloseTimer);
            this._wsCloseTimer = null;
        }
    }

    /** Close all connections and clean up. */
    close() {
        if (this._stopping) return;
        this._stopping = true;
        console.log('[WebRTC] Closing...');

        // Clear any pending WS close grace timer
        this._clearWsCloseTimer();

        // Stop cleanup timer
        if (this._cleanupTimer) {
            clearInterval(this._cleanupTimer);
            this._cleanupTimer = null;
        }

        // Clear reassembly buffers
        this._reassembly.clear();

        // Close DataChannels
        for (const [label, dc] of Object.entries(this.dataChannels)) {
            if (dc && dc.readyState !== 'closed') {
                try { dc.close(); } catch (e) { /* ignore */ }
            }
            this.dataChannels[label] = null;
        }

        // Close PeerConnection
        if (this.pc) {
            try { this.pc.close(); } catch (e) { /* ignore */ }
            this.pc = null;
        }

        // Close signaling WS
        if (this.signalingWs) {
            this.signalingWs.onopen = null;
            this.signalingWs.onmessage = null;
            this.signalingWs.onerror = null;
            this.signalingWs.onclose = null;
            if (this.signalingWs.readyState === WebSocket.OPEN ||
                this.signalingWs.readyState === WebSocket.CONNECTING) {
                try { this.signalingWs.close(); } catch (e) { /* ignore */ }
            }
            this.signalingWs = null;
        }

        this.connected = false;
        console.log('[WebRTC] Closed');
    }

    // =========================================================================
    // PeerConnection setup
    // =========================================================================

    _createPeerConnection() {
        console.log('[WebRTC] Creating RTCPeerConnection');

        const config = {
            iceServers: this.iceServers,
            iceTransportPolicy: 'all'
        };

        this.pc = new RTCPeerConnection(config);

        // --- ICE candidate handler ---
        this.pc.onicecandidate = (evt) => {
            if (evt.candidate && this.signalingWs &&
                this.signalingWs.readyState === WebSocket.OPEN) {
                this._sendSignaling({
                    type: 'ice',
                    candidate: evt.candidate.candidate,
                    mid: evt.candidate.sdpMid || '0'
                });
            }
        };

        // --- ICE state ---
        this.pc.oniceconnectionstatechange = () => {
            const state = this.pc.iceConnectionState;
            console.log('[WebRTC] ICE state:', state);
            if (state === 'connected' || state === 'completed') {
                this._iceConnected = true;
                // DataChannels should open shortly via SCTP
            } else if (state === 'disconnected' || state === 'failed') {
                if (!this._stopping) {
                    // If we were waiting for DCs after WS close, the ICE
                    // disconnect is definitive — stop waiting and error.
                    this._clearWsCloseTimer();
                    this._onError('ICE ' + state);
                }
            }
        };

        // --- DataChannel handler (browser receives offers from backend) ---
        // In our negotiated scheme, the backend creates the DCs with negotiated=true
        // and fixed IDs. The browser must create matching DCs with the same IDs.
        // The browser's DCs are "outgoing" but use the same negotiated ID.
        this._createDataChannels();

        // Wait for the SDP offer from the backend (via signaling WS).
        // When we receive it, we'll call _handleSdpOffer().
        console.log('[WebRTC] PC created, waiting for SDP offer...');
    }

    _createDataChannels() {
        // Video DataChannel (ID=0, ordered=false, maxRetransmits=1)
        const videoInit = {
            negotiated: true,
            id: 0,
            ordered: false,
            maxRetransmits: 1
        };
        this.dataChannels.video = this.pc.createDataChannel('video', videoInit);
        this._setupDataChannel('video', this.dataChannels.video);

        // Audio DataChannel (ID=1, ordered=true, reliable)
        const audioInit = {
            negotiated: true,
            id: 1,
            ordered: true
        };
        this.dataChannels.audio = this.pc.createDataChannel('audio', audioInit);
        this._setupDataChannel('audio', this.dataChannels.audio);

        // Input DataChannel (ID=2, ordered=true, reliable)
        const inputInit = {
            negotiated: true,
            id: 2,
            ordered: true
        };
        this.dataChannels.input = this.pc.createDataChannel('input', inputInit);
        this._setupDataChannel('input', this.dataChannels.input);

        console.log('[WebRTC] DataChannels created (video=0, audio=1, input=2)');
    }

    _setupDataChannel(label, dc) {
        // Ensure binary data arrives as ArrayBuffer, not Blob.
        // Chrome defaults to Blob, which _onVideoChunk / _onAudioChunk
        // do not handle (they expect ArrayBuffer or typed arrays).
        dc.binaryType = 'arraybuffer';

        dc.onopen = () => {
            console.log('[WebRTC] DC "' + label + '" open');

            // All 3 channels open together (they share the same SCTP association)
            // Check if all are open
            if (this._allDcOpen()) {
                console.log('[WebRTC] All DataChannels open!');
                this.connected = true;

                // Clear any pending WS close grace timer — DCs are up
                this._clearWsCloseTimer();

                if (this.onOpen) this.onOpen();

                // Close signaling WS — no longer needed
                this._closeSignalingWs();
            }
        };

        dc.onclose = () => {
            console.log('[WebRTC] DC "' + label + '" closed');
            if (!this._stopping && this.connected) {
                this._onError('DataChannel "' + label + '" closed unexpectedly');
            }
        };

        dc.onerror = (err) => {
            // RTCErrorEvent with "User-Initiated Abort" or "sctp-failure"
            // during remote close is normal — not a real error.
            if (!this._stopping) {
                console.warn('[WebRTC] DC "' + label + '" error:', err);
            }
        };

        dc.onmessage = (evt) => {
            if (this._stopping) return;

            if (label === 'video') {
                this._onVideoChunk(evt.data);
            } else if (label === 'audio') {
                this._onAudioChunk(evt.data);
            } else if (label === 'input') {
                // Server-to-client input messages (rumble, etc.) — not used yet
                console.log('[WebRTC] Input DC message:', evt.data);
            }
        };
    }

    _allDcOpen() {
        return this.dataChannels.video &&
            this.dataChannels.video.readyState === 'open' &&
            this.dataChannels.audio &&
            this.dataChannels.audio.readyState === 'open' &&
            this.dataChannels.input &&
            this.dataChannels.input.readyState === 'open';
    }

    // =========================================================================
    // Signaling handlers
    // =========================================================================

    _handleSignalingMessage(msg) {
        if (msg.type === 'sdp') {
            this._handleSdpOffer(msg.sdp);
        } else if (msg.type === 'ice') {
            this._handleIceCandidate(msg.candidate, msg.mid);
        } else {
            console.warn('[WebRTC] Unknown signaling message type:', msg.type);
        }
    }

    async _handleSdpOffer(sdp) {
        console.log('[WebRTC] Received SDP offer, length=' + sdp.length);

        try {
            // Show first 200 chars of SDP for debugging
            console.log('[WebRTC] SDP offer (first 200):', sdp.substring(0, 200));

            const remoteDesc = new RTCSessionDescription({
                type: 'offer',
                sdp: sdp
            });
            await this.pc.setRemoteDescription(remoteDesc);
            console.log('[WebRTC] Remote description set (offer)');

            // Create answer
            const answer = await this.pc.createAnswer();
            await this.pc.setLocalDescription(answer);
            console.log('[WebRTC] Local description set (answer), sending...');

            // Send answer via signaling WS
            this._sendSignaling({
                type: 'sdp',
                sdp: this.pc.localDescription.sdp
            });
        } catch (e) {
            console.error('[WebRTC] SDP handling error:', e.message);
            this._onError('SDP negotiation failed: ' + e.message);
        }
    }

    async _handleIceCandidate(candidate, mid) {
        if (this._stopping) return;

        try {
            const iceCandidate = new RTCIceCandidate({
                candidate: candidate,
                sdpMid: mid
            });
            await this.pc.addIceCandidate(iceCandidate);
            if (this._logCount < 5) {
                console.log('[WebRTC] Added ICE candidate, mid=' + mid);
                this._logCount++;
            }
        } catch (e) {
            console.warn('[WebRTC] Failed to add ICE candidate:', e.message);
        }
    }

    _sendSignaling(obj) {
        if (this.signalingWs && this.signalingWs.readyState === WebSocket.OPEN) {
            this.signalingWs.send(JSON.stringify(obj));
        }
    }

    _closeSignalingWs() {
        if (this.signalingWs) {
            console.log('[WebRTC] Closing signaling WS (DataChannels established)');
            this.signalingWs.onopen = null;
            this.signalingWs.onmessage = null;
            this.signalingWs.onerror = null;
            this.signalingWs.onclose = null;
            if (this.signalingWs.readyState === WebSocket.OPEN ||
                this.signalingWs.readyState === WebSocket.CONNECTING) {
                try { this.signalingWs.close(); } catch (e) { /* ignore */ }
            }
            this.signalingWs = null;
        }
    }

    // =========================================================================
    // Chunk reassembly
    // =========================================================================

    _onVideoChunk(data) {
        this.stats.chunksReceived++;

        if (data instanceof ArrayBuffer) {
            data = new Uint8Array(data);
        } else if (data instanceof Blob) {
            // Blob not expected (binaryType = 'arraybuffer'), but handle gracefully
            console.warn('[WebRTC] Unexpected Blob data, skipping');
            return;
        }

        if (data.byteLength < this.FRAG_HEADER_SIZE) {
            console.warn('[WebRTC] Chunk too small:', data.byteLength);
            return;
        }

        // Parse header (big endian)
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        const frameId = view.getUint32(0, false);       // offset 0, big endian
        const chunkIndex = view.getUint16(4, false);    // offset 4, big endian
        const totalChunks = view.getUint16(6, false);   // offset 6, big endian
        const isKeyframe = view.getUint8(8) !== 0;      // offset 8
        const payloadSize = view.getUint32(9, false);   // offset 9, big endian
        const payload = new Uint8Array(data.buffer, data.byteOffset + this.FRAG_HEADER_SIZE, payloadSize);

        // Check if we already have an entry for this frame
        let entry = this._reassembly.get(frameId);
        if (!entry) {
            entry = {
                chunks: [],
                total: totalChunks,
                received: 0,
                keyframe: isKeyframe,
                firstChunkTime: performance.now(),
                completed: false
            };
            // Pre-allocate array
            for (let i = 0; i < totalChunks; i++) entry.chunks[i] = null;
            this._reassembly.set(frameId, entry);
        }

        // Store chunk
        if (chunkIndex < totalChunks && !entry.chunks[chunkIndex]) {
            entry.chunks[chunkIndex] = payload;
            entry.received++;
        }

        // Check if complete
        if (entry.received >= totalChunks && !entry.completed) {
            entry.completed = true;
            this._assembleFrame(frameId, entry);
            this._reassembly.delete(frameId);
        }
    }

    _assembleFrame(frameId, entry) {
        // Check if all chunks received (for keyframe, require ALL)
        if (entry.keyframe && entry.received < entry.total) {
            // Keyframe missing chunks — drop it
            this.stats.framesDropped++;
            if (this._frameLogCount < 3) {
                console.warn('[WebRTC] Dropping incomplete keyframe #' + frameId +
                    ': got ' + entry.received + '/' + entry.total + ' chunks');
                this._frameLogCount++;
            }
            return;
        }

        // For delta frames, fill missing chunks with zeros
        let totalSize = 0;
        for (let i = 0; i < entry.total; i++) {
            if (entry.chunks[i]) {
                totalSize += entry.chunks[i].length;
            }
        }

        const assembled = new Uint8Array(totalSize);
        let offset = 0;
        for (let i = 0; i < entry.total; i++) {
            if (entry.chunks[i]) {
                assembled.set(entry.chunks[i], offset);
                offset += entry.chunks[i].length;
            } else {
                // Missing chunk in delta frame — fill with zeros
                // This may corrupt the frame, but allows decoding to continue
                if (this._frameLogCount < 3) {
                    console.warn('[WebRTC] Filling missing chunk #' + i + '/' + entry.total +
                        ' for delta frame #' + frameId + ' with zeros');
                    this._frameLogCount++;
                }
            }
        }

        this.stats.framesAssembled++;
        this.stats.framesReceived++;

        if (this.stats.framesAssembled <= 3 || this.stats.framesAssembled % 60 === 0) {
            console.log('[WebRTC] Assembled frame #' + frameId +
                ' size=' + assembled.length +
                ' keyframe=' + entry.keyframe +
                ' chunks=' + entry.received + '/' + entry.total +
                ' totalFrames=' + this.stats.framesAssembled);
        }

        // Emit video frame
        if (this.onVideo) {
            this.onVideo(assembled, entry.keyframe);
        }
    }

    _onAudioChunk(data) {
        this.stats.chunksReceived++;

        if (data instanceof ArrayBuffer) {
            data = new Uint8Array(data);
        } else if (data instanceof Blob) {
            return;
        }

        if (data.byteLength < this.FRAG_HEADER_SIZE) return;

        // Parse header (same format, but isKeyframe is always 0 for audio)
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        const payloadSize = view.getUint32(9, false);
        const payload = new Uint8Array(data.buffer, data.byteOffset + this.FRAG_HEADER_SIZE, payloadSize);

        // Audio packets are typically single-chunk, so we emit directly
        if (this.onAudio) {
            this.onAudio(payload);
        }
    }

    // =========================================================================
    // Cleanup
    // =========================================================================

    _cleanupStaleFrames() {
        const now = performance.now();
        const toDelete = [];

        for (const [frameId, entry] of this._reassembly) {
            if (entry.completed) {
                toDelete.push(frameId);
                continue;
            }

            const age = now - entry.firstChunkTime;
            if (age > this.FRAME_TIMEOUT_MS) {
                // Frame timed out — drop it
                this.stats.framesDropped++;
                if (this._frameLogCount < 5) {
                    console.warn('[WebRTC] Dropping stale frame #' + frameId +
                        ': age=' + Math.round(age) + 'ms, got ' + entry.received + '/' + entry.total);
                    this._frameLogCount++;
                }

                // For keyframes that timeout, notify the app so it can request a new keyframe
                if (entry.keyframe && this._pendingKeyframeTimeout) {
                    // No mechanism for keyframe request in current protocol
                }

                toDelete.push(frameId);
            }
        }

        for (const id of toDelete) {
            this._reassembly.delete(id);
        }
    }

    // =========================================================================
    // Error handling
    // =========================================================================

    _onError(message) {
        console.error('[WebRTC] Error:', message);
        if (this.onError) this.onError(new Error(message));
        if (this.onClose) this.onClose();
        this.close();
    }
}
