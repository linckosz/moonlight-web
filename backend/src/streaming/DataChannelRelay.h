#pragma once

#include <QObject>
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
struct Configuration;
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
class DataChannelRelay : public QObject
{
    Q_OBJECT

public:
    explicit DataChannelRelay(MoonlightShim* shim, QObject* parent = nullptr);
    ~DataChannelRelay();

    // PeerConnection access for SignalingServer
    std::shared_ptr<rtc::PeerConnection> peerConnection() const { return m_Pc; }

    // Prepare PeerConnection + DataChannels. Returns the local SDP description
    // once generated (via signalingSdpReady signal) or false on failure.
    // The caller (SignalingServer) waits for signalingSdpReady before sending
    // the offer to the browser.
    bool prepare(const rtc::Configuration& config, bool isInternet = false);

    // Feed remote SDP answer (from browser) back into the PeerConnection.
    bool setRemoteDescription(const std::string& sdp);

    // Feed remote ICE candidate (from browser) into the PeerConnection.
    bool addRemoteCandidate(const std::string& candidate, const std::string& mid);

    void stop();

    // Request an IDR frame from Sunshine.
    // Called when the browser signals that it needs a keyframe (no decoder config yet).
    void requestIdrFrame();

    // ── UPnP NAT traversal ──────────────────────────────────────────────────

    /// Set the public IP and port discovered via UPnP.
    /// When set, host candidates in SDP will be rewritten to use this public
    /// address instead of the private LAN address, enabling direct P2P
    /// connections from outside the local network.
    void setPublicAddress(const std::string& publicIP, uint16_t publicPort);

    /// Enable rewriting of host candidates to use the configured public address.
    void setForceHostCandidatePublic(bool force) { m_ForceHostPublic = force; }

    /// When UPnP is active, suppress IPv6 candidates so the browser is forced
    /// to use the IPv4 UPnP path (IPv6 often fails through residential NAT/firewall).
    void setSuppressIPv6Candidates(bool suppress) { m_SuppressIPv6 = suppress; }

    bool isConnected() const { return m_Connected; }

signals:
    // Emitted when the local SDP description (offer) is ready to send to browser.
    void signalingSdpReady(const std::string& sdp);
    // Emitted for each local ICE candidate to forward to browser.
    void signalingIceCandidate(const std::string& candidate, const std::string& mid);
    // All DataChannels are open and ready for data.
    void dataChannelsOpen();
    // Session ended (disconnect / error).
    void sessionEnded();

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
};
