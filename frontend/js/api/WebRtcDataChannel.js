/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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
import { armAudioPlayRetry } from '../util/audioAutoplay.js';
import { forceOpusStereo } from '../util/SdpUtils.js';
import { openSignalingSocket } from '../net/tunnelBridge.js';
import {
    beginHandshake,
    helloMessage,
    signAnswer,
    verifyHostSignature,
    extractFingerprint,
} from '../util/pairingCrypto.js';
import { defaultIceServers } from './IceServers.js';

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
     * @param {boolean} [options.wssMode] - Legacy StreamRelay WS passthrough:
     *   act as a plain WS client, with no PeerConnection or DataChannels.
     * @param {boolean} [options.wssFragmented] - In WSS mode, expect the 17-byte
     *   fragmentation header (DataChannel protocol) instead of the legacy
     *   2-byte one.
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
        // Cleanup for the autoplay gesture retry, when play() was rejected.
        this._audioRetryCleanup = null;

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

        // ICE config — populated dynamically by the host's ice-config message,
        // which is authoritative and may legitimately be an empty list (LAN).
        // The fallback below only applies when no such message ever arrives.
        this._dynamicIceServers = null;
        this._defaultIceServers = defaultIceServers();

        // Callbacks — set by the caller
        this.onOpen = null; // All DataChannels open
        this.onClose = null; // Disconnected / error
        this.onError = null; // Error event
        this.onVideo = null; // (frame: Uint8Array, isKeyframe: boolean, backendTs: number)
        this.onAudio = null; // (sample: Uint8Array)
        this.onStats = null; // (msg: object) stats/pong messages from backend
        this.onTakeover = null; // () session taken over by another device
        this.onRevoked = null; // () this device's access was revoked by the admin
        this.onSessionEnded = null; // () the owner ended the session we were invited to

        // Stats
        this.stats = {
            framesReceived: 0,
            chunksReceived: 0,
            framesDropped: 0,
            framesAssembled: 0,
            // Episodes of silence longer than STARVATION_TIMEOUT_MS, and
            // ride-outs that ran their course without the stream recovering.
            stalls: 0,
            rideOutFailed: 0,
        };

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
        // Not "a few frame intervals": a still screen legitimately goes quiet.
        // The native host sends a liveness frame every 500 ms on a screen where
        // nothing moves (and Sunshine sends nothing at all), so this has to sit
        // above that floor with margin, or every paused desktop would read as a
        // stalled stream. 1 s is that margin; the host sized its floor to it.
        this.STARVATION_TIMEOUT_MS = 1000;
        // When the current silence began, for the "resumed after N ms" line;
        // 0 while frames flow.
        this._stalledSince = 0;

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

        // ICE connection timeout: if ICE doesn't reach "connected" in time,
        // trigger WebSocket fallback (UDP blocked, corporate firewall).
        //
        // The host runs the same deadline and sizes it from where this client
        // is (RelayBase::kIceTimeoutLocalMs / kIceTimeoutInternetMs), then
        // ships the value in ice-config. Whichever end fires first ends the
        // attempt, so this value must follow the host's: left at 3s it kept
        // cutting off internet handshakes — over 4G, an attempt whose video was
        // already flowing died here at exactly 3.0s. The 3s below is only the
        // pre-ice-config default, and what an older host implies.
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
        // Exponential backoff: while requests keep firing without a keyframe being
        // assembled in between, the interval doubles (500ms → 4s). Every IDR is a
        // large frame that inflates the bitrate exactly when the link is saturated;
        // occasional artifacts are preferable to feeding the congestion spiral.
        this._lastIdrRequestTime = 0;
        this.IDR_THROTTLE_MS = 500; // base interval between IDR requests (> backend 300ms cooldown)
        this.IDR_BACKOFF_MAX_MS = 4000;
        this._idrBackoffMs = this.IDR_THROTTLE_MS; // current adaptive interval
        this._idrOutstanding = false; // true until the next keyframe is assembled

        // Callback: (frameId, wasKeyframe) — fired when an assembled frame is dropped incomplete.
        this.onFrameLoss = null;
        // Callback: (reason) — fired when an IDR request is actually sent (post-throttle).
        // StreamView's congestion monitor counts these to detect a saturated link.
        this.onIdrRequested = null;

        this._lastAssembledFrameId = -1;

        // ── Riding out a gap instead of demanding a keyframe ─────────────────
        //
        // Set from the /start reply, and ONLY when the host confirmed its
        // encoder really is intra-refreshing (see `intra_refresh`). Requesting
        // nothing against a stream with no refresh wave would leave the picture
        // corrupt for good, so this is never assumed from the client's wish
        // alone.
        this.rideOutLoss = false;
        // When the current ride-out started, or 0 when not riding one out.
        this._rideOutSince = 0;
        // How long to let the refresh wave work before giving up and asking for
        // a keyframe after all. One cycle is 2 s whatever the stream's rate (the
        // native host sizes the sweep in frames of the rate it really encodes
        // at); a little more than that means the wave is genuinely not
        // repairing the picture — a decoder that handles damage badly, or loss
        // faster than the refresh. The worst case is then exactly today's
        // behaviour, one beat later.
        this.RIDE_OUT_MAX_MS = 2500;
        // Set when a ride-out ran its full course without the stream resuming.
        // Until the stream proves itself again — two frames in a row — recovery
        // goes back to plain keyframe requests. Without this, the very next loss
        // opens a fresh 2.5 s window and the watchdog's verdict is thrown away
        // the moment it is reached.
        this._rideOutFailed = false;
        // The wave's period in frames, when the host said it (/start
        // intra_refresh_frames). The wave advances once per frame ENCODED, so
        // its duration is not 2 s but "period frames at whatever rate frames
        // actually come": a game presenting at 30 under a 165 fps stream makes
        // a 330-frame sweep last 11 s, and a 2.5 s clock would have called it
        // failed four times over. So the watchdog counts frames it receives
        // (a frame not received advanced the wave too, but did not reach us —
        // counting only ours is the conservative side) against 1.2 periods,
        // with the wall clock kept as a backstop for a stream that stops. 0 =
        // period unknown, the 2.5 s clock stands alone.
        this.rideOutFrames = 0;
        this._rideOutFramesSeen = 0;
        this.RIDE_OUT_HARD_MAX_MS = 15000;
        // Set from the /start reply when the host heals a named loss with a
        // delta (ref_invalidation). Recovery then belongs to StreamView's gap
        // handler, which knows WHICH frames are missing and tells the host;
        // every keyframe request this transport would make on its own — an
        // incomplete frame, a stale one, a silence — is at best redundant with
        // that and at worst the largest frame there is, sent down a link that
        // just dropped something. So they are all counted here and sent
        // nowhere. Off for every host but the native one; nothing changes for
        // GameStream.
        this.healsByInvalidation = false;
        this._idrSuppressed = 0;
    }

    /**
     * The page is back from the background. The silence it caused was the
     * browser's, not the link's: do not count it as a stall, and do not act
     * on it now that frames flow again.
     */
    noteResumed() {
        this._lastAssembledTime = performance.now();
        this._starvationRequested = false;
        this._stalledSince = 0;
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
        // Through the rendezvous this is a socket carried on the control
        // channel; on a direct connection it is a plain WebSocket. Only the
        // signalling comes this way — video and input get their own peer
        // connection either way, negotiated over whichever this is.
        this.signalingWs = openSignalingSocket(this.signalingUrl);

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
                // MW-BIND-v1: announce our key and nonce. The host holds its SDP
                // offer until this arrives, since its signature covers the nonce.
                beginHandshake().then((identity) => {
                    this._mwBind = identity;
                    if (identity) this._sendSignaling(helloMessage(identity));
                });
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

    /** Start the ICE connection timeout timer (see ICE_TIMEOUT_MS). */
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

    /** Called when ICE fails to connect within ICE_TIMEOUT_MS. */
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

        // Drop the autoplay gesture retry: quitting before ever interacting must
        // not leave a listener on window. Placed BEFORE the WSS early-return so
        // it can never be skipped.
        if (this._audioRetryCleanup) {
            this._audioRetryCleanup();
            this._audioRetryCleanup = null;
        }

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

        // What the host said, or the fallback list when it said nothing. An
        // empty array from the host is an answer — "no STUN" — and survives the
        // `||` because an empty array is truthy.
        const iceServers = this._dynamicIceServers || this._defaultIceServers;
        /** @type {RTCConfiguration} */
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
                    p.catch((e) => {
                        // Autoplay policy rejected it (cold Media Engagement
                        // profile, Safari/Firefox defaults). Retry on the next
                        // gesture — in a stream the mouse-capture click always
                        // comes — instead of staying silent for the session.
                        console.warn('[WebRTC] audio play() failed:', e.message);
                        this._audioRetryCleanup = armAudioPlayRetry(this.audioElement);
                    });
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
        // Video DataChannel (ID=0, ordered=true, maxRetransmits=3)
        // Must match backend DataChannelRelay::createDataChannels().
        // Partial reliability: a keyframe spans ~140 UDP packets, so with zero
        // retransmits a single loss kills the whole frame and forces IDR recovery.
        // Ordered: frames reference their predecessor, so delivery order IS
        // decode order — unordered delivery turned every SCTP retransmit into a
        // false frameId gap (frame N completing after N+1) and an IDR cycle.
        const videoInit = {
            negotiated: true,
            id: 0,
            ordered: true,
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

        console.log('[WebRTC] Channels created (video=DC#0, audio=RTP, input=DC#2)');
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
                    if (
                        msg.type === 'stats' ||
                        msg.type === 'pong' ||
                        msg.type === 'rumble' ||
                        msg.type === 'clipboard' ||
                        msg.type === 'clipboardcaps' ||
                        msg.type === 'cursor'
                    ) {
                        if (this.onStats) this.onStats(msg);
                    } else if (msg.type === 'takeover') {
                        if (this.onTakeover) this.onTakeover();
                    } else if (msg.type === 'revoked') {
                        if (this.onRevoked) this.onRevoked();
                    } else if (msg.type === 'session-ended') {
                        if (this.onSessionEnded) this.onSessionEnded();
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
            this._handleSdpOffer(msg);
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
            // The host's own ICE servers, which replace the fallback list —
            // including when the host sends an empty list, which is what a LAN
            // session does rather than pointing the browser at a STUN server
            // this side has already decided not to use.
            console.log('[WebRTC] Received ice-config:', JSON.stringify(msg.iceServers));
            this._dynamicIceServers = msg.iceServers;
            // Adopt the host's ICE deadline: it classified where this client
            // actually is, which this side cannot do. Bounded so a malformed or
            // hostile value can neither disable the watchdog nor hang the UI on
            // a dead attempt. Absent → keep the default (older host).
            if (Number.isFinite(msg.iceTimeoutMs)) {
                this.ICE_TIMEOUT_MS = Math.min(Math.max(msg.iceTimeoutMs, 3000), 30000);
                console.log('[WebRTC] ICE deadline from host: ' + this.ICE_TIMEOUT_MS + 'ms');
            }
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

    async _handleSdpOffer(msg) {
        const sdp = msg.sdp;
        console.log('[WebRTC] Received SDP offer, length=' + sdp.length);

        // ── MW-BIND-v1 step 2 ───────────────────────────────────────────────
        // Verify the host BEFORE creating any DTLS state. On failure we return
        // without ever calling setRemoteDescription, so nothing was negotiated
        // with whoever sent this.
        if (this._mwBind?.hostPublicKey) {
            const ok = await verifyHostSignature(this._mwBind, msg);
            if (!ok) {
                this._onError(
                    'This host could not prove its identity — refusing to connect. ' +
                        'If you re-installed MoonlightWeb on it, pair again with a PIN.',
                );
                return;
            }
            console.log('[MW-BIND] Host signature verified');
        } else if (!msg.nonce) {
            // The host did not sign, so this session has no pairing key bound.
            // Normal on the host machine itself, which gets local privilege
            // without a session — and where no relay exists to guard against.
            console.log('[MW-BIND] Unsigned offer — this session has no pairing key');
        } else if (this._mwBind) {
            // The host signed but we have no identity to check it against: our
            // stored record is incomplete (site data cleared, or paired before
            // the host had a key). We can still sign our own half, which is what
            // the host verifies; re-pairing restores the other direction.
            console.warn('[MW-BIND] No stored host identity — cannot verify this offer');
        }

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
            await this._flushPendingCandidates();

            // Create answer. Stereo Opus decode for the RTP audio track:
            // without stereo=1 in the answer fmtp the browser decodes MONO
            // and downmixes (L+R)/2 → ~-6 dB quieter.
            const answer = await this.pc.createAnswer();
            const modifiedAnswer = new RTCSessionDescription({
                type: 'answer',
                sdp: forceOpusStereo(answer.sdp),
            });
            await this.pc.setLocalDescription(modifiedAnswer);
            console.log('[WebRTC] Local description set (answer), sending...');

            // Start ICE timeout: if ICE doesn't reach connected/completed
            // within ICE_TIMEOUT_MS, the connection has failed (likely UDP
            // blocked by a corporate firewall).
            this._startIceTimer();

            // Send answer via signaling WS, signed when this browser is paired.
            const answerMsg = {
                type: 'sdp',
                sdp: this.pc.localDescription.sdp,
            };

            // MW-BIND-v1 step 3: sign the fingerprint of the answer we just
            // committed to, over the host's nonce and fingerprint. The host
            // refuses the answer if this is missing or does not verify.
            if (this._mwBind && msg.nonce) {
                const sig = await signAnswer(
                    this._mwBind,
                    msg.host_id || this._mwBind.hostId || '',
                    msg.nonce,
                    extractFingerprint(sdp),
                    answerMsg.sdp,
                );
                if (!sig) {
                    this._onError('Could not sign this connection — refusing to continue.');
                    return;
                }
                answerMsg.sig = sig;
            }

            this._sendSignaling(answerMsg);
        } catch (e) {
            console.error('[WebRTC] SDP handling error:', e.message);
            this._onError('SDP negotiation failed: ' + e.message);
        }
    }

    /**
     * Add a remote ICE candidate, or hold it until there is a remote
     * description to add it to.
     *
     * Trickle ICE has no ordering guarantee, and addIceCandidate() on a
     * PeerConnection with no remote description throws — after which the
     * candidate is gone. That is not theoretical: the host holds its offer back
     * until the MW-BIND hello while its candidates go out immediately, so every
     * session lost the host's own candidates this way (five per stream on
     * 2026-08-30) and had to rediscover the host's address peer-reflexively.
     */
    async _handleIceCandidate(candidate, mid) {
        if (this._stopping) return;

        if (!this.pc || !this.pc.remoteDescription) {
            (this._pendingRemoteCandidates ||= []).push({ candidate, mid });
            return;
        }

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

    /** Flush the candidates held by _handleIceCandidate. Call right after
     *  setRemoteDescription resolves. */
    async _flushPendingCandidates() {
        const held = this._pendingRemoteCandidates;
        if (!held || held.length === 0) return;
        this._pendingRemoteCandidates = [];
        console.log('[WebRTC] Applying ' + held.length + ' ICE candidate(s) held before the offer');
        for (const c of held) await this._handleIceCandidate(c.candidate, c.mid);
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

        // Read BEFORE the id advances: the contiguity test further down needs the
        // previous frame's id, and taking it afterwards compares a frame against
        // itself.
        const previousFrameId = this._lastAssembledFrameId;
        if (frameId > this._lastAssembledFrameId) this._lastAssembledFrameId = frameId;

        // Track last assembled frame time for starvation detection.
        // Re-arm here (assembled frame), not on chunk arrival — a stream stuck
        // delivering chunks of incomplete frames must still trigger starvation.
        this._lastAssembledTime = performance.now();
        this._starvationRequested = false;
        if (this._stalledSince) {
            console.warn(
                '[WebRTC] Stream resumed after ' +
                    Math.round(this._lastAssembledTime - this._stalledSince) +
                    ' ms of silence (stall ' +
                    this.stats.stalls +
                    ')',
            );
            this._stalledSince = 0;
        }
        // One more frame of the refresh wave seen, while a gap is being ridden out.
        if (this._rideOutSince) this._rideOutFramesSeen++;

        // Ordered delivery: a frame this one overtook can never complete —
        // its missing chunks were given up on by the sender (maxRetransmits)
        // or they would have come first. Declare it lost now rather than
        // 500 ms from now (FRAME_TIMEOUT_MS), which is how long the picture
        // would otherwise stay one frame behind the one it could show. Only
        // on the native path: the GameStream path keeps its clock.
        if (this.healsByInvalidation && this._reassembly.size > 0) {
            for (const [id, older] of this._reassembly) {
                if (id >= frameId || older.completed) continue;
                this.stats.framesDropped++;
                if (this._frameLogCount < 5) {
                    console.warn(
                        '[WebRTC] Frame #' +
                            id +
                            ' overtaken by #' +
                            frameId +
                            ' with ' +
                            older.received +
                            '/' +
                            older.total +
                            ' chunks — lost',
                    );
                    this._frameLogCount++;
                }
                if (this.onFrameLoss) this.onFrameLoss(id, older.keyframe);
                this._reassembly.delete(id);
            }
        }

        // Keyframe assembled: IDR recovery completed — reset the request backoff.
        if (entry.keyframe) {
            this._idrOutstanding = false;
            this._idrBackoffMs = this.IDR_THROTTLE_MS;
        }

        // A contiguous frame means the gap is behind us: whatever damage there
        // was is now being repaired by the refresh wave rather than by a
        // keyframe, so close the ride-out window. Leaving it open would let the
        // watchdog fire on a stream that had already recovered, and put back
        // the very IDR this exists to avoid.
        //
        // Two frames in a row is the criterion, not one: the first frame after a
        // gap is by definition NOT contiguous with what came before it, so it
        // proves only that something arrived. The one after it proves the stream
        // is running in order again.
        if (frameId === previousFrameId + 1) {
            this._rideOutSince = 0;
            this._rideOutFailed = false;
        }

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
        //
        // With a host that heals by invalidation there is nothing to request:
        // a frame that has not come is not a frame that was lost. If frames
        // were dropped on the way, the first one to arrive carries a frameId
        // gap and StreamView names the missing ones to the host; if none were,
        // the stream simply resumes. Either way a keyframe here would be
        // waste. The episode is still counted and timed — it is the number
        // that says how often the picture froze.
        if (
            !this._starvationRequested &&
            this._lastAssembledTime > 0 &&
            now - this._lastAssembledTime > this.STARVATION_TIMEOUT_MS &&
            this.stats.framesAssembled > 5 /* skip initial quiet period */
        ) {
            this._starvationRequested = true;
            this.stats.stalls++;
            this._stalledSince = this._lastAssembledTime;
            if (this.healsByInvalidation) {
                if (this.stats.stalls <= 5 || this.stats.stalls % 20 === 0) {
                    console.warn(
                        '[WebRTC] No frame for ' +
                            Math.round(now - this._lastAssembledTime) +
                            ' ms — waiting, the host names what is lost (stall ' +
                            this.stats.stalls +
                            ')',
                    );
                }
            } else {
                this._requestIdrFrame(
                    'starvation (' +
                        Math.round(now - this._lastAssembledTime) +
                        'ms since last frame)',
                );
            }
        }
    }

    /** Request an IDR (key) frame from Sunshine via the input DataChannel or WS text (WSS/fallback mode).
     *  Client-side throttle with exponential backoff: at most one request per adaptive
     *  interval (500ms base, doubling to 4s while no keyframe arrives); backend also
     *  throttles server-side. */
    _requestIdrFrame(reason) {
        const now = performance.now();

        // The host heals what it is told it lost: StreamView tells it. Nothing
        // this transport could ask for on its own would help — see the flag.
        if (this.healsByInvalidation) {
            this._idrSuppressed++;
            if (this._idrSuppressed <= 3 || this._idrSuppressed % 50 === 0) {
                console.log(
                    '[WebRTC] No IDR asked for (' +
                        reason +
                        ') — the host heals named losses with deltas (' +
                        this._idrSuppressed +
                        ' so far)',
                );
            }
            return;
        }

        // Riding it out: the stream carries a moving band of intra blocks, so
        // the picture repairs itself over one cycle. Asking for a keyframe here
        // would undo that AND send the largest frame there is down a link that
        // just proved it was struggling — the drop is what brought us here.
        //
        // The watchdog is what makes this safe to try: if the picture is still
        // not right after one cycle plus a margin, the wave is not doing its
        // job and we fall back to exactly today's recovery. "One cycle" is
        // counted in frames received when the host said how long its wave is,
        // on the clock otherwise — see rideOutFrames.
        if (this.rideOutLoss && !this._rideOutFailed) {
            if (!this._rideOutSince) {
                this._rideOutSince = now;
                this._rideOutFramesSeen = 0;
                console.log('[WebRTC] Riding out a gap (' + reason + ') — no IDR requested');
                return;
            }
            const elapsedMs = now - this._rideOutSince;
            const waveHadItsChance =
                this.rideOutFrames > 0
                    ? this._rideOutFramesSeen >= Math.ceil(this.rideOutFrames * 1.2) ||
                      elapsedMs >= this.RIDE_OUT_HARD_MAX_MS
                    : elapsedMs >= this.RIDE_OUT_MAX_MS;
            if (!waveHadItsChance) return;
            this.stats.rideOutFailed++;
            if (this.stats.rideOutFailed <= 3 || this.stats.rideOutFailed % 20 === 0) {
                console.warn(
                    '[WebRTC] Ride-out did not recover after ' +
                        Math.round(elapsedMs) +
                        ' ms / ' +
                        this._rideOutFramesSeen +
                        ' frames — falling back to an IDR request (' +
                        this.stats.rideOutFailed +
                        ' so far)',
                );
            }
            this._rideOutSince = 0;
            this._rideOutFailed = true;
        }

        if (now - this._lastIdrRequestTime < this._idrBackoffMs) {
            return; // Throttled — too soon since last request
        }
        // Previous request never yielded an assembled keyframe → back off.
        // Reset to base when a keyframe is assembled (see _assembleFrame).
        if (this._idrOutstanding) {
            this._idrBackoffMs = Math.min(this._idrBackoffMs * 2, this.IDR_BACKOFF_MAX_MS);
        }
        this._idrOutstanding = true;
        this._lastIdrRequestTime = now;
        if (this.onIdrRequested) this.onIdrRequested(reason);

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
     * StreamRelay protocol, two shapes sharing the channel prefix byte
     * (channel=0x01 video, channel=0x02 audio):
     *   fragmented (_wssFragmented): [channel:1][frag_header:17][payload...]
     *   legacy:                      [channel:1][flags:1][payload...]
     *                                flags bit0: 1=keyframe (video only)
     *
     * The video payload is raw H.264/HEVC/AV1 Annex B; audio is encoded Opus
     * (moonlight-common-c never hands us PCM), decoded by AudioPipeline.
     * Text messages carry stats/pong/rumble/clipboard JSON.
     */
    _onWssMessage(evt) {
        if (this._stopping) return;

        // Text messages carry stats/pong JSON (backend StreamRelay). Route them
        // to onStats so the latency overlay works in WSS mode — previously these
        // were dropped, leaving "Latency: --".
        if (typeof evt.data === 'string') {
            try {
                const msg = JSON.parse(evt.data);
                if (
                    (msg.type === 'stats' ||
                        msg.type === 'pong' ||
                        msg.type === 'rumble' ||
                        msg.type === 'clipboard' ||
                        msg.type === 'clipboardcaps') &&
                    this.onStats
                ) {
                    this.onStats(msg);
                } else if (msg.type === 'takeover' && this.onTakeover) {
                    this.onTakeover();
                } else if (msg.type === 'revoked' && this.onRevoked) {
                    this.onRevoked();
                } else if (msg.type === 'session-ended' && this.onSessionEnded) {
                    this.onSessionEnded();
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
                if (
                    msg.type === 'stats' ||
                    msg.type === 'pong' ||
                    msg.type === 'rumble' ||
                    msg.type === 'clipboard' ||
                    msg.type === 'clipboardcaps'
                ) {
                    if (this.onStats) this.onStats(msg);
                } else if (msg.type === 'takeover') {
                    if (this.onTakeover) this.onTakeover();
                } else if (msg.type === 'revoked') {
                    if (this.onRevoked) this.onRevoked();
                } else if (msg.type === 'session-ended') {
                    if (this.onSessionEnded) this.onSessionEnded();
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
