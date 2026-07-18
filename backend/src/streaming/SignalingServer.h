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
    bool isPrivateAddress(const QString& ip) const;

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
    void sendIceConfig();

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

    /// Whether the streaming client is on our LAN (loopback/RFC1918, incl. a
    /// NAT-hairpinned client on the public URL). When true and UPnP rewrites host
    /// candidates to the public IP, the relay also advertises the private LAN
    /// candidate so the local client can connect directly. Never set for internet
    /// clients — avoids leaking the LAN IP.
    void setClientIsLocal(bool local) { m_ClientIsLocal = local; }

    /// The external port mapped via UPnP (0 = not mapped).
    uint16_t upnpMappedPort() const { return m_UpnpMappedPort; }

    /// The public IP discovered via UPnP (empty if not available).
    QString upnpPublicIP() const { return m_UpnpPublicIP; }

private:
    /// Discover IGD and add a UDP port mapping via UPnP.
    /// Called async (QTimer::singleShot(0)) so it doesn't block start().
    bool setupUPnP();

    /// Remove the UPnP port mapping and clean up resources.
    void cleanupUPnP();

    /// Build the rtc::Configuration with ICE servers and port range.
    /// ICE-TCP is always enabled as fallback. STUN is always present in
    /// Internet mode. UPnP sets a fixed port range and rewrites host candidates.
    static rtc::Configuration buildIceConfig(bool isInternet, uint16_t upnpMappedPort,
                                             const QString& stunServerUrl,
                                             bool forceIceTcp = false);

    bool m_UseUPnP = true;
    bool m_ClientIsLocal = false; // Streaming client is on our LAN (see setClientIsLocal)
    UPNPClient* m_Upnp = nullptr;
    uint16_t m_UpnpMappedPort = 0;
    QString m_UpnpPublicIP;
    QTimer* m_UpnpRenewTimer = nullptr;

    /// Default UDP port for UPnP mapping (must match libdatachannel port range).
    static constexpr uint16_t kUpnpPort = 48010;
    /// Max number of port+1 fallback attempts if the default port is taken.
    static constexpr int kUpnpMaxPortAttempts = 5;
    /// Lease duration in seconds (1 hour). Renew timer fires at half this interval.
    static constexpr uint32_t kUpnpLeaseDurationSec = 3600;
    /// Renew timer interval (ms): every 30 minutes.
    static constexpr int kUpnpRenewIntervalMs = 1800000;
};
