/**
 * Describe a WebSocket close code for diagnostic logging.
 * Returns a human-readable English string suitable for console output.
 */
function wsCloseDescription(code) {
    switch (code) {
        case 1000: return 'normal closure';
        case 1001: return 'endpoint going away';
        case 1002: return 'protocol error';
        case 1003: return 'unsupported data';
        case 1005: return 'no status code (normal)';
        case 1006: return 'abnormal closure — DNS / TLS / network timeout';
        case 1007: return 'invalid frame payload data';
        case 1008: return 'policy violation';
        case 1009: return 'message too big';
        case 1010: return 'mandatory extension';
        case 1011: return 'internal server error';
        case 1015: return 'TLS handshake failure — check system date/time';
        default:   return 'code=' + code;
    }
}

/**
 * WebRTC Media Track wrapper for Moonlight streaming.
 *
 * Alternative to WebRtcDataChannel that uses RTP media tracks for video
 * instead of DataChannels.  The browser's native RTCPeerConnection
 * handles the H.264 RTP stream, decoding it directly to a <video> element.
 *
 * Audio and input still use DataChannels (PCM16 audio, JSON input).
 *
 * Signaling flow (same as WebRtcDataChannel):
 *   1. Connect to signaling WS (URL given by backend /start response).
 *   2. Receive SDP offer from backend (contains m=video + m=application).
 *   3. Create RTCPeerConnection, set remote description, generate answer.
 *   4. Send SDP answer back via signaling WS.
 *   5. Exchange ICE candidates bidirectionally.
 *   6. Once DataChannels open + video track active, close signaling WS.
 *
 * DataChannel layout (must match MediaTrackRelay backend):
 *   - Audio DC: negotiated=true, id=0, label="audio" (PCM16, same fragmentation as WebRtcDataChannel)
 *   - Input DC: negotiated=true, id=1, label="input" (JSON, same as WebRtcDataChannel)
 *
 * Video: arrives via pc.ontrack (H.264 RTP), rendered to <video> element.
 */
export class WebRtcMedia {
    /**
     * @param {string} signalingUrl - URL of the backend's SignalingServer WS.
     * @param {object} [options]
     * @param {object[]} [options.iceServers] - Optional ICE servers (STUN/TURN).
     * @param {HTMLVideoElement} [options.videoElement] - <video> element for rendering.
     */
    constructor(signalingUrl, options = {}) {
        this.signalingUrl = signalingUrl;
        this.signalingWs = null;
        this.pc = null;
        this.dataChannels = { audio: null, input: null };
        this.connected = false;

        // ICE config — populated dynamically by backend ice-config message.
        // Fallback: Google public STUN if no message received before PC creation.
        this._dynamicIceServers = null;
        this._defaultIceServers = [
            { urls: 'stun:stun.l.google.com:19302' },
            { urls: 'stun:stun.cloudflare.com:3478' },
            { urls: 'stun:stun.nextcloud.com:443' },
            { urls: 'stun:relay.metered.ca:80' }
        ];

        // Video element for native decoding of RTP media track
        this.videoElement = options.videoElement || null;

        // Callbacks — set by the caller
        this.onOpen = null;       // All channels/tracks open
        this.onClose = null;      // Disconnected / error
        this.onError = null;      // Error event
        this.onAudio = null;      // (sample: Uint8Array) PCM16 audio
        this.onStats = null;      // (msg: object) stats/pong messages from backend

        // Stats
        this.stats = { framesReceived: 0, chunksReceived: 0, framesDropped: 0, framesAssembled: 0 };

        // Audio reassembly buffers (same format as WebRtcDataChannel)
        this._reassembly = new Map();
        this._cleanupTimer = null;

        // Logging
        this._logCount = 0;
        this._frameLogCount = 0;

        // Guard
        this._stopping = false;

        // WS open/error tracking for better error diagnostics
        this._wsHadOpen = false;
        this._wsHadError = false;

        // ICE state tracking
        this._iceConnected = false;

        // Grace period for WS close after ICE connected
        this._wsCloseTimer = null;
        this.WS_GRACE_PERIOD_MS = 10000;

        // Constants (must match backend)
        this.FRAG_HEADER_SIZE = 17;
        this.CLEANUP_INTERVAL_MS = 500;
        this.FRAME_TIMEOUT_MS = 500;

        // DataChannel labels (must match backend MediaTrackRelay)
        this.DC_AUDIO_LABEL = 'audio';
        this.DC_INPUT_LABEL = 'input';
    }

    /**
     * Set or replace the <video> element for native RTP decoding.
     * Safe to call before or after connect().
     */
    setVideoElement(videoEl) {
        this.videoElement = videoEl;
    }

    /**
     * Start the WebRTC connection: connect signaling WS, create PeerConnection,
     * wait for offer from backend, answer, exchange ICE.
     */
    connect() {
        if (this._stopping) return;

        console.log('[WebRtcMedia] Connecting to signaling:', this.signalingUrl);
        this.signalingWs = new WebSocket(this.signalingUrl);

        this.signalingWs.onopen = () => {
            this._wsHadOpen = true;
            console.log('[WebRtcMedia] Signaling WS connected, waiting for SDP offer...');
            this._createPeerConnection();
        };

        this.signalingWs.onmessage = (evt) => {
            if (this._stopping) return;
            try {
                const msg = JSON.parse(evt.data);
                this._handleSignalingMessage(msg);
            } catch (e) {
                console.warn('[WebRtcMedia] Invalid signaling message:', e.message);
            }
        };

        this.signalingWs.onerror = (err) => {
            console.error('[WebRtcMedia] Signaling WS onerror' +
                (this._wsHadOpen ? ' (after open)' : ' (BEFORE open)'));
            this._wsHadError = true;
            if (!this._stopping) {
                // onerror fires before onclose but carries no close code.
                // Let onclose handle the user-facing error (it has the code).
                // Only trigger now if we were already connected (runtime error).
                if (this._wsHadOpen) {
                    this._onError("Erreur de connexion au serveur de streaming");
                }
            }
        };

        this.signalingWs.onclose = (evt) => {
            const desc = wsCloseDescription(evt.code);
            console.log('[WebRtcMedia] Signaling WS closed: ' + desc + ' (code=' + evt.code + ')' +
                (evt.reason ? ', reason=' + evt.reason : ''));

            if (this.connected || this._stopping) return;

            if (this._iceConnected) {
                console.log('[WebRtcMedia] WS closed after ICE connected — waiting ' +
                    (this.WS_GRACE_PERIOD_MS / 1000) + 's for DataChannels...');
                this._wsCloseTimer = setTimeout(() => {
                    if (!this.connected && !this._stopping) {
                        this._onError('DataChannels not established after ' +
                            (this.WS_GRACE_PERIOD_MS / 1000) + 's');
                    }
                }, this.WS_GRACE_PERIOD_MS);
            } else {
                // Close code tells us what went wrong
                if (evt.code === 1015) {
                    this._onError("Erreur de securite TLS. " +
                        "Verifiez la date et l'heure du systeme.");
                } else if (evt.code === 1006 || !this._wsHadOpen) {
                    this._onError("Connexion au serveur de streaming impossible. " +
                        "Verifiez votre pare-feu, antivirus (HTTPS Scanning), " +
                        "ou proxy.");
                } else {
                    this._onError("Connexion au serveur de streaming interrompue");
                }
            }
        };

        // Start cleanup timer for stale audio frames
        this._cleanupTimer = setInterval(() => this._cleanupStaleFrames(), this.CLEANUP_INTERVAL_MS);
    }

    /** Send JSON input message over the input DataChannel. */
    send(obj) {
        if (this.dataChannels.input && this.dataChannels.input.readyState === 'open') {
            this.dataChannels.input.send(JSON.stringify(obj));
        } else {
            if (this._logCount < 5) {
                console.warn('[WebRtcMedia] Input DC not open, dropping message:', obj.type);
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

    /** Mark as stopping without closing — suppresses "closed unexpectedly" noise. */
    markStopping() {
        this._stopping = true;
    }

    /** Close all connections and clean up. */
    close() {
        if (this._stopping) return;
        this._stopping = true;
        console.log('[WebRtcMedia] Closing...');

        this._clearWsCloseTimer();

        // Stop cleanup timer
        if (this._cleanupTimer) {
            clearInterval(this._cleanupTimer);
            this._cleanupTimer = null;
        }

        // Clear reassembly buffers
        this._reassembly.clear();

        // Release video element
        if (this.videoElement) {
            this.videoElement.srcObject = null;
        }

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
        console.log('[WebRtcMedia] Closed');
    }

    // =========================================================================
    // PeerConnection setup
    // =========================================================================

    _createPeerConnection() {
        console.log('[WebRtcMedia] Creating RTCPeerConnection');

        // Use dynamically-received ICE servers from backend, or fall back
        // to the default Google public STUN server.
        const iceServers = this._dynamicIceServers || this._defaultIceServers;
        const config = {
            iceServers: iceServers,
            iceTransportPolicy: 'all',
            bundlePolicy: 'max-bundle',
            rtcpMuxPolicy: 'require'
        };
        console.log('[WebRtcMedia] ICE servers:', JSON.stringify(iceServers));

        this.pc = new RTCPeerConnection(config);

        // --- ICE candidate handler (filter TURN, prioritize UDP) ---
        this.pc.onicecandidate = (evt) => {
            if (!evt.candidate || !this.signalingWs ||
                this.signalingWs.readyState !== WebSocket.OPEN) {
                return;
            }

            // Drop TURN relay candidates — direct connection only (no TURN server)
            if (evt.candidate.candidate.indexOf(' typ relay ') !== -1) {
                console.log('[WebRtcMedia] Dropping TURN relay candidate');
                return;
            }

            this._sendSignaling({
                type: 'ice',
                candidate: evt.candidate.candidate,
                mid: evt.candidate.sdpMid || '0'
            });
        };

        // --- ICE state ---
        this.pc.oniceconnectionstatechange = () => {
            const state = this.pc.iceConnectionState;
            console.log('[WebRtcMedia] ICE state:', state);
            if (state === 'connected' || state === 'completed') {
                this._iceConnected = true;
            } else if (state === 'disconnected' || state === 'failed') {
                if (!this._stopping) {
                    this._clearWsCloseTimer();
                    this._onError('ICE ' + state);
                }
            }
        };

        // --- Video track handler (native RTP decoding) ---
        this.pc.ontrack = (event) => {
            console.log('[WebRtcMedia] Track received: kind=' + event.track.kind +
                ' id=' + event.track.id);

            // Set jitter buffer to 12ms for resilience against packet reordering/loss
            try {
                for (const receiver of this.pc.getReceivers()) {
                    if (receiver.jitterBufferTarget !== undefined) {
                        // 4ms jitter buffer: enough for LAN jitter (1-3ms) while keeping latency minimal.
                        // Previously 12ms — reduced because it added ~72% of a frame interval
                        // (16.67ms at 60fps) as constant latency on every frame.
                        receiver.jitterBufferTarget = 4;
                    }
                }
            } catch (e) {
                console.warn('[WebRtcMedia] Failed to set jitter buffer target:', e.message);
            }

            if (event.track.kind === 'video') {
                console.log('[WebRtcMedia] Video track received, attaching to <video> element');
                if (this.videoElement) {
                    const stream = new MediaStream([event.track]);
                    this.videoElement.srcObject = stream;
                    this.videoElement.play().catch(e => {
                        console.warn('[WebRtcMedia] <video> play() failed:', e.message);
                    });
                } else {
                    console.warn('[WebRtcMedia] No video element set for video track');
                }

                // Log resolution once metadata loads
                if (this.videoElement) {
                    this.videoElement.onloadedmetadata = () => {
                        console.log('[WebRtcMedia] Video resolution: ' +
                            this.videoElement.videoWidth + 'x' + this.videoElement.videoHeight);
                    };
                }

                // Proactive IDR request 250ms after playback starts.
                // The first keyframe may have been lost during ICE negotiation;
                // this ensures the decoder has a clean reference frame to start from.
                if (this.videoElement) {
                    this.videoElement.oncanplay = () => {
                        setTimeout(() => {
                            this.send({ type: "request_idr" });
                        }, 250);
                    };
                }
            }
        };

        // --- Create negotiated DataChannels for audio + input ---
        this._createDataChannels();

        console.log('[WebRtcMedia] PC created, waiting for SDP offer...');
    }

    _createDataChannels() {
        // Audio DataChannel (ID=0, ordered=true, reliable)
        // PCM16 fragmented, same format as WebRtcDataChannel audio
        const audioInit = {
            negotiated: true,
            id: 0,
            ordered: true
        };
        this.dataChannels.audio = this.pc.createDataChannel(this.DC_AUDIO_LABEL, audioInit);
        this._setupDataChannel(this.DC_AUDIO_LABEL, this.dataChannels.audio);

        // Input DataChannel (ID=1, ordered=true, reliable)
        const inputInit = {
            negotiated: true,
            id: 1,
            ordered: true
        };
        this.dataChannels.input = this.pc.createDataChannel(this.DC_INPUT_LABEL, inputInit);
        this._setupDataChannel(this.DC_INPUT_LABEL, this.dataChannels.input);

        console.log('[WebRtcMedia] DataChannels created (audio=0, input=1)');
    }

    _setupDataChannel(label, dc) {
        dc.binaryType = 'arraybuffer';

        dc.onopen = () => {
            console.log('[WebRtcMedia] DC "' + label + '" open');

            // All DataChannels open together (same SCTP association)
            if (this._allDcOpen()) {
                console.log('[WebRtcMedia] All DataChannels open!');
                this.connected = true;
                this._clearWsCloseTimer();
                if (this.onOpen) this.onOpen();
                this._closeSignalingWs();
            }
        };

        dc.onclose = () => {
            console.log('[WebRtcMedia] DC "' + label + '" closed');
            if (!this._stopping && this.connected) {
                this._onError('DataChannel "' + label + '" closed unexpectedly');
            }
        };

        dc.onerror = (err) => {
            if (!this._stopping) {
                console.warn('[WebRtcMedia] DC "' + label + '" error:', err);
            }
        };

        dc.onmessage = (evt) => {
            if (this._stopping) return;

            if (label === this.DC_AUDIO_LABEL) {
                this._onAudioChunk(evt.data);
            } else if (label === this.DC_INPUT_LABEL) {
                try {
                    const msg = JSON.parse(evt.data);
                    if (msg.type === 'stats' || msg.type === 'pong') {
                        if (this.onStats) this.onStats(msg);
                    } else {
                        console.log('[WebRtcMedia] Input DC message:', msg);
                    }
                } catch (e) {
                    console.warn('[WebRtcMedia] Invalid input DC message:', evt.data);
                }
            }
        };
    }

    _allDcOpen() {
        return this.dataChannels.audio &&
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
        } else if (msg.type === 'ice-config') {
            // Dynamic ICE server configuration from backend.
            console.log('[WebRtcMedia] Received ice-config:', JSON.stringify(msg.iceServers));
            this._dynamicIceServers = msg.iceServers;
            if (this.pc) {
                try {
                    this.pc.setConfiguration({ iceServers: this._dynamicIceServers });
                    console.log('[WebRtcMedia] Updated PC ICE servers via setConfiguration');
                } catch (e) {
                    console.warn('[WebRtcMedia] Failed to update PC ICE config:', e.message);
                }
            }
        } else {
            console.warn('[WebRtcMedia] Unknown signaling message type:', msg.type);
        }
    }

    /**
     * Validate that the browser supports the video codec advertised in the SDP.
     * Parses the m=video line and checks against RTCRtpReceiver.getCapabilities().
     */
    _validateCodecSupport(sdp) {
        try {
            const caps = RTCRtpReceiver.getCapabilities('video');
            if (!caps) {
                console.warn('[WebRtcMedia] Cannot check codec support — no capabilities');
                return true; // Assume supported
            }

            // Extract codec names from the SDP offer
            const rtpmapRe = /a=rtpmap:\d+\s+(\w+)/gi;
            const sdpCodecs = new Set();
            let m;
            while ((m = rtpmapRe.exec(sdp)) !== null) {
                sdpCodecs.add(m[1].toUpperCase());
            }

            const browserCodecs = new Set();
            for (const c of caps.codecs) {
                browserCodecs.add(c.mimeType.replace('video/', '').toUpperCase());
            }

            console.log('[WebRtcMedia] SDP codecs:', [...sdpCodecs],
                'Browser codecs:', [...browserCodecs]);

            for (const codec of sdpCodecs) {
                if (browserCodecs.has(codec)) {
                    console.log('[WebRtcMedia] Codec supported by browser:', codec);
                    return true;
                }
            }

            console.warn('[WebRtcMedia] No matching codec found — browser may not support this stream');
            return false;
        } catch (e) {
            console.warn('[WebRtcMedia] Codec validation error:', e.message);
            return true; // Don't block on validation errors
        }
    }

    async _handleSdpOffer(sdp) {
        console.log('[WebRtcMedia] Received SDP offer, length=' + sdp.length);

        try {
            console.log('[WebRtcMedia] SDP offer (first 200):', sdp.substring(0, 200));

            // Validate codec support before committing
            const codecOk = this._validateCodecSupport(sdp);
            if (!codecOk) {
                console.warn('[WebRtcMedia] Codec not supported by browser — transport may fail');
            }

            const remoteDesc = new RTCSessionDescription({
                type: 'offer',
                sdp: sdp
            });
            await this.pc.setRemoteDescription(remoteDesc);
            console.log('[WebRtcMedia] Remote description set (offer)');

            // Create answer with receive-only constraints (lowest latency)
            const answer = await this.pc.createAnswer({
                offerToReceiveVideo: true,
                offerToReceiveAudio: true
            });

            // Apply low-latency SDP transformations before setLocalDescription
            let answerSdp = answer.sdp;
            // Direction: receive only (client is viewer, never sends media)
            answerSdp = answerSdp.replace(/a=sendrecv/g, 'a=recvonly');
            // Minimal packetization time (reduces audio/video buffering)
            answerSdp = answerSdp.replace(/a=ptime:\d+/g, 'a=ptime:10');

            const modifiedAnswer = new RTCSessionDescription({
                type: 'answer',
                sdp: answerSdp
            });
            await this.pc.setLocalDescription(modifiedAnswer);
            console.log('[WebRtcMedia] Local description set (answer, low-latency), sending...');

            // Send answer via signaling WS
            this._sendSignaling({
                type: 'sdp',
                sdp: this.pc.localDescription.sdp
            });
        } catch (e) {
            console.error('[WebRtcMedia] SDP handling error:', e.message);
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
                console.log('[WebRtcMedia] Added ICE candidate, mid=' + mid);
                this._logCount++;
            }
        } catch (e) {
            console.warn('[WebRtcMedia] Failed to add ICE candidate:', e.message);
        }
    }

    _sendSignaling(obj) {
        if (this.signalingWs && this.signalingWs.readyState === WebSocket.OPEN) {
            this.signalingWs.send(JSON.stringify(obj));
        }
    }

    _closeSignalingWs() {
        if (this.signalingWs) {
            console.log('[WebRtcMedia] Closing signaling WS (DataChannels established)');
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
    // Audio chunk reassembly (same format as WebRtcDataChannel)
    // =========================================================================

    _onAudioChunk(data) {
        this.stats.chunksReceived++;

        if (data instanceof ArrayBuffer) {
            data = new Uint8Array(data);
        } else if (data instanceof Blob) {
            return;
        }

        if (data.byteLength < this.FRAG_HEADER_SIZE) return;

        // Parse header (big endian, same format as WebRtcDataChannel)
        const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
        const payloadSize = view.getUint32(9, false);
        const payload = new Uint8Array(data.buffer, data.byteOffset + this.FRAG_HEADER_SIZE, payloadSize);

        // Audio packets are typically single-chunk, so we emit directly
        if (this.onAudio) {
            this.onAudio(payload);
        }
    }

    // =========================================================================
    // Audio reassembly cleanup
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
                this.stats.framesDropped++;
                if (this._frameLogCount < 5) {
                    console.warn('[WebRtcMedia] Dropping stale audio frame #' + frameId +
                        ': age=' + Math.round(age) + 'ms');
                    this._frameLogCount++;
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
        console.error('[WebRtcMedia] Error:', message);
        if (this.onError) this.onError(new Error(message));
        if (this.onClose) this.onClose();
        this.close();
    }
}
