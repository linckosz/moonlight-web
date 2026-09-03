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

#pragma once

#include "RelayBase.h"
#include "FrameSender.h"
#include <QByteArray>
#include <QElapsedTimer>
#include <QMutex>
#include <QTimer>
#include <memory>
#include <atomic>
#include <string>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <mutex>
#include <chrono>

namespace rtc {
class DataChannel;
class Track;
} // namespace rtc

class IMediaEngine;

// WebRTC DataChannel relay that replaces StreamRelay.
// Forwards video/audio from the media engine + input from the browser over a
// hybrid PeerConnection:
//   - video: DataChannel (SCTP, negotiated id=0, ordered, maxRetransmits=3)
//   - audio: rtc::Track (Opus over RTP) — not a DataChannel, despite the name
//   - input: DataChannel (SCTP, negotiated id=2, reliable + ordered)
//
// Thread safety:
// - The relay lives on a per-session relay thread (Session::spawnRelayThread),
//   together with the media engine and the signaling server.
// - GameStream engines: media engine signals are queued onto the relay thread
//   (AutoConnection), byte for byte the historical path.
// - Native engine: no signal at all. The relay installs itself as the engine's
//   direct frame sink (NativeMediaEngine::setDirectFrameSink), so
//   handleVideoFrame runs on the capture thread that just finished encoding,
//   reading the encoder's own buffer, and the wire chunks are built right
//   there — no event-loop hop and no intermediate copy before the frame
//   reaches the sender. Everything that path touches (buffered keyframe, IDR
//   gate, drop counters, the video DC pointer) is guarded by m_VideoMutex, the
//   arrangement MediaTrackRelay already runs with.
// - libdatachannel callbacks (onMessage for input) fire from internal threads.
//   We marshal input back to the relay thread via QMetaObject::invokeMethod.
class DataChannelRelay : public RelayBase
{
    Q_OBJECT

public:
    explicit DataChannelRelay(IMediaEngine* engine, QObject* parent = nullptr);
    ~DataChannelRelay() override;

    // PeerConnection access (not part of RelayBase interface)
    std::shared_ptr<rtc::PeerConnection> peerConnection() const { return m_Pc; }

    // ── RelayBase interface ─────────────────────────────────────────────────

    bool prepare(const rtc::Configuration& config, bool isInternet = false) override;

    bool setRemoteDescription(const std::string& sdp) override;

    bool addRemoteCandidate(const std::string& candidate, const std::string& mid) override;

    void stop() override;

    void notifyClientTakenOver() override;

    void notifyClientRevoked() override;
    void notifyClientSessionEnded() override;

    /// Retrieve and clear the buffered keyframe (if any).
    /// Used by SignalingServer before stop() to preserve the keyframe for
    /// WebSocket fallback — without this, the fallback starts with delta
    /// frames and the browser's VideoDecoder can never configure.
    QByteArray takeBufferedKeyframe()
    {
        std::lock_guard<std::mutex> lk(m_VideoMutex);
        QByteArray kf = m_BufferedKeyframe;
        m_BufferedKeyframe.clear();
        m_BufferedKeyframePresUs = -1;
        m_HaveBufferedKeyframe = false;
        m_NewKeyframeArrived = false;
        return kf;
    }

    void requestIdrFrame() override;

    bool isConnected() const override { return m_Connected; }

    IMediaEngine* mediaEngine() const override { return m_Shim; }

    /// Enable bidirectional text clipboard sync. Only called with true when
    /// the streamed host is this machine (the backend clipboard IS the host
    /// clipboard) — see ClipboardBridge. Must be called before the relay is
    /// moved to its dedicated thread (Session does, right after creation).
    void setClipboardEnabled(bool enabled);

    /// Stop answering every gap with a keyframe request, and let the picture
    /// repair itself instead.
    ///
    /// Only ever true when BOTH hold: the encoder really is intra-refreshing
    /// (so there is a repair to wait for), and the client said it will keep
    /// decoding through the damage (so the repair is allowed to happen). Either
    /// missing and this stays false, which is exactly today's behaviour.
    ///
    /// Set once before the relay moves to its own thread, like the flags above.
    void setRideOutLoss(bool enabled) { m_RideOutLoss = enabled; }

private:
    /// Both halves of the bargain, asked at the moment a gap happens.
    ///
    /// The engine's half has to be a live question: the relay is built before
    /// the engine starts, so anything cached at construction would answer "no
    /// intra-refresh" for the whole session. Defined in the .cpp because the
    /// engine is only forward-declared here.
    bool ridingOutLoss() const;

public:
    // Signals inherited from RelayBase: signalingSdpReady, signalingIceCandidate,
    // dataChannelsOpen, sessionEnded.

private slots:
    void onVideoFrame(const QByteArray& data, int frameType, int frameNumber,
                      qint64 presentationTimeUs);
    void onAudioSample(const QByteArray& data);
    void onShimConnectionTerminated(int errorCode);

private:
    // The video path proper, shared by the queued slot above and the native
    // engine's direct sink. `data` may be a BORROWED buffer
    // (QByteArray::fromRawData over the encoder's output): valid until this
    // returns, never to be kept — the one place that keeps a frame (the
    // buffered keyframe) copies explicitly.
    void handleVideoFrame(const QByteArray& data, bool isKeyframe, int frameNumber,
                          qint64 presentationTimeUs);

    // Best-effort exit notice ({"type": ...}) on the input DC before stop().
    void sendExitNotice(const char* type);

    void setupPeerConnection(const rtc::Configuration& config);
    void createDataChannels();
    void onInputMessage(const std::string& message);
    void handleKeyEvent(const std::string& type, const std::string& body);
    void handleMouseMove(const std::string& body);
    void handleMouseButton(const std::string& body);
    void handleMouseScroll(const std::string& body);

    // Fragmentation helpers — sends data in chunks over a DataChannel.
    // Header:
    // [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4][backend_ts:4]
    // backend_ts: monotonic millisecond timestamp (mod 2^32) taken at send time,
    // used by the frontend to compute end-to-end latency.
    // Max payload per chunk: kMaxPayloadSize (stays under SCTP 16KB fragment limit).
    static constexpr int kFragHeaderSize = 17;
    static constexpr int kMaxPayloadSize = 16000;

    // Backpressure: if the SCTP send buffer exceeds this threshold, drop
    // incoming delta frames to prevent main-thread blocking on dc->send().
    // Keyframes always pass through. Must exceed the largest expected HEVC
    // keyframe (~165KB): a lower threshold made every keyframe trip the
    // backpressure drop on following deltas, re-arming m_AwaitingIdr and
    // looping IDR requests at the 300ms throttle (3-4 fps).
    static constexpr size_t kHighWatermark = 256 * 1024;

    // presentationTimeUs: the frame's own capture time (from the decode unit),
    // carried through the queued signal. -1 = unknown → fall back to the shim's
    // latest value. Never read the shim atomic for regular frames: a drained
    // burst would share one backendTs and defeat the frontend's ordering filter.
    //
    // frameNumber: the engine's own number for the frame, so the sender can
    // report when it left (IMediaEngine::frameSentSink). -1 = not a live frame
    // (a buffered keyframe replayed at DC open) — nothing is reported.
    //
    // In direct mode the chunks are built here, on the calling thread, from
    // `data` (which may be borrowed — see handleVideoFrame), and the sender
    // only sends. Otherwise the frame is queued whole and the sender cuts it.
    void sendFragmented(const QByteArray& data, bool isKeyframe,
                        std::shared_ptr<rtc::DataChannel>& dc, qint64 presentationTimeUs = -1,
                        int frameNumber = -1);

    // Send a previously buffered keyframe (arrived before Video DC was open).
    // Called from the Video DC onOpen callback (marshaled to the relay thread).
    void sendBufferedKeyframe();

    // Coalescing IDR throttle: all IDR requests (frontend + internal) go through
    // this method. Requests arriving within the adaptive cooldown of the last
    // effective request are absorbed to prevent LiRequestIdrFrame flooding.
    // Caller holds m_VideoMutex.
    void sendIdrRequestThrottled();

    IMediaEngine* m_Shim;

    // True when this relay is the native engine's direct frame sink:
    // handleVideoFrame then runs on the engine's capture thread over a borrowed
    // buffer, and sendFragmented builds the chunks itself. False for every
    // GameStream engine, whose frames keep arriving through the relay thread's
    // event loop exactly as before.
    bool m_DirectVideoSend = false;

    // Serializes the video path — onVideoFrame (capture thread in direct mode),
    // sendBufferedKeyframe / takeBufferedKeyframe / requestidr (relay thread),
    // stop() and the stats tick. Held for one frame's bookkeeping at most; the
    // sender thread never takes it, so a full SCTP buffer cannot stall a
    // holder. Everything from here to the ICE timer that is not atomic is
    // guarded by it.
    std::mutex m_VideoMutex;

    // Dedicated thread for DataChannel fragmentation + send (keeps the per-frame
    // memcpy + dc->send off the Qt main thread / HTTP event loop).
    std::unique_ptr<FrameSender> m_Sender;

    std::shared_ptr<rtc::PeerConnection> m_Pc;
    std::shared_ptr<rtc::DataChannel> m_VideoDc;
    // Audio is a native RTP Opus track (browser-decoded: jitter buffer + in-band
    // FEC + PLC) on the same PeerConnection as the video DataChannel — a lost
    // packet no longer head-of-line-blocks the audio (the periodic dropouts).
    std::shared_ptr<rtc::Track> m_AudioTrack;
    std::shared_ptr<rtc::DataChannel> m_InputDc;

    // Audio RTP timestamp (48 kHz Opus clock), advanced by samplesPerFrame per
    // packet for a smooth, jitter-free clock; serialized with track teardown.
    std::mutex m_AudioMutex;
    uint32_t m_AudioRtpTs = 0;

    std::atomic<bool> m_Connected{false};
    std::atomic<bool> m_Stopping{false};
    // Bidirectional clipboard sync (only when the streamed host is this
    // machine). Written once on the main thread before the relay moves to its
    // dedicated thread, read from relay/libdatachannel threads afterwards.
    bool m_ClipboardEnabled = false;
    /// See setRideOutLoss. Written once before the thread move, read on the
    /// relay thread afterwards.
    bool m_RideOutLoss = false;
    int m_FrameCount = 0;
    uint32_t m_FrameId = 0;      // Monotonic counter for VIDEO fragmentation headers
    uint32_t m_AudioFrameId = 0; // Separate counter for audio — audio must not
                                 // consume video frameIds (frontend gap detection
                                 // relies on contiguous video ids)

    // Backpressure counters (diagnostic logging)
    int m_DeltaDroppedCount = 0; // Delta frames dropped due to full SCTP buffer
    // Deltas dropped by the awaiting-IDR gate. This is the BULK of a stall:
    // one delta hits a full buffer, the gate closes, and every frame after it
    // is discarded here — at 60 fps, ~60 per second — while m_DeltaDroppedCount
    // barely moves (it only counts the deltas that reached the buffer check).
    // Uncounted, a 19s outage read as "21 dropped deltas" in the logs.
    int m_AwaitingIdrDropCount = 0;
    int m_KeyframeBackpressureWarnings = 0; // Keyframes sent while buffer was above watermark
    int m_BackpressureDropCount = 0;        // Frames dropped in current backpressure episode
    qint64 m_LastDropSnapshot = 0;          // Sum of all drop counters at last stats tick
    // Decode latency tracking (microseconds)
    std::atomic<int64_t> m_LastDecodeLatencyUs{0};

    // IDR coalescing: adaptive cooldown between effective LiRequestIdrFrame calls.
    // All IDR sources (frontend requestidr, backpressure) converge here.
    // Exponential backoff: while requests keep firing without a keyframe getting
    // through, the cooldown doubles (300 ms → 5 s). Each IDR is a large frame
    // that inflates the encoded bitrate exactly when the link is saturated, so
    // an IDR flood feeds the very congestion it tries to fix.
    static constexpr qint64 kIdrCooldownBaseMs = 300;
    static constexpr qint64 kIdrCooldownMaxMs = 5000;
    QElapsedTimer m_IdrCooldownTimer;            // Monotonic timer; invalid until first request
    qint64 m_IdrCooldownMs = kIdrCooldownBaseMs; // Current adaptive cooldown
    bool m_IdrOutstanding = false; // True from an effective request until a keyframe is sent

    // Awaiting IDR: true when a delta was dropped (backpressure or DC not ready).
    // All delta frames are dropped and IDR requested until a keyframe is sent.
    // Guarded by m_VideoMutex.
    bool m_AwaitingIdr = false;

    // Buffered keyframe: if the first IDR arrives before the Video DataChannel
    // is open, we save it here and send it as soon as the DC opens.
    // This prevents a rare black-screen race where the browser receives only
    // delta frames because it missed the initial IDR.
    //
    // Stale buffer detection: tracks whether a NEW keyframe was sent directly
    // (via sendFragmented) while the buffer was held. When the DC opens, Sunshine
    // may send a second IDR (with updated SPS/VUI) while the first is still
    // in the buffer. Sending both creates a race where the browser's decoder
    // configures with stale SPS/PSS, producing wrong colors (green image).
    // DELTA frames arriving do NOT make the buffer stale — they are useless
    // without a keyframe, so we must still send the buffered one.
    QByteArray m_BufferedKeyframe;
    qint64 m_BufferedKeyframePresUs = -1; // presentationTimeUs of the buffered keyframe
    bool m_HaveBufferedKeyframe = false;
    bool m_NewKeyframeArrived = false; // True if a new keyframe was sent directly while buffer held

    // ── HEVC VPS/SPS patching ──────────────────────────────────────────────
    // Applied once to the first HEVC keyframe.  Patches level_idc and
    // max_sub_layers to fix Chrome Windows black screen on decode.
    bool m_HevcPatched = false;

    // ── ICE timeout ──────────────────────────────────────────────────────────
    QTimer* m_IceCheckTimer = nullptr;

    // ── Stats timer (2s interval) ────────────────────────────────────────────
    QTimer* m_StatsTimer = nullptr;

private slots:
    void onIceCheckTimeout();
    void onStatsTimerTick();

signals:
    /// Emitted when ICE fails to reach Connected within 3s after setRemoteDescription.
    /// Used by SignalingServer to trigger WS fallback.
    void iceTimedOut();
};
