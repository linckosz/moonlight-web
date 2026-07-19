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
#include <QByteArray>
#include <QMutex>
#include <QTimer>
#include <memory>
#include <atomic>
#include <string>
#include <functional>
#include <cstdint>
#include <cstddef>
#include <chrono>
#include <mutex>

namespace rtc {
class PeerConnection;
class DataChannel;
class Track;
struct Configuration;
} // namespace rtc

class MoonlightShim;

// WebRTC Media Track relay — alternative to DataChannelRelay using RTP media tracks.
//
// Architecture:
//   - 1 rtc::Track for video (H.264 over RTP, hardware-decoded by browser)
//   - 1 rtc::DataChannel for audio (PCM16, same as DataChannelRelay — hybrid mode)
//   - 1 rtc::DataChannel for input (JSON, bidirectionnel)
//
// The H.264 RTP packetizer (H264RtpPacketizer with LongStartSequence separator)
// handles Annex B -> RTP fragmentation automatically.
// Audio and input remain on DataChannels for v1 (no Opus encoder dependency).
class MediaTrackRelay : public RelayBase
{
    Q_OBJECT

public:
    explicit MediaTrackRelay(MoonlightShim* shim, QObject* parent = nullptr);
    ~MediaTrackRelay() override;

    // PeerConnection access (not part of RelayBase interface)
    std::shared_ptr<rtc::PeerConnection> peerConnection() const { return m_Pc; }

    // ── RelayBase interface ─────────────────────────────────────────────────

    bool prepare(const rtc::Configuration& config, bool isInternet = false) override;

    bool setRemoteDescription(const std::string& sdp) override;

    bool addRemoteCandidate(const std::string& candidate, const std::string& mid) override;

    void stop() override;

    void notifyClientTakenOver() override;

    void notifyClientRevoked() override;

    void requestIdrFrame() override;

    // UPnP NAT traversal
    void setPublicAddress(const std::string& publicIP, uint16_t publicPort) override;
    void setForceHostCandidatePublic(bool force) override { m_ForceHostPublic = force; }
    void setSuppressIPv6Candidates(bool suppress) override { m_SuppressIPv6 = suppress; }
    void setEmitLanHostCandidate(bool enable) override { m_EmitLanCandidate = enable; }

    bool isConnected() const override { return m_Connected; }

    MoonlightShim* moonlightShim() const override { return m_Shim; }

    /// Enable bidirectional text clipboard sync. Only called with true when
    /// the streamed host is this machine (the backend clipboard IS the host
    /// clipboard) — see ClipboardBridge. Must be called before the relay is
    /// moved to its dedicated thread (Session does, right after creation).
    void setClipboardEnabled(bool enabled);

    // Signals inherited from RelayBase: signalingSdpReady, signalingIceCandidate,
    // dataChannelsOpen, sessionEnded.

signals:
    // ICE connection did not reach Connected within 3s after setRemoteDescription().
    // In auto mode this triggers fallback to the next transport; in explicit mode
    // it triggers the existing WS fallback inside SignalingServer.
    void iceTimedOut();

private slots:
    void onVideoFrame(const QByteArray& data, int frameType, int frameNumber);
    void onAudioSample(const QByteArray& data);
    void onShimConnectionTerminated(int errorCode);
    void onIceCheckTimeout();

private:
    // Best-effort exit notice ({"type": ...}) on the input DC before stop().
    void sendExitNotice(const char* type);

    void setupPeerConnection(const rtc::Configuration& config);
    void createTracksAndChannels();
    void onInputMessage(const std::string& message);

    // Send a previously buffered keyframe (arrived before Video Track was ready).
    void sendBufferedKeyframe();

    // Compute the 90 kHz RTP timestamp from real frame arrival time, so pacing
    // is correct for any frame rate (30/60/90/120...) and reflects real jitter.
    // Main-thread only (called from onVideoFrame / sendBufferedKeyframe).
    uint32_t computeRtpTimestamp();

    // IDR request throttle with exponential backoff (300 ms → 5 s while requests
    // keep firing without a keyframe getting through). Browser PLI storms during
    // sustained loss would otherwise flood Sunshine with IDR requests, inflating
    // the encoded bitrate exactly when the link is saturated. Thread-safe:
    // callers run on the libdatachannel PLI thread, the Qt main thread (input
    // messages) and the session thread.
    void sendIdrRequestThrottled();
    static constexpr int64_t kIdrCooldownBaseMs = 300;
    static constexpr int64_t kIdrCooldownMaxMs = 5000;
    std::atomic<int64_t> m_IdrCooldownMs{kIdrCooldownBaseMs};
    std::atomic<int64_t> m_LastIdrRequestMs{0}; // steady_clock ms of last effective request
    std::atomic<bool> m_IdrOutstanding{false};  // True until the next keyframe is sent

    MoonlightShim* m_Shim;

    std::shared_ptr<rtc::PeerConnection> m_Pc;
    std::shared_ptr<rtc::Track> m_VideoTrack;
    // Audio is a native RTP Opus track (browser-decoded: jitter buffer + in-band
    // FEC + PLC), NOT a DataChannel — a lost packet no longer head-of-line-blocks
    // the audio stream, eliminating the periodic micro-dropouts.
    std::shared_ptr<rtc::Track> m_AudioTrack;
    std::shared_ptr<rtc::DataChannel> m_InputDc;

    std::atomic<bool> m_Connected{false};
    std::atomic<bool> m_Stopping{false};
    // Bidirectional clipboard sync (only when the streamed host is this
    // machine). Written once on the main thread before the relay moves to its
    // dedicated thread, read from relay/libdatachannel threads afterwards.
    bool m_ClipboardEnabled = false;
    int m_FrameCount = 0;

    // P2-B: when true, onVideoFrame runs on the capture thread (DirectConnection)
    // instead of being marshaled to the Qt main thread. Set from env at construction.
    bool m_DirectVideoSend = true;
    // Serializes the video send path (onVideoFrame / sendBufferedKeyframe) with
    // track teardown in stop(). Required because onVideoFrame may run on the
    // capture thread in direct mode.
    std::mutex m_VideoMutex;

    // RTP timestamp base: captured on the first sent frame; subsequent
    // timestamps are derived from elapsed wall-clock time (see computeRtpTimestamp).
    std::chrono::steady_clock::time_point m_FirstFrameTime;
    bool m_HaveFirstFrameTime = false;

    // Audio RTP timestamp (48 kHz Opus clock), advanced by samplesPerFrame per
    // packet for a smooth, jitter-free clock; serialized with track teardown.
    std::mutex m_AudioMutex;
    uint32_t m_AudioRtpTs = 0;

    // Buffered keyframe: if the first IDR arrives before the Video Track is open
    QByteArray m_BufferedKeyframe;
    bool m_HaveBufferedKeyframe = false;
    // True when a delta frame was dropped after the buffered keyframe (track
    // still closed): the buffer is stale — sending it followed by live deltas
    // yields an undecodable stream with no RTP gap (no NACK/PLI recovery).
    bool m_DeltaAfterBufferedKeyframe = false;
    // Delta gate: no delta is sent on the track before a keyframe has been
    // sent (session start, or after a worker-side delta drop broke the
    // reference chain). Guarded by m_VideoMutex.
    bool m_SentKeyframeOnTrack = false;

    // ── UPnP NAT traversal ──────────────────────────────────────────────────
    std::string m_PublicIP;
    uint16_t m_PublicPort = 0;
    bool m_ForceHostPublic = false;
    bool m_SuppressIPv6 = false;
    bool m_EmitLanCandidate = false; // Also emit the private host candidate (LAN client only)

    // ── ICE connection timeout ──────────────────────────────────────────────
    // Starts on setRemoteDescription(), cancelled on PC Connected or stop().
    QTimer* m_IceCheckTimer = nullptr;

    // ── Stats timer (2s interval) ────────────────────────────────────────────
    QTimer* m_StatsTimer = nullptr;

private slots:
    void onStatsTimerTick();
};
