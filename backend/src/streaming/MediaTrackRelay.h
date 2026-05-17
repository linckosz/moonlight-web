#pragma once

#include "RelayBase.h"
#include <QByteArray>
#include <QMutex>
#include <memory>
#include <atomic>
#include <string>
#include <functional>
#include <cstdint>
#include <cstddef>

namespace rtc {
class PeerConnection;
class DataChannel;
class Track;
struct Configuration;
}

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
    ~MediaTrackRelay();

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

signals:
    // Emitted when the local SDP description (offer) is ready to send to browser.
    void signalingSdpReady(const std::string& sdp);
    // Emitted for each local ICE candidate to forward to browser.
    void signalingIceCandidate(const std::string& candidate, const std::string& mid);
    // All channels/tracks are open and ready for data.
    void dataChannelsOpen();
    // Session ended (disconnect / error).
    void sessionEnded();

private slots:
    void onVideoFrame(const QByteArray& data, int frameType, int frameNumber);
    void onAudioSample(const QByteArray& data);
    void onShimConnectionTerminated(int errorCode);

private:
    void setupPeerConnection(const rtc::Configuration& config);
    void createTracksAndChannels();
    void onInputMessage(const std::string& message);

    // Audio fragmentation helpers — PCM16 via DataChannel (same format as DataChannelRelay)
    static constexpr int kFragHeaderSize = 17;
    static constexpr int kMaxPayloadSize = 14000;
    static constexpr size_t kHighWatermark = 128 * 1024;

    void sendAudioFragmented(const QByteArray& data,
                              std::shared_ptr<rtc::DataChannel>& dc);

    // Send a previously buffered keyframe (arrived before Video Track was ready).
    void sendBufferedKeyframe();

    MoonlightShim* m_Shim;

    std::shared_ptr<rtc::PeerConnection> m_Pc;
    std::shared_ptr<rtc::Track> m_VideoTrack;
    std::shared_ptr<rtc::DataChannel> m_AudioDc;
    std::shared_ptr<rtc::DataChannel> m_InputDc;

    std::atomic<bool> m_Connected{false};
    std::atomic<bool> m_Stopping{false};
    int m_FrameCount = 0;

    // RTP timestamp for video (90 kHz clock, incremented per frame)
    uint32_t m_RtpTimestamp = 0;

    // Buffered keyframe: if the first IDR arrives before the Video Track is open
    QByteArray m_BufferedKeyframe;
    bool m_HaveBufferedKeyframe = false;

    // ── UPnP NAT traversal ──────────────────────────────────────────────────
    std::string m_PublicIP;
    uint16_t m_PublicPort = 0;
    bool m_ForceHostPublic = false;
    bool m_SuppressIPv6 = false;
};
