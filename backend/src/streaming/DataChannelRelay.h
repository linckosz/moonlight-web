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

class MoonlightShim;

// WebRTC DataChannel relay that replaces StreamRelay.
// Uses libdatachannel to establish 3 DataChannels (video, audio, input)
// and forwards video/audio from MoonlightShim + input from browser.
//
// Thread safety:
// - MoonlightShim signals arrive on Qt main thread (AutoConnection).
//   sendMessage() is called from the main thread -> libdatachannel's DC send
//   is internally thread-safe.
// - libdatachannel callbacks (onMessage for input) fire from internal threads.
//   We marshal input back to the main thread via QMetaObject::invokeMethod.
class DataChannelRelay : public RelayBase
{
    Q_OBJECT

public:
    explicit DataChannelRelay(MoonlightShim* shim, QObject* parent = nullptr);
    ~DataChannelRelay() override;

    // PeerConnection access (not part of RelayBase interface)
    std::shared_ptr<rtc::PeerConnection> peerConnection() const { return m_Pc; }

    // ── RelayBase interface ─────────────────────────────────────────────────

    bool prepare(const rtc::Configuration& config, bool isInternet = false) override;

    bool setRemoteDescription(const std::string& sdp) override;

    bool addRemoteCandidate(const std::string& candidate, const std::string& mid) override;

    void stop() override;

    void notifyClientTakenOver() override;

    /// Retrieve and clear the buffered keyframe (if any).
    /// Used by SignalingServer before stop() to preserve the keyframe for
    /// WebSocket fallback — without this, the fallback starts with delta
    /// frames and the browser's VideoDecoder can never configure.
    QByteArray takeBufferedKeyframe()
    {
        QByteArray kf = m_BufferedKeyframe;
        m_BufferedKeyframe.clear();
        m_HaveBufferedKeyframe = false;
        m_NewKeyframeArrived = false;
        return kf;
    }

    void requestIdrFrame() override;

    // UPnP NAT traversal
    void setPublicAddress(const std::string& publicIP, uint16_t publicPort) override;
    void setForceHostCandidatePublic(bool force) override { m_ForceHostPublic = force; }
    void setSuppressIPv6Candidates(bool suppress) override { m_SuppressIPv6 = suppress; }

    bool isConnected() const override { return m_Connected; }

    MoonlightShim* moonlightShim() const override { return m_Shim; }

    // Signals inherited from RelayBase: signalingSdpReady, signalingIceCandidate,
    // dataChannelsOpen, sessionEnded.

private slots:
    void onVideoFrame(const QByteArray& data, int frameType, int frameNumber);
    void onAudioSample(const QByteArray& data);
    void onShimConnectionTerminated(int errorCode);

private:
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

    void sendFragmented(const QByteArray& data, bool isKeyframe,
                        std::shared_ptr<rtc::DataChannel>& dc);

    // Send a previously buffered keyframe (arrived before Video DC was open).
    // Called from the Video DC onOpen callback (marshaled to main thread).
    void sendBufferedKeyframe();

    // Coalescing IDR throttle: all IDR requests (frontend + internal) go through
    // this method. Requests arriving within the adaptive cooldown of the last
    // effective request are absorbed to prevent LiRequestIdrFrame flooding.
    void sendIdrRequestThrottled();

    MoonlightShim* m_Shim;

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
    int m_FrameCount = 0;
    uint32_t m_FrameId = 0;      // Monotonic counter for VIDEO fragmentation headers
    uint32_t m_AudioFrameId = 0; // Separate counter for audio — audio must not
                                 // consume video frameIds (frontend gap detection
                                 // relies on contiguous video ids)

    // Backpressure counters (diagnostic logging)
    int m_DeltaDroppedCount = 0;            // Delta frames dropped due to full SCTP buffer
    int m_KeyframeBackpressureWarnings = 0; // Keyframes sent while buffer was above watermark
    int m_BackpressureDropCount = 0;        // Frames dropped in current backpressure episode
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
    QElapsedTimer m_IdrCooldownTimer;             // Monotonic timer; invalid until first request
    qint64 m_IdrCooldownMs = kIdrCooldownBaseMs;  // Current adaptive cooldown
    bool m_IdrOutstanding = false; // True from an effective request until a keyframe is sent

    // Awaiting IDR: true when a delta was dropped (backpressure or DC not ready).
    // All delta frames are dropped and IDR requested until a keyframe is sent.
    // Plain bool is safe — all accesses are on the Qt main thread.
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
    bool m_HaveBufferedKeyframe = false;
    bool m_NewKeyframeArrived = false; // True if a new keyframe was sent directly while buffer held

    // ── UPnP NAT traversal ──────────────────────────────────────────────────
    std::string m_PublicIP;    // Public IP discovered via UPnP (or empty)
    uint16_t m_PublicPort = 0; // Mapped port from UPnP (0 = not mapped)
    bool m_ForceHostPublic = false;
    bool m_SuppressIPv6 = false; // Suppress IPv6 candidates when UPnP active

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
