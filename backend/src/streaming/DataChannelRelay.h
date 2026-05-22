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

namespace rtc {
class DataChannel;
}

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
    ~DataChannelRelay();

    // PeerConnection access (not part of RelayBase interface)
    std::shared_ptr<rtc::PeerConnection> peerConnection() const { return m_Pc; }

    // ── RelayBase interface ─────────────────────────────────────────────────

    bool prepare(const rtc::Configuration& config, bool isInternet = false) override;

    bool setRemoteDescription(const std::string& sdp) override;

    bool addRemoteCandidate(const std::string& candidate, const std::string& mid) override;

    void stop() override;

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
    // Header: [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4][backend_ts:4]
    // backend_ts: monotonic millisecond timestamp (mod 2^32) taken at send time,
    // used by the frontend to compute end-to-end latency.
    // Max payload per chunk: kMaxPayloadSize (under SCTP 16KB limit).
    static constexpr int kFragHeaderSize = 17;
    static constexpr int kMaxPayloadSize = 14000;

    // Backpressure: if the SCTP send buffer exceeds this threshold, drop
    // incoming delta frames to prevent main-thread blocking on dc->send().
    // Keyframes always pass through.  The SCTP association's send buffer is
    // typically ~256 KB; we set the watermark at 128 KB so there is room
    // for keyframe chunks without blocking the main thread.
    static constexpr size_t kHighWatermark = 128 * 1024;

    void sendFragmented(const QByteArray& data, bool isKeyframe,
                        std::shared_ptr<rtc::DataChannel>& dc);

    // Send a previously buffered keyframe (arrived before Video DC was open).
    // Called from the Video DC onOpen callback (marshaled to main thread).
    void sendBufferedKeyframe();

    MoonlightShim* m_Shim;

    std::shared_ptr<rtc::PeerConnection> m_Pc;
    std::shared_ptr<rtc::DataChannel> m_VideoDc;
    std::shared_ptr<rtc::DataChannel> m_AudioDc;
    std::shared_ptr<rtc::DataChannel> m_InputDc;

    std::atomic<bool> m_Connected{false};
    std::atomic<bool> m_Stopping{false};
    int m_FrameCount = 0;
    uint32_t m_FrameId = 0;  // Monotonic counter for fragmentation headers

    // Backpressure counters (diagnostic logging)
    int m_DeltaDroppedCount = 0;           // Delta frames dropped due to full SCTP buffer
    int m_KeyframeBackpressureWarnings = 0; // Keyframes sent while buffer was above watermark

    // Decode latency tracking (microseconds)
    std::atomic<int64_t> m_LastDecodeLatencyUs{0};

    // Buffered keyframe: if the first IDR arrives before the Video DataChannel
    // is open, we save it here and send it as soon as the DC opens.
    // This prevents a rare black-screen race where the browser receives only
    // delta frames because it missed the initial IDR.
    QByteArray m_BufferedKeyframe;
    bool m_HaveBufferedKeyframe = false;

    // ── UPnP NAT traversal ──────────────────────────────────────────────────
    std::string m_PublicIP;      // Public IP discovered via UPnP (or empty)
    uint16_t m_PublicPort = 0;   // Mapped port from UPnP (0 = not mapped)
    bool m_ForceHostPublic = false;
    bool m_SuppressIPv6 = false; // Suppress IPv6 candidates when UPnP active

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
