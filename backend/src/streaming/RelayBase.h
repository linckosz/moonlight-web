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
class Candidate;
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
    // ── ICE connectivity deadline ───────────────────────────────────────────
    //
    /// How long ICE may take to reach Connected after setRemoteDescription()
    /// before the transport is declared dead and the fallback chain moves on.
    ///
    /// Sized from where the peer is. On loopback/LAN a candidate pair is
    /// checked in a couple of round trips. A public peer has to punch through
    /// two NATs, and its checks carry STUN's retransmit backoff — 3s expired
    /// mid-handshake and killed sessions whose video was already flowing,
    /// leaving the host to tear down a stream it had just started (and, on
    /// Sunshine, to stop answering /launch for minutes afterwards).
    ///
    /// Public because the browser runs the very same deadline on its own side
    /// and must be told which one applies: whichever end fires first ends the
    /// attempt, so a host that waits 10s while the browser gives up at 3s has
    /// gained nothing. SignalingServer ships the value in its ice-config.
    static constexpr int kIceTimeoutLocalMs = 3000;
    static constexpr int kIceTimeoutInternetMs = 10000;

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

    void setPublicAddress(const std::string& publicIP, uint16_t publicPort);
    void setForceHostCandidatePublic(bool force) { m_ForceHostPublic = force; }
    void setSuppressIPv6Candidates(bool suppress) { m_SuppressIPv6 = suppress; }
    // When host candidates are rewritten to the public IP, also advertise the
    // original internal candidate — but ONLY for a peer inside a network we
    // control (loopback/RFC1918/mesh VPN, incl. NAT-hairpinned access via the
    // public URL), so the internal topology is never leaked to a public peer.
    // Lets a same-LAN client connect directly when the router doesn't hairpin
    // UDP, and keeps a tunnel peer on the tunnel.
    void setEmitLanHostCandidate(bool enable) { m_EmitLanCandidate = enable; }

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
    /// Apply the UPnP host-candidate rewrite and the IPv6 suppression, then
    /// emit signalingIceCandidate. Both relays call this from their
    /// onLocalCandidate handler; the logic used to be duplicated verbatim in
    /// each and had already drifted in its logging.
    ///
    /// @param logTag prefix for this relay's log lines, e.g. "[MediaTrackRelay]".
    void emitLocalCandidate(const rtc::Candidate& candidate, const char* logTag);

    /// Log the candidate pair ICE actually nominated, once the PeerConnection
    /// reaches Connected. Nothing else in the logs says which path the media
    /// took: with the UPnP rewrite in emitLocalCandidate() above, a same-LAN
    /// peer and a peer on the far side of the internet are both offered the
    /// public address, so a session hairpinning through the router looks
    /// exactly like a direct one until its throughput collapses (2026-08-30:
    /// a 19s SCTP stall that could not be attributed either way).
    ///
    /// @param logTag prefix for this relay's log lines, e.g. "[MediaTrackRelay]".
    void logSelectedCandidatePair(rtc::PeerConnection& pc, const char* logTag);

    /// Written once on the main thread before the relay moves to its own
    /// thread, read-only afterwards — same lifetime discipline as
    /// m_ClipboardEnabled.
    InputMsg::Policy m_InputPolicy;

    // ── ICE connectivity deadline ───────────────────────────────────────────
    /// Set by prepare() from the peer classification; read by
    /// setRemoteDescription() when it arms the timer.
    int m_IceTimeoutMs = kIceTimeoutLocalMs;

    // ── UPnP NAT traversal ──────────────────────────────────────────────────
    std::string m_PublicIP;    ///< Public IP discovered via UPnP (or empty)
    uint16_t m_PublicPort = 0; ///< Mapped port from UPnP (0 = not mapped)
    bool m_ForceHostPublic = false;
    ///< Withhold NON-GLOBAL IPv6 candidates (ULA, mesh VPN, link-local) when
    ///< UPnP is active. A global IPv6 address goes out: it is public anyway, and
    ///< it is the only candidate that lets a same-LAN peer avoid hairpinning
    ///< through the router. See emitLocalCandidate().
    bool m_SuppressIPv6 = false;
    bool m_EmitLanCandidate = false; ///< Also emit the internal host candidate
};
