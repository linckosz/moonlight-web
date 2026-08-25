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

#include "server/NetClassify.h"

#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QSslConfiguration>
#include <QTimer>
#include <memory>
#include <atomic>

namespace rtc {
struct Configuration;
}

class RelayBase;
class UPNPClient;
class MoonlightShim;

// Minimal WebSocket server for WebRTC signaling only.
// Exchanges SDP offer/answer and ICE candidates between the browser
// and the DataChannelRelay's libdatachannel PeerConnection.
//
// Protocol (JSON over WebSocket):
//   browser -> server: {"type":"sdp","sdp":"..."}          // SDP answer (after receiving offer
//   from us) server -> browser: {"type":"sdp","sdp":"..."}          // SDP offer browser -> server:
//   {"type":"ice","candidate":"...","mid":"0"} server -> browser:
//   {"type":"ice","candidate":"...","mid":"0"}
class SignalingServer : public QObject
{
    Q_OBJECT

public:
    SignalingServer(RelayBase* relay, quint16 wsPort, const QString& serverHost = "localhost",
                    QObject* parent = nullptr);
    ~SignalingServer() override;

    bool start();
    void stop();

    void setServerHost(const QString& host) { m_ServerHost = host; }
    void setHttpsPort(quint16 port) { m_HttpsPort = port; }

    /// Proxy path prefix for the signaling WebSocket ("/ws" default, "/ws1"
    /// for the second concurrent stream slot). Must match HttpServer's
    /// path->port routing.
    void setWsPath(const QString& path) { m_WsPath = path; }
    quint16 port() const { return m_WsPort; }

    /// Returns the WebSocket URL for browser connections.
    /// If an override URL is set (e.g. for a public tunnel), returns that instead.
    QString wsUrl() const;

    /// Override the WS URL (used when a public tunnel provides a WSS endpoint).
    /// The browser will connect to this URL instead of the local ws://... one.
    void setOverrideWsUrl(const QString& url) { m_OverrideWsUrl = url; }

    /// Set the MoonlightShim instance for WS fallback mode.
    /// When ICE times out, video/audio data is sent over the signaling WebSocket
    /// instead of WebRTC DataChannels, using the same fragmentation format.
    void setMoonlightShim(MoonlightShim* shim) { m_Shim = shim; }

    /// Set the STUN server URL to use for ICE configuration.
    /// Default: "stun:stun.l.google.com:19302"
    void setStunServer(const QString& url) { m_StunServerUrl = url; }

    /// WebRTC/UPnP media UDP port for this stream slot. Each concurrent slot
    /// binds a distinct port (base + slot) so simultaneous streams never
    /// collide on it, and UPnP maps exactly that port. Defaults to the base
    /// (kUpnpPort) for the single-stream path.
    void setMediaPort(quint16 port) { m_MediaPort = port; }

    /// Enable/disable ICE-TCP candidates.
    /// When true, ICE-TCP is enabled (UDP + TCP candidates).
    /// When false (default), only UDP candidates are used.
    void setEnableIceTcp(bool enable) { m_ForceIceTcp = enable; }

    /// Control whether WS fallback is allowed on ICE timeout.
    /// In auto mode (allow=false): iceTimedOut → sessionEnded() so the
    /// auto fallback chain can try the next transport.
    /// In explicit mode (allow=true, default): iceTimedOut → startWsFallback().
    void setAllowWsFallback(bool allow) { m_AllowWsFallback = allow; }

signals:
    void clientConnected();
    void clientDisconnected();
    void sessionEnded();

private slots:
    void onNewWsConnection();
    void onWsTextMessage(const QString& message);
    void onWsDisconnected();

    // DataChannelRelay signals
    void onLocalSdp(const std::string& sdp);
    void onLocalIceCandidate(const std::string& candidate, const std::string& mid);
    void onDataChannelsOpen();

    // ICE timeout → WS fallback
    void onRelayIceTimedOut();

private:
    // ── WS Fallback mode ────────────────────────────────────────────────────
    // When ICE negotiation times out (UDP blocked by corporate firewall),
    // video/audio data is forwarded over the existing signaling WebSocket
    // instead of WebRTC DataChannels. This provides a TCP-based fallback
    // path through restrictive networks.

    /// Start WS fallback — triggered by DataChannelRelay::iceTimedOut().
    /// Sends {type:"fallback-ws"} to the browser, then routes MoonlightShim
    /// video/audio signals through the signaling WebSocket as binary frames.
    void startWsFallback();

    /// Send ICE server configuration to the browser as {type:"ice-config"}.
    /// Called in onNewWsConnection() so the browser knows which STUN server
    /// to use for its RTCPeerConnection, overriding the hardcoded default.
    /// Send the browser its ICE servers and the ICE deadline it must run.
    /// @param iceTimeoutMs the deadline both ends apply, so the browser does
    ///        not abandon an attempt the host is still nursing.
    void sendIceConfig(int iceTimeoutMs);

    /// Handle text messages received on the WS in fallback mode.
    /// These are input commands (keydown, mousemove, etc.) from the browser.
    void handleWsFallbackInput(const QString& message);

    /// Forward a MoonlightShim video frame to the browser as a binary WS frame.
    /// Uses the same fragmentation format as DataChannelRelay, with a 1-byte
    /// channel prefix (0x01=video, 0x02=audio) before the frag header.
    void forwardVideoViaWs(const QByteArray& data, int frameType, int frameNumber);

    /// Forward a MoonlightShim audio sample to the browser as a binary WS frame.
    void forwardAudioViaWs(const QByteArray& data);

    bool m_WsFallbackActive = false;
    bool m_AllowWsFallback = true; ///< Default: WS fallback allowed. Auto mode sets false.
    bool m_ShimConnected = false;  ///< MoonlightShim signals connected for fallback

    /// ── Members ─────────────────────────────────────────────────────────────

    RelayBase* m_Relay;
    MoonlightShim* m_Shim = nullptr;

    QWebSocketServer* m_WsServer = nullptr;
    QWebSocket* m_WsClient = nullptr;
    quint16 m_WsPort = 0;
    quint16 m_HttpsPort = 443;
    QString m_WsPath = QStringLiteral("/ws");
    QString m_ServerHost;
    bool m_Running = false;
    std::atomic<bool> m_Stopping{false};
    bool m_SignalingComplete = false;
    bool m_DataChannelsOpen = false;

    int m_ClientPort = 0;

    /// If non-empty, wsUrl() returns this URL instead of constructing one.
    QString m_OverrideWsUrl;

    /// STUN server URL for ICE configuration. Default: Google public STUN.
    QString m_StunServerUrl = QStringLiteral("stun:stun.l.google.com:19302");

    /// Force ICE-TCP candidates (true = UDP + TCP, false = UDP only).
    bool m_ForceIceTcp = false;

    // ── UPnP NAT traversal ──────────────────────────────────────────────────

public:
    /// Enable/disable UPnP port mapping for NAT traversal.
    /// Called before start() to set the preference from settings.
    void setUseUPnP(bool enable) { m_UseUPnP = enable; }

    /// Where the streaming client actually sits, classified from the browser's
    /// own address by whoever accepted the /start request.
    ///
    /// It must be told: the signaling WebSocket reaches us through the local
    /// HTTP proxy, so this server only ever sees 127.0.0.1 and classifying the
    /// socket peer would call every client — a phone on 4G included — loopback.
    /// That is what stripped internet sessions of their STUN server and left
    /// them with a single UPnP candidate to connect through.
    ///
    /// Two decisions read it, and they are not the same question:
    ///  - isPrivateOrLoopback → do we need STUN? A mesh VPN peer says yes, so
    ///    its srflx address survives if the tunnel path fails.
    ///  - isTrustedPeer → may we show it our internal host candidate? A mesh
    ///    VPN peer says yes too; a public peer never does, or the LAN leaks.
    void setClientKind(NetClassify::Kind kind) { m_ClientKind = kind; }

    // ── MW-BIND-v1 (docs/design/pairing-signature.md) ──────────────────────
    //
    // Signaling will eventually be relayed by an introduction server we do not
    // want to have to trust. These bind the SDP fingerprints to keys that server
    // has never seen, so it cannot substitute its own, terminate DTLS itself and
    // inject input into the desktop.
    //
    // @param hostId       identifier of this host, inside both signatures so one
    //                     captured elsewhere cannot be replayed here.
    // @param hostKeyPem   this host's P-256 private key. Reaches the worker on
    //                     stdin, never on the command line.
    // @param browserSpki  public key of the browser this session is paired with;
    //                     empty when the pairing predates the mechanism, which
    //                     disables the checks (see onWsTextMessage for why that
    //                     is not a downgrade an attacker can reach).
    void setPairingIdentity(const QString& hostId, const QByteArray& hostKeyPem,
                            const QByteArray& browserSpki);

    /// The external port mapped via UPnP (0 = not mapped).
    uint16_t upnpMappedPort() const { return m_UpnpMappedPort; }

    /// The public IP discovered via UPnP (empty if not available).
    QString upnpPublicIP() const { return m_UpnpPublicIP; }

private:
    /// Discover IGD and add the UDP and TCP port mappings via UPnP.
    /// Called async (QTimer::singleShot(0)) so it doesn't block start().
    bool setupUPnP();

    /// Add or refresh one mapping, retrying without a lease if the router
    /// rejects the leased form. Uses m_UpnpLeaseSec, which it may latch to 0.
    bool addUpnpMapping(uint16_t port, const std::string& protocol);

    /// Remove the UPnP port mappings and clean up resources.
    void cleanupUPnP();

    /// Build the rtc::Configuration with ICE servers and port range.
    /// ICE-TCP is always enabled as fallback. STUN is always present in
    /// Internet mode. UPnP sets a fixed port range and rewrites host candidates.
    static rtc::Configuration buildIceConfig(bool isInternet, uint16_t upnpMappedPort,
                                             uint16_t mediaPort, const QString& stunServerUrl,
                                             bool forceIceTcp = false);

    // ── MW-BIND-v1 state ───────────────────────────────────────────────────
    /// Sign the pending SDP offer and hand it to the browser. Called once the
    /// browser's nonce has arrived, since the host's signature covers it.
    void sendSignedOffer();
    /// Refuse the connection and say why. Closes the WS without ever reaching
    /// setRemoteDescription, so no DTLS state exists when we give up.
    void abortSignaling(const QString& reason);

    /// How long the signed offer waits for the browser's hello before giving up.
    static constexpr int kHelloTimeoutMs = 15000;

    QString m_HostId;
    QByteArray m_HostKeyPem;
    QByteArray m_BrowserSpki;   // empty ⇒ pairing predates MW-BIND-v1
    QByteArray m_NonceBrowser;  // from the browser's hello; covered by our signature
    QByteArray m_NonceHost;     // ours; covered by the browser's signature
    QString m_LocalFingerprint; // fpH, extracted from our own offer
    std::string m_PendingOffer; // held back until the browser's nonce arrives
    bool m_HelloReceived = false;

    bool m_UseUPnP = true;
    /// Distinct WebRTC/UPnP media UDP port for this slot (base + slot). Keeps
    /// concurrent workers off each other's port; slot 0 keeps 48010. See
    /// setMediaPort().
    quint16 m_MediaPort = kUpnpPort;
    NetClassify::Kind m_ClientKind = NetClassify::Kind::Public; // see setClientKind
    UPNPClient* m_Upnp = nullptr;
    uint16_t m_UpnpMappedPort = 0;
    QString m_UpnpPublicIP;
    QTimer* m_UpnpRenewTimer = nullptr;
    /// TCP mapped alongside UDP? Best effort: losing it only costs the ICE-TCP
    /// transports, while UDP carries the vast majority of sessions.
    bool m_UpnpTcpMapped = false;
    /// Lease currently in effect. Latched to 0 (permanent) when the router
    /// rejects a leased mapping, or when renewal keeps failing.
    uint32_t m_UpnpLeaseSec = kUpnpLeaseDurationSec;
    /// Consecutive renewal failures; two is enough to stop trusting the lease.
    int m_UpnpRenewFailures = 0;

    /// Default port for UPnP mapping (must match libdatachannel port range).
    /// Mapped in both UDP and TCP: the ICE-TCP transports need an inbound TCP
    /// port to be reachable from the internet, since browsers only ever open
    /// ICE-TCP connections outbound.
    static constexpr uint16_t kUpnpPort = 48010;
    /// Lease duration in seconds (1 hour). Renew timer fires at half this interval.
    static constexpr uint32_t kUpnpLeaseDurationSec = 3600;
    /// Renew timer interval (ms): every 30 minutes.
    static constexpr int kUpnpRenewIntervalMs = 1800000;
};
