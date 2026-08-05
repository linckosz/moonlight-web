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

#include "InputPolicy.h"

#include <QObject>
#include <memory>
#include <string>
#include <cstdint>

namespace rtc {
class PeerConnection;
struct Configuration;
} // namespace rtc

class MoonlightShim;

/// Abstract base class for DataChannelRelay and MediaTrackRelay.
///
/// Defines the common interface used by SignalingServer and Session.
/// Both relay types share the same lifecycle, UPnP, and signaling signals.
class RelayBase : public QObject
{
    Q_OBJECT

public:
    explicit RelayBase(QObject* parent = nullptr)
        : QObject(parent)
    {}
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

    /// Notify the browser that its session was taken over by another device,
    /// so it can show a graceful "session interrupted" exit before the channels
    /// close. Sent on the control channel just before stop(); implementations
    /// flush it synchronously, since the caller tears down immediately after.
    virtual void notifyClientTakenOver() {}

    /// Notify the browser that its device access was revoked by the admin,
    /// so it can show a graceful "access revoked" exit before the channels
    /// close. Same delivery guarantee as notifyClientTakenOver().
    virtual void notifyClientRevoked() {}

    /// Notify an invited player that the owner ended the shared session, so the
    /// player's page can explain it rather than showing a connection error.
    /// Same delivery guarantee as notifyClientTakenOver().
    virtual void notifyClientSessionEnded() {}

    /// Restrict what this session's client may send to the host. Set once,
    /// before the relay starts, from the share activation the player joined
    /// with; the owner's own sessions leave it unrestricted.
    void setInputPolicy(const InputMsg::Policy& policy) { m_InputPolicy = policy; }
    const InputMsg::Policy& inputPolicy() const { return m_InputPolicy; }

    /// Request an IDR frame from Sunshine (keyframe).
    virtual void requestIdrFrame() = 0;

    // ── UPnP NAT traversal ────────────────────────────────────────────────────

    virtual void setPublicAddress(const std::string& publicIP, uint16_t publicPort) = 0;
    virtual void setForceHostCandidatePublic(bool force) = 0;
    virtual void setSuppressIPv6Candidates(bool suppress) = 0;
    // When host candidates are rewritten to the public IP, also advertise the
    // original private LAN candidate — but ONLY for a client that is itself on
    // the LAN (loopback/RFC1918, incl. NAT-hairpinned access via the public
    // URL), so the private IP is never leaked to internet peers. Lets a same-LAN
    // client connect directly when the router doesn't hairpin UDP.
    virtual void setEmitLanHostCandidate(bool enable) = 0;

    // ── Status ────────────────────────────────────────────────────────────────

    virtual bool isConnected() const = 0;

    /// Access the MoonlightShim for explicit stopConnection() before cleanup.
    virtual MoonlightShim* moonlightShim() const = 0;

signals:
    /// Local SDP description (offer) ready to send to the browser.
    void signalingSdpReady(const std::string& sdp);

    /// Local ICE candidate to forward to the browser.
    void signalingIceCandidate(const std::string& candidate, const std::string& mid);

    /// All DataChannels/tracks are open and ready for data.
    void dataChannelsOpen();

    /// Session ended (disconnect / error).
    void sessionEnded();

protected:
    /// Written once on the main thread before the relay moves to its own
    /// thread, read-only afterwards — same lifetime discipline as
    /// m_ClipboardEnabled.
    InputMsg::Policy m_InputPolicy;
};
