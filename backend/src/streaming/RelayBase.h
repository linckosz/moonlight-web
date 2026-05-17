#pragma once

#include <QObject>
#include <memory>
#include <string>
#include <cstdint>

namespace rtc {
class PeerConnection;
struct Configuration;
}

/// Abstract base class for DataChannelRelay and MediaTrackRelay.
///
/// Defines the common interface used by SignalingServer and Session.
/// Both relay types share the same lifecycle, UPnP, and signaling signals.
class RelayBase : public QObject
{
    Q_OBJECT

public:
    explicit RelayBase(QObject* parent = nullptr) : QObject(parent) {}
    ~RelayBase() override = default;

    // ── Lifecycle ──────────────────────────────────────────────────────────────

    /// Prepare PeerConnection + DataChannels/tracks. Emits signalingSdpReady
    /// when the local SDP offer is generated.
    virtual bool prepare(const rtc::Configuration& config, bool isInternet = false) = 0;

    /// Feed remote SDP answer (from browser) back into the PeerConnection.
    virtual bool setRemoteDescription(const std::string& sdp) = 0;

    /// Feed remote ICE candidate (from browser) into the PeerConnection.
    virtual bool addRemoteCandidate(const std::string& candidate, const std::string& mid) = 0;

    /// Stop the relay, close all channels and the PeerConnection.
    virtual void stop() = 0;

    /// Request an IDR frame from Sunshine (keyframe).
    virtual void requestIdrFrame() = 0;

    // ── UPnP NAT traversal ────────────────────────────────────────────────────

    virtual void setPublicAddress(const std::string& publicIP, uint16_t publicPort) = 0;
    virtual void setForceHostCandidatePublic(bool force) = 0;
    virtual void setSuppressIPv6Candidates(bool suppress) = 0;

    // ── Status ────────────────────────────────────────────────────────────────

    virtual bool isConnected() const = 0;

signals:
    /// Local SDP description (offer) ready to send to the browser.
    void signalingSdpReady(const std::string& sdp);

    /// Local ICE candidate to forward to the browser.
    void signalingIceCandidate(const std::string& candidate, const std::string& mid);

    /// All DataChannels/tracks are open and ready for data.
    void dataChannelsOpen();

    /// Session ended (disconnect / error).
    void sessionEnded();
};
