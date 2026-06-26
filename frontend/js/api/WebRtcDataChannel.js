/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <https://www.gnu.org/licenses/>.
 */

import * as iosAudioUnlock from '../audio/iosAudioUnlock.js';

/**
 * Describe a WebSocket close code for diagnostic logging.
 * Returns a human-readable English string suitable for console output.
 */
function wsCloseDescription(code) {
    switch (code) {
        case 1000:
            return 'normal closure';
        case 1001:
            return 'endpoint going away';
        case 1002:
            return 'protocol error';
        case 1003:
            return 'unsupported data';
        case 1005:
            return 'no status code (normal)';
        case 1006:
            return 'abnormal closure — DNS / TLS / network timeout';
        case 1007:
            return 'invalid frame payload data';
        case 1008:
            return 'policy violation';
        case 1009:
            return 'message too big';
        case 1010:
            return 'mandatory extension';
        case 1011:
            return 'internal server error';
        case 1015:
            return 'TLS handshake failure — check system date/time';
        default:
            return 'code=' + code;
    }
}

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
 *   Header: [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4][backend_ts:4]
 *   backend_ts: backend monotonic ms timestamp (mod 2^32) for end-to-end latency.
 *   Max payload per chunk: 16000 bytes (under SCTP 16KB limit).
 *
 * Reassembly:
 *   - Buffers chunks per frame_id in a Map.
 *   - Completes frame when all chunks received (total_chunks == chunk_index + 1).
 *   - Incomplete frames (keyframe or delta) are DROPPED — a partial bitstream
 *     would crash the VideoDecoder. IDR recovery handles the gap.
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
        this.dataChannels = { video: null, input: null };
        this.connected = false;

        // Native RTP Opus audio track playback target. Audio is no longer a
        // DataChannel — the backend sends Opus over an RTP track on this same
        // PeerConnection; the browser decodes it (jitter buffer + FEC + PLC) and
        // we render it through this <audio> element. Set by StreamView.
        this.audioElement = null;

        // WSS mode: legacy StreamRelay WebSocket passthrough.
        // When true, this class acts as a simple WS client that receives
        // binary video/audio frames (2-byte header + payload) and sends
        // input commands as JSON text, without any WebRTC PeerConnection.
        this._wssMode = options.wssMode === true;

        // WSS fragmentation mode: when true, the backend sends video/audio with
        // a 17-byte fragmentation header matching the DataChannel protocol
        // (same format as the WS fallback path).  Each binary message:
        //   [channel:1][frag_header:17][payload...]
        // When false (legacy): [channel:1][flags:1][payload...]
        this._wssFragmented = options.wssFragmented === true;

        // ICE config — populated dynamically by backend ice-config message.
        // Fallback: Google public STUN if no message received before PC creation.
        this._dynamicIceServers = null;
        this._defaultIceServers = [
            { urls: 'stun:stun.l.google.com:19302' },
            { urls: 'stun:stun.cloudflare.com:3478' },
            { urls: 'stun:stun.nextcloud.com:443' },
            { urls: 'stun:relay.metered.ca:80' },
        ];

        // Callbacks — set by the caller
        this.onOpen = null; // All DataChannels open
        this.onClose = null; // Disconnected / error
        this.onError = null; // Error event
        this.onVideo = null; // (frame: Uint8Array, isKeyframe: boolean, backendTs: number)
        this.onAudio = null; // (sample: Uint8Array)
        this.onStats = null; // (msg: object) stats/pong messages from backend
        this.onTakeover = null; // () session taken over by another device

        // Stats
        this.stats = { framesReceived: 0, chunksReceived: 0, framesDropped: 0, framesAssembled: 0 };

        // Reassembly buffers: Map<frame_id, { chunks: Uint8Array[], total: number, keyframe: boolean, firstChunkTime: number }>
        this._reassembly = new Map();
        this._cleanupTimer = null;

        // Logging
        this._logCount = 0;
        this._frameLogCount = 0;
        this._idrLogCount = 0;

        // Starvation detection: track the timestamp of the last assembled frame.
        // If it exceeds STARVATION_TIMEOUT_MS without a new frame, request an IDR
        // to kick-start the decoder.
        this._lastAssembledTime = 0;
        this._starvationRequested = false;
        this.STARVATION_TIMEOUT_MS = 1000; // 1s without a frame — avoids false positives at 60fps with jitter

        // Guard
        this._stopping = false;
        this._closed = false; // separate from _stopping: markStopping() must not block close()

        // WS open/error tracking for better error diagnostics
        this._wsHadOpen = false; // set true once onopen fires
        this._wsHadError = false; // set true once onerror fires

        // WS fallback mode: when ICE times out (UDP blocked), the backend
        // sends video/audio data over the existing signaling WebSocket as
        // binary frames. Text frames carry input commands the other way.
        this._wsFallback = false;
        this._fallbackRequestTimer = null;

        // Chain-fallback mode: when set, ICE failures do NOT trigger the
        // in-session WS fallback. Instead they surface as an error so the
        // caller (StreamView → MoonlightApp) can relaunch with the next
        // transport in the priority chain (… → wss is a distinct attempt).
        this._chainFallback = false;

        // ICE connection timeout: if ICE doesn't reach "connected" within
        // 3s, trigger WebSocket fallback (UDP blocked, corporate firewall).
        this._iceTimeout = null;
        this.ICE_TIMEOUT_MS = 3000;

        // ICE state tracking — used to distinguish premature WS close
        // (before ICE) from graceful WS close after ICE connected but
        // before DataChannels open (which can recover).
        this._iceConnected = false;

        // Grace period timer: when the signaling WS closes after ICE is
        // connected but DataChannels haven't opened yet, we wait this long
        // for SCTP to complete before treating it as an error.
        this._wsCloseTimer = null;
        this.WS_GRACE_PERIOD_MS = 10000; // 10 seconds

        // Constants (must match backend)
        this.FRAG_HEADER_SIZE = 17;
        this.CLEANUP_INTERVAL_MS = 100; // Fast stale-frame detection (was 500)
        this.FRAME_TIMEOUT_MS = 500;

        // Channel labels (must match backend)
        this.DC_VIDEO_LABEL = 'video';
        this.DC_AUDIO_LABEL = 'audio';
        this.DC_INPUT_LABEL = 'input';

        // Shared IDR request throttle: minimum interval between any requestidr sent.
        // Covers stale-frame drops, starvation, and onFrameLoss — one timestamp for all.
        this._lastIdrRequestTime = 0;
        this.IDR_THROTTLE_MS = 500; // minimum ms between IDR requests (> backend 300ms cooldown)

        // Callback: (frameId, wasKeyframe) — fired when an assembled frame is dropped incomplete.
        this.onFrameLoss = null;

        this._lastAssembledFrameId = -1;
    }

    /**
     * Start the WebRTC connection: connect signaling WS, create PeerConnection,
     * wait for offer from backend, answer, exchange ICE.
     *
     * In WSS mode (legacy StreamRelay), skips WebRTC entirely and uses the
     * WebSocket as a direct binary passthrough for video/audio data.
     */
    connect() {
        if (this._stopping) return;

        console.log('[WebRTC] Connecting to signaling:', this.signalingUrl);
        this.signalingWs = new WebSocket(this.signalingUrl);

        if (this._wssMode) {
            // ── WSS mode: direct WebSocket binary passthrough ────────────────
            this.signalingWs.binaryType = 'arraybuffer';

            this.signalingWs.onopen = () => {
                this._wsHadOpen = true;
                console.log('[WSS] WS connected, stream ready');
                this.connected = true;
                if (this.onOpen) this.onOpen();
            };

            this.signalingWs.onmessage = (evt) => this._onWssMessage(evt);

            this.signalingWs.onerror = (err) => {
                console.error(
                    '[WSS] WS onerror' + (this._wsHadOpen ? ' (after open)' : ' (BEFORE open)'),
                );
                this._wsHadError = true;
                if (!this._stopping) {
                    if (!this._wsHadOpen) {
                        this._onError(
                            'Connexion au serveur de streaming impossible. ' +
                                'Verifiez votre pare-feu, antivirus (HTTPS Scanning), ' +
                                'ou proxy.',
                        );
                    } else {
                        this._onError('Erreur de connexion au serveur de streaming');
                    }
                }
            };

            this.signalingWs.onclose = (evt) => {
                const desc = wsCloseDescription(evt.code);
                console.log(
                    '[WSS] WS closed: ' +
                        desc +
                        ' (code=' +
                        evt.code +
                        ')' +
                        (evt.reason ? ', reason=' + evt.reason : ''),
                );
                if (!this._stopping && !this._wsHadError) {
                    // onerror may have already triggered — only act if it didn't.
                    if (evt.code === 1015) {
                        this._onError(
                            'Erreur de securite TLS. ' + "Verifiez la date et l'heure du systeme.",
                        );
                    } else if (this.connected) {
                        this._onError('Connexion au serveur de streaming interrompue');
                    } else {
                        this._onError(
                            'Connexion au serveur de streaming impossible. ' +
                                'Verifiez votre pare-feu, antivirus (HTTPS Scanning), ' +
                                'ou proxy.',
                        );
                    }
                }
            };
        } else {
            // ── Normal WebRTC mode: wait for SDP offer ──────────────────────────
            this.signalingWs.onopen = () => {
                this._wsHadOpen = true;
                console.log('[WebRTC] Signaling WS connected, waiting for ICE config...');
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
                console.error(
                    '[WebRTC] Signaling WS onerror' +
                        (this._wsHadOpen ? ' (after open)' : ' (BEFORE open)'),
                );
                this._wsHadError = true;
                if (!this._stopping) {
                    // onerror fires before onclose but carries no close code.
                    // Let onclose handle the user-facing error (it has the code).
                    // Only trigger now if we were already connected (runtime error).
                    if (this._wsHadOpen) {
                        this._onError('Erreur de connexion au serveur de streaming');
                    }
                }
            };

            this.signalingWs.onclose = (evt) => {
                const desc = wsCloseDescription(evt.code);
                console.log(
                    '[WebRTC] Signaling WS closed: ' +
                        desc +
                        ' (code=' +
                        evt.code +
                        ')' +
                        (evt.reason ? ', reason=' + evt.reason : ''),
                );
                this._wsHadError = false; // Reset so onclose can trigger its own error

                // If already connected (DCs open) or stopping, WS close is expected
                if (this.connected || this._stopping) return;

                if (this._iceConnected) {
                    console.log(
                        '[WebRTC] WS closed after ICE connected — waiting ' +
                            this.WS_GRACE_PERIOD_MS / 1000 +
                            's for DataChannels...',
                    );
                    this._wsCloseTimer = setTimeout(() => {
                        if (!this.connected && !this._stopping) {
                            this._onError('Timed out waiting for DataChannels after WS closed');
                        }
                    }, this.WS_GRACE_PERIOD_MS);
                } else {
                    // Close code tells us what went wrong
                    if (evt.code === 1015) {
                        this._onError(
                            'Erreur de securite TLS. ' + "Verifiez la date et l'heure du systeme.",
                        );
                    } else if (evt.code === 1006 || !this._wsHadOpen) {
                        this._onError(
                            'Connexion au serveur de streaming impossible. ' +
                                'Verifiez votre pare-feu, antivirus (HTTPS Scanning), ' +
                                'ou proxy.',
                        );
                    } else {
                        this._onError('Connexion au serveur de streaming interrompue');
                    }
                }
            };

            // Start cleanup timer (for chunk reassembly in WebRTC mode)
            this._cleanupTimer = setInterval(
                () => this._cleanupStaleFrames(),
                this.CLEANUP_INTERVAL_MS,
            );
        }
    }

    /**
     * Send a JSON message (typically input command) to the backend.
     * Uses the input DataChannel in normal mode, or the signaling WS in WSS/fallback mode.
     */
    send(obj) {
        // WSS mode or WS fallback mode: send via text on the signaling WS
        if (this._wssMode || this._wsFallback) {
            if (this.signalingWs && this.signalingWs.readyState === WebSocket.OPEN) {
                this.signalingWs.send(JSON.stringify(obj));
            }
            return;
        }

        // Normal mode: send via input DataChannel
        if (this.dataChannels.input && this.dataChannels.input.readyState === 'open') {
            this.dataChannels.input.send(JSON.stringify(obj));
        } else {
            // Input DC not ready — try signaling WS as fallback (handles the
            // race window during ICE→WS fallback transition where _wsFallback
            // hasn't been set yet but the signaling WS is still open).
            if (this.signalingWs && this.signalingWs.readyState === WebSocket.OPEN) {
                this.signalingWs.send(JSON.stringify(obj));
                return;
            }
            // No transport available — silently drop
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

    /** Cancel any pending fallback request timer. */
    _clearFallbackRequestTimer() {
        if (this._fallbackRequestTimer) {
            clearTimeout(this._fallbackRequestTimer);
            this._fallbackRequestTimer = null;
        }
    }

    /** Start the ICE connection timeout timer (3s). */
    _startIceTimer() {
        this._clearIceTimer();
        this._iceTimeout = setTimeout(() => this._onIceTimeout(), this.ICE_TIMEOUT_MS);
        console.log('[WebRTC] ICE timeout set to ' + this.ICE_TIMEOUT_MS / 1000 + 's');
    }

    /** Cancel the ICE connection timeout timer. */
    _clearIceTimer() {
        if (this._iceTimeout) {
            clearTimeout(this._iceTimeout);
            this._iceTimeout = null;
        }
    }

    /** Called when ICE fails to connect within 3s. */
    _onIceTimeout() {
        if (this._stopping || this.connected || this._iceConnected) return;
        this._iceTimeout = null;

        console.warn(
            '[WebRTC] ICE timeout — connection not established within ' +
                this.ICE_TIMEOUT_MS / 1000 +
                's',
        );

        // Chain-fallback: surface as an error so the caller relaunches with the
        // next transport instead of rerouting over the signaling WS in-session.
        if (this._chainFallback) {
            this._onError('ICE timeout');
            return;
        }

        // Try WS fallback: if the signaling WebSocket is still open (TCP),
        // send a fallback request to the backend.
        if (this.signalingWs && this.signalingWs.readyState === WebSocket.OPEN) {
            this._requestWsFallback('timeout');
        } else {
            // WS is gone — no fallback possible, display error
            this._onError(
                "La connexion WebRTC n'a pas pu être établie. " +
                    "Vérifiez que votre réseau autorise l'UDP sortant.",
            );
        }
    }

    /**
     * Request WebSocket fallback transport.
     *
     * Called when ICE fails before ever connecting (UDP blocked by corporate
     * firewall). Sends a request to the backend to route video/audio data over
     * the existing signaling WebSocket (TCP) instead of WebRTC DataChannels.
     *
     * A 5-second timeout guards against the case where the backend doesn't
     * respond (e.g. the WS is closed before the response arrives).
     *
     * @param {string} reason - Debug label ('disconnected', 'failed', 'timeout')
     */
    _requestWsFallback(reason) {
        if (this._wsFallback || this._stopping) return;

        console.warn('[WebRTC] ICE ' + reason + ' — requesting WS fallback from backend');

        // Cancel the ICE timeout — the 5s fallback timer below replaces it.
        this._clearIceTimer();
        this._clearFallbackRequestTimer();
        this._fallbackRequestTimer = setTimeout(() => {
            if (!this._wsFallback && !this._stopping) {
                console.error('[WebRTC] WS fallback response not received within 5s — error');
                this._onError(
                    "La connexion WebRTC n'a pas pu être établie. " +
                        "Vérifiez que votre réseau autorise l'UDP sortant.",
                );
            }
        }, 5000);

        this._sendSignaling({ type: 'fallback-ws-request' });
    }

    /** Mark as stopping without closing — suppresses "closed unexpectedly" noise. */
    markStopping() {
        this._stopping = true;
    }

    /** Close all connections and clean up. */
    close() {
        if (this._closed) return; // idempotent — but markStopping() must NOT short-circuit this
        this._closed = true;
        this._stopping = true;
        console.log('[WebRTC] Closing...');

        // Stop the cleanup timer in ALL modes — the WSS early-return below
        // used to leak it, leaving a zombie interval running forever.
        if (this._cleanupTimer) {
            clearInterval(this._cleanupTimer);
            this._cleanupTimer = null;
        }
        this._reassembly.clear();

        if (this._wssMode) {
            // WSS mode: only the WS needs closing
            if (this.signalingWs) {
                this.signalingWs.onopen = null;
                this.signalingWs.onmessage = null;
                this.signalingWs.onerror = null;
                this.signalingWs.onclose = null;
                if (
                    this.signalingWs.readyState === WebSocket.OPEN ||
                    this.signalingWs.readyState === WebSocket.CONNECTING
                ) {
                    try {
                        this.signalingWs.close();
                    } catch (e) {
                        /* ignore */
                    }
                }
                this.signalingWs = null;
            }
            this.connected = false;
            return;
        }

        // Clear any pending WS close grace timer
        this._clearWsCloseTimer();

        // Clear ICE timeout
        this._clearIceTimer();

        // Clear fallback request timer
        this._clearFallbackRequestTimer();

        this._wsFallback = false;

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
                try {
                    dc.close();
                } catch (e) {
                    /* ignore */
                }
            }
            this.dataChannels[label] = null;
        }

        // Close PeerConnection
        if (this.pc) {
            try {
                this.pc.close();
            } catch (e) {
                /* ignore */
            }
            this.pc = null;
        }

        // Close signaling WS
        if (this.signalingWs) {
            this.signalingWs.onopen = null;
            this.signalingWs.onmessage = null;
            this.signalingWs.onerror = null;
            this.signalingWs.onclose = null;
            if (
                this.signalingWs.readyState === WebSocket.OPEN ||
                this.signalingWs.readyState === WebSocket.CONNECTING
            ) {
                try {
                    this.signalingWs.close();
                } catch (e) {
                    /* ignore */
                }
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

        // Use dynamically-received ICE servers from backend, or fall back
        // to the default Google public STUN server.
        const iceServers = this._dynamicIceServers || this._defaultIceServers;
        const config = {
            iceServers: iceServers,
            iceTransportPolicy: 'all',
            bundlePolicy: 'max-bundle',
            rtcpMuxPolicy: 'require',
        };
        console.log('[WebRTC] ICE servers:', JSON.stringify(iceServers));

        this.pc = new RTCPeerConnection(config);

        // --- Native RTP audio track (Opus, browser-decoded) ---
        // The backend offers a send-only Opus track; the browser handles jitter
        // buffer + FEC + PLC. On mobile playStream routes it through the
        // gesture-blessed element (autoplay unlock); on desktop it returns false
        // and we play it on our own <audio> element.
        this.pc.ontrack = (evt) => {
            if (evt.track.kind !== 'audio') return;
            console.log('[WebRTC] Audio track received');
            const stream = new MediaStream([evt.track]);
            if (!iosAudioUnlock.playStream(stream) && this.audioElement) {
                this.audioElement.srcObject = stream;
                const p = this.audioElement.play();
                if (p && p.catch)
                    p.catch((e) => console.warn('[WebRTC] audio play() failed:', e.message));
            }
        };

        // --- ICE candidate handler (filter TURN, prioritize UDP) ---
        this.pc.onicecandidate = (evt) => {
            if (
                !evt.candidate ||
                !this.signalingWs ||
                this.signalingWs.readyState !== WebSocket.OPEN
            ) {
                return;
            }

            // Drop TURN relay candidates — direct connection only
            if (evt.candidate.candidate.indexOf(' typ relay ') !== -1) {
                console.log('[WebRTC] Dropping TURN relay candidate');
                return;
            }

            this._sendSignaling({
                type: 'ice',
                candidate: evt.candidate.candidate,
                mid: evt.candidate.sdpMid || '0',
            });
        };

        // --- ICE state ---
        this.pc.oniceconnectionstatechange = () => {
            const state = this.pc.iceConnectionState;
            console.log('[WebRTC] ICE state:', state);
            if (state === 'connected' || state === 'completed') {
                this._iceConnected = true;
                // Cancel ICE timeout — connection established
                this._clearIceTimer();
                // Clear fallback request timer — ICE came up
                this._clearFallbackRequestTimer();
                // DataChannels should open shortly via SCTP
            } else if (state === 'disconnected' || state === 'failed') {
                if (!this._stopping) {
                    if (!this._iceConnected) {
                        // ICE transitioned to disconnected/failed BEFORE ever
                        // reaching connected. Chain-fallback: surface as error
                        // so the caller relaunches with the next transport.
                        // Otherwise try the in-session WS fallback (UDP blocked).
                        if (this._chainFallback) {
                            this._clearIceTimer();
                            this._onError('ICE ' + state);
                        } else {
                            this._requestWsFallback(state);
                        }
                    } else {
                        // ICE WAS connected before — real disconnection.
                        // Cancel ICE timeout — ICE already failed
                        this._clearIceTimer();
                        // If we were waiting for DCs after WS close, the ICE
                        // disconnect is definitive — stop waiting and error.
                        this._clearWsCloseTimer();
                        this._onError('ICE ' + state);
                    }
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
        // Video DataChannel (ID=0, ordered=false, maxRetransmits=3)
        // Must match backend DataChannelRelay::createDataChannels(): maxRetransmits=3.
        // Partial reliability: a keyframe spans ~140 UDP packets, so with zero
        // retransmits a single loss kills the whole frame and forces IDR recovery.
        const videoInit = {
            negotiated: true,
            id: 0,
            ordered: false,
            maxRetransmits: 3,
        };
        this.dataChannels.video = this.pc.createDataChannel('video', videoInit);
        this._setupDataChannel('video', this.dataChannels.video);

        // NOTE: no audio DataChannel — audio is a native RTP Opus track now (id=1
        // is intentionally left unused so the input channel keeps its id=2,
        // matching the backend).

        // Input DataChannel (ID=2, ordered=true, reliable)
        const inputInit = {
            negotiated: true,
            id: 2,
            ordered: true,
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
                // Parse server-to-client JSON messages (stats, pong, rumble, etc.)
                try {
                    const msg = JSON.parse(evt.data);
                    if (msg.type === 'stats' || msg.type === 'pong') {
                        if (this.onStats) this.onStats(msg);
                    } else if (msg.type === 'takeover') {
                        if (this.onTakeover) this.onTakeover();
                    } else {
                        console.log('[WebRTC] Input DC message:', msg);
                    }
                } catch (e) {
                    console.warn('[WebRTC] Invalid input DC message:', evt.data);
                }
            }
        };
    }

    _allDcOpen() {
        // Audio is an RTP track now (not a DataChannel) — only video + input gate
        // the "connected" state. The audio track flows over the same transport.
        return (
            this.dataChannels.video &&
            this.dataChannels.video.readyState === 'open' &&
            this.dataChannels.input &&
            this.dataChannels.input.readyState === 'open'
        );
    }

    // =========================================================================
    // Signaling handlers
    // =========================================================================

    _handleSignalingMessage(msg) {
        if (msg.type === 'sdp') {
            this._handleSdpOffer(msg.sdp);
        } else if (msg.type === 'ice') {
            this._handleIceCandidate(msg.candidate, msg.mid);
        } else if (msg.type === 'fallback-ws') {
            // Backend initiated WS fallback (ICE timeout — UDP blocked).
            // The signaling WebSocket will now carry video/audio as binary frames.
            console.log('[WebRTC] Received fallback-ws from backend — switching to WS transport');
            this._handleFallbackWs();
        } else if (msg.type === 'fallback-ws-ack') {
            // Response to our fallback-ws-request — backend confirms fallback
            console.log('[WebRTC] Received fallback-ws-ack from backend');
            if (!this._wsFallback) {
                this._handleFallbackWs();
            }
        } else if (msg.type === 'ice-config') {
            // Dynamic ICE server configuration from backend.
            // Overrides the default Google STUN with the user-configured server.
            console.log('[WebRTC] Received ice-config:', JSON.stringify(msg.iceServers));
            this._dynamicIceServers = msg.iceServers;
            // Create the PeerConnection now that we have ICE servers.
            // Previously this happened in onopen, but we need the ice-config
            // to ensure proper ICE candidate generation from the start.
            if (!this.pc) {
                this._createPeerConnection();
            }
        } else {
            console.warn('[WebRTC] Unknown signaling message type:', msg.type);
        }
    }

    async _handleSdpOffer(sdp) {
        console.log('[WebRTC] Received SDP offer, length=' + sdp.length);

        // Safety net: if ice-config never arrived, create PC now.
        // This can happen if the backend sends the SDP offer before the
        // ice-config message is processed (rare race on slow connections).
        if (!this.pc) {
            console.log('[WebRTC] Creating PC in SDP handler (ice-config not received)');
            this._createPeerConnection();
        }

        try {
            const remoteDesc = new RTCSessionDescription({
                type: 'offer',
                sdp: sdp,
            });
            await this.pc.setRemoteDescription(remoteDesc);
            console.log('[WebRTC] Remote description set (offer)');

            // Create answer
            const answer = await this.pc.createAnswer();
            await this.pc.setLocalDescription(answer);
            console.log('[WebRTC] Local description set (answer), sending...');

            // Start ICE timeout: if ICE doesn't reach connected/completed
            // within ICE_TIMEOUT_MS, the connection has failed (likely UDP
            // blocked by a corporate firewall).
            this._startIceTimer();

            // Send answer via signaling WS
            this._sendSignaling({
                type: 'sdp',
                sdp: this.pc.localDescription.sdp,
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
                sdpMid: mid,
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
            if (
                this.signalingWs.readyState === WebSocket.OPEN ||
                this.signalingWs.readyState === WebSocket.CONNECTING
            ) {
                try {
                    this.signalingWs.close();
                } catch (e) {
                    /* ignore */
                }
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
        const frameId = view.getUint32(0, false); // offset 0, big endian
        const chunkIndex = view.getUint16(4, false); // offset 4, big endian
        const totalChunks = view.getUint16(6, false); // offset 6, big endian
        const isKeyframe = view.getUint8(8) !== 0; // offset 8
        const payloadSize = view.getUint32(9, false); // offset 9, big endian
        const backendTs = view.getUint32(13, false); // offset 13, big endian
        const payload = new Uint8Array(
            data.buffer,
            data.byteOffset + this.FRAG_HEADER_SIZE,
            payloadSize,
        );

        // Check if we already have an entry for this frame
        let entry = this._reassembly.get(frameId);
        if (!entry) {
            entry = {
                chunks: [],
                total: totalChunks,
                received: 0,
                keyframe: isKeyframe,
                firstChunkTime: performance.now(),
                completed: false,
                backendTs: backendTs, // Same for all chunks in a frame
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
        // If any chunks are missing, drop the frame entirely.
        // For keyframes: required for correct SPS/PPS extraction.
        // For delta frames: missing bytes produce an invalid bitstream that
        // crashes the VideoDecoder. It's better to drop and let the IDR
        // recovery mechanism handle it.
        if (entry.received < entry.total) {
            this.stats.framesDropped++;
            if (this._frameLogCount < 10) {
                const tag = entry.keyframe ? 'keyframe' : 'delta';
                console.warn(
                    '[WebRTC] Dropping incomplete ' +
                        tag +
                        ' #' +
                        frameId +
                        ': got ' +
                        entry.received +
                        '/' +
                        entry.total +
                        ' chunks',
                );
                this._frameLogCount++;
            }
            // Notify StreamView so it can invalidate decoder reference state
            if (this.onFrameLoss) this.onFrameLoss(frameId, entry.keyframe);
            // Request IDR via shared throttle — backend also throttles server-side
            this._requestIdrFrame('incomplete frame #' + frameId);
            return;
        }

        // All chunks received — assemble normally
        let totalSize = 0;
        for (let i = 0; i < entry.total; i++) {
            totalSize += entry.chunks[i].length;
        }

        const assembled = new Uint8Array(totalSize);
        let offset = 0;
        for (let i = 0; i < entry.total; i++) {
            assembled.set(entry.chunks[i], offset);
            offset += entry.chunks[i].length;
        }

        this.stats.framesAssembled++;
        this.stats.framesReceived++;

        if (frameId > this._lastAssembledFrameId) this._lastAssembledFrameId = frameId;

        // Track last assembled frame time for starvation detection.
        // Re-arm here (assembled frame), not on chunk arrival — a stream stuck
        // delivering chunks of incomplete frames must still trigger starvation.
        this._lastAssembledTime = performance.now();
        this._starvationRequested = false;

        // Emit video frame with backend timestamp for latency calculations
        if (this.onVideo) {
            this.onVideo(assembled, entry.keyframe, entry.backendTs, frameId);
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
        const payload = new Uint8Array(
            data.buffer,
            data.byteOffset + this.FRAG_HEADER_SIZE,
            payloadSize,
        );

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
        let staleCount = 0;

        for (const [frameId, entry] of this._reassembly) {
            if (entry.completed) {
                toDelete.push(frameId);
                continue;
            }

            const age = now - entry.firstChunkTime;
            if (age > this.FRAME_TIMEOUT_MS) {
                // Frame timed out — drop it
                this.stats.framesDropped++;
                staleCount++;
                if (this._frameLogCount < 5) {
                    console.warn(
                        '[WebRTC] Dropping stale frame #' +
                            frameId +
                            ': age=' +
                            Math.round(age) +
                            'ms, got ' +
                            entry.received +
                            '/' +
                            entry.total,
                    );
                    this._frameLogCount++;
                }
                // Notify StreamView so it can invalidate decoder reference state
                if (this.onFrameLoss) this.onFrameLoss(frameId, entry.keyframe);

                toDelete.push(frameId);
            }
        }

        for (const id of toDelete) {
            this._reassembly.delete(id);
        }

        // Proactive IDR request: if multiple frames went stale, the decoder
        // may be starved for data. Ask the backend to request an IDR from
        // Sunshine so we can recover quickly.
        if (staleCount >= 1) {
            this._requestIdrFrame('stale frames (' + staleCount + ' dropped)');
        }

        // Starvation detection: if no frame was assembled for STARVATION_TIMEOUT_MS
        // and we haven't already requested an IDR for this episode, request one.
        if (
            !this._starvationRequested &&
            this._lastAssembledTime > 0 &&
            now - this._lastAssembledTime > this.STARVATION_TIMEOUT_MS &&
            this.stats.framesAssembled > 5 /* skip initial quiet period */
        ) {
            this._starvationRequested = true;
            this._requestIdrFrame(
                'starvation (' + Math.round(now - this._lastAssembledTime) + 'ms since last frame)',
            );
        }
    }

    /** Request an IDR (key) frame from Sunshine via the input DataChannel or WS text (WSS/fallback mode).
     *  Client-side throttle: at most one request per IDR_THROTTLE_MS; backend also throttles server-side. */
    _requestIdrFrame(reason) {
        const now = performance.now();
        if (now - this._lastIdrRequestTime < this.IDR_THROTTLE_MS) {
            return; // Throttled — too soon since last request
        }
        this._lastIdrRequestTime = now;

        // WSS mode or WS fallback mode: send via signaling WS text message
        if (this._wssMode || this._wsFallback) {
            if (this.signalingWs && this.signalingWs.readyState === WebSocket.OPEN) {
                this._idrLogCount++;
                if (this._idrLogCount <= 5 || this._idrLogCount % 10 === 0) {
                    console.warn(
                        '[' +
                            (this._wssMode ? 'WSS' : 'WebRTC') +
                            '] Requesting IDR frame via WS fallback (' +
                            reason +
                            ')',
                    );
                }
                this.signalingWs.send(JSON.stringify({ type: 'requestidr' }));
            }
            return;
        }

        if (!this.dataChannels.input || this.dataChannels.input.readyState !== 'open') {
            // Cannot request IDR yet — input DC not open.  The backend will
            // buffer the keyframe from Sunshine and send it when the Video
            // DataChannel opens.
            if (this._idrLogCount < 3) {
                console.warn('[WebRTC] Cannot request IDR — input DC not open yet');
                this._idrLogCount++;
            }
            return;
        }

        this._idrLogCount++;
        if (this._idrLogCount <= 5 || this._idrLogCount % 10 === 0) {
            console.warn('[WebRTC] Requesting IDR frame (' + reason + ')');
        }
        this.dataChannels.input.send(JSON.stringify({ type: 'requestidr' }));
    }

    // =========================================================================
    // WebSocket fallback (when ICE times out — UDP blocked)
    // =========================================================================

    /**
     * Switch to WebSocket data transport when WebRTC ICE negotiation fails.
     *
     * The signaling WebSocket (already connected via TCP) is repurposed to carry
     * video/audio data as binary frames and input commands as text frames.
     * This provides a TCP-based fallback through restrictive corporate firewalls
     * that block UDP (preventing ICE from completing).
     *
     * Video/audio binary format (same as DataChannel fragmentation):
     *   [channel:1][frag_header:17][payload...]
     *   channel: 0x01=video, 0x02=audio
     *
     * After the switch, we set binaryType='arraybuffer' on the WS and route
     * incoming binary frames to the existing _onVideoChunk / _onAudioChunk
     * reassembly logic (stripping the channel prefix byte).
     */
    _handleFallbackWs() {
        if (this._wsFallback || this._stopping) return;
        console.log('[WebRTC] === SWITCHING TO WS FALLBACK ===');
        this._wsFallback = true;

        // Cancel ICE timeout — WS transport is now active
        this._clearIceTimer();
        // Cancel fallback request timer — fallback has been confirmed
        this._clearFallbackRequestTimer();

        // Configure WS for binary data reception
        if (this.signalingWs) {
            // Remove old message handlers — we install our own
            this.signalingWs.onmessage = (evt) => this._onWsFallbackMessage(evt);
            // Ensure binary data arrives as ArrayBuffer
            this.signalingWs.binaryType = 'arraybuffer';
        }

        // Close the PeerConnection — we're done with WebRTC
        // (DataChannels will never open since ICE failed)
        if (this.pc) {
            try {
                this.pc.close();
            } catch (e) {}
            this.pc = null;
        }
        this.dataChannels = { video: null, audio: null, input: null };

        // Mark as connected — triggers StreamView to set up decoder etc.
        this.connected = true;

        console.log('[WebRTC] WS fallback active — video/audio via binary WS, input via text WS');

        // Notify caller that transport is ready
        if (this.onOpen) this.onOpen();
    }

    /**
     * Handle incoming WS messages in fallback mode.
     * Binary → video/audio data, Text → ignored (no server-to-client text yet).
     */
    _onWsFallbackMessage(evt) {
        if (this._stopping) return;

        // Binary frames carry video/audio data with a 1-byte channel prefix
        if (evt.data instanceof ArrayBuffer) {
            const raw = new Uint8Array(evt.data);
            if (raw.length < 1) return;

            const channel = raw[0];
            // Strip channel prefix byte — the remaining data starts with 17-byte frag header
            const payload = raw.subarray(1);

            if (channel === 0x01) {
                // Video channel
                this._onVideoChunk(payload);
            } else if (channel === 0x02) {
                // Audio channel
                this._onAudioChunk(payload);
            } else {
                console.warn('[WebRTC] WS fallback: unknown channel byte:', channel);
            }
        } else if (typeof evt.data === 'string') {
            // Text messages from backend in fallback mode are currently unused
            // (reserved for future extensions like rumble or connection stats)
        }
    }

    // =========================================================================
    // WSS mode: legacy StreamRelay binary passthrough
    // =========================================================================

    /**
     * Handle incoming WebSocket messages in WSS mode.
     *
     * StreamRelay protocol (2-byte header + payload):
     *   [channel:1][flags:1][payload...]
     *   channel=0x01 video, channel=0x02 audio
     *   flags bit0: 1=keyframe (video only)
     *
     * The video payload is raw H.264 Annex B data; audio is raw PCM16.
     * Text messages from the backend are ignored (they carry no useful data).
     */
    _onWssMessage(evt) {
        if (this._stopping) return;

        // Text messages carry stats/pong JSON (backend StreamRelay). Route them
        // to onStats so the latency overlay works in WSS mode — previously these
        // were dropped, leaving "Latency: --".
        if (typeof evt.data === 'string') {
            try {
                const msg = JSON.parse(evt.data);
                if ((msg.type === 'stats' || msg.type === 'pong') && this.onStats) {
                    this.onStats(msg);
                } else if (msg.type === 'takeover' && this.onTakeover) {
                    this.onTakeover();
                }
            } catch (e) {
                /* ignore non-JSON text */
            }
            return;
        }

        if (evt.data instanceof ArrayBuffer) {
            const raw = new Uint8Array(evt.data);
            if (raw.length < 2) return;

            const channel = raw[0];

            // ── Fragmented mode (same format as WS fallback) ────────────────
            // Protocol: [channel:1][frag_header:17][payload...]
            // Routes to existing _onVideoChunk / _onAudioChunk reassembly logic.
            if (this._wssFragmented) {
                const fragData = raw.subarray(1); // Strip channel prefix byte
                if (fragData.length < this.FRAG_HEADER_SIZE) return;

                if (channel === 0x01) {
                    this._onVideoChunk(fragData);
                } else if (channel === 0x02) {
                    this._onAudioChunk(fragData);
                } else {
                    console.warn(
                        '[WSS] Fragmented: unknown channel byte: 0x' + channel.toString(16),
                    );
                }
                return;
            }

            // ── Legacy mode (2-byte header) ──────────────────────────────────
            // Protocol: [channel:1][flags:1][payload...]
            const flags = raw[1];
            const payload = raw.subarray(2);

            if (channel === 0x01) {
                // Video frame
                const isKeyframe = (flags & 0x01) !== 0;

                if (this.onVideo) {
                    this.onVideo(payload, isKeyframe, undefined);
                }
            } else if (channel === 0x02) {
                // Audio sample (PCM16)
                if (this.onAudio) {
                    this.onAudio(payload);
                }
            } else {
                console.warn('[WSS] Unknown channel byte: 0x' + channel.toString(16));
            }
            return;
        }

        // Text messages: pong/stats for the stats overlay, others ignored
        if (typeof evt.data === 'string') {
            try {
                const msg = JSON.parse(evt.data);
                if (msg.type === 'stats' || msg.type === 'pong') {
                    if (this.onStats) this.onStats(msg);
                } else if (msg.type === 'takeover') {
                    if (this.onTakeover) this.onTakeover();
                }
            } catch (e) {
                /* non-JSON text — ignore */
            }
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
