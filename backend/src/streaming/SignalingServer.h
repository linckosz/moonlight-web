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
class IMediaEngine;

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
    /// The path alone, for a browser that has no route to this machine and must
    /// resolve it against its own origin (see StreamSession::setTunnelArrival).
    QString wsPath() const { return m_WsPath; }
    quint16 port() const { return m_WsPort; }

    /// Returns the WebSocket URL for browser connections.
    /// If an override URL is set (e.g. for a public tunnel), returns that instead.
    QString wsUrl() const;

    /// Override the WS URL (used when a public tunnel provides a WSS endpoint).
    /// The browser will connect to this URL instead of the local ws://... one.
    void setOverrideWsUrl(const QString& url) { m_OverrideWsUrl = url; }

    /// Set the media engine used by WS fallback mode.
    /// When ICE times out, video/audio data is sent over the signaling WebSocket
    /// instead of WebRTC DataChannels, using the same fragmentation format.
    void setMediaEngine(IMediaEngine* engine) { m_Shim = engine; }

    /// Set the STUN server URL to use for ICE configuration.
    /// Default: "stun:stream.{MW_DOMAIN}:3478" (AppSettings::stunServer).
    void setStunServer(const QString& url) { m_StunServerUrl = url; }

    /// WebRTC media UDP port for this stream slot. Each concurrent slot binds
    /// a distinct port (base + slot) so simultaneous streams never collide on
    /// it; the router forwards the preset external port to it. Defaults to the
    /// base for the single-stream path.
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
    /// Put one candidate on the wire. Split out of onLocalIceCandidate so the
    /// flush after the offer takes the identical path.
    void sendIceCandidate(const std::string& candidate, const std::string& mid);
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
    /// Sends {type:"fallback-ws"} to the browser, then routes media engine
    /// video/audio signals through the signaling WebSocket as binary frames.
    void startWsFallback();

    /// Send ICE server configuration to the browser as {type:"ice-config"}.
    /// Called in onNewWsConnection() so the browser knows which STUN server
    /// to use for its RTCPeerConnection, overriding the hardcoded default.
    /// Send the browser its ICE servers and the ICE deadline it must run.
    /// @param iceTimeoutMs the deadline both ends apply, so the browser does
    ///        not abandon an attempt the host is still nursing.
    /// Push the browser's ICE servers. `needStun` follows the same rule
    /// buildIceConfig applies to our own side: a LAN peer is reached on host
    /// candidates alone, so it is told to use no STUN server at all rather than
    /// being handed one this side has already decided not to use.
    void sendIceConfig(bool needStun, int iceTimeoutMs);

    /// Handle text messages received on the WS in fallback mode.
    /// These are input commands (keydown, mousemove, etc.) from the browser.
    void handleWsFallbackInput(const QString& message);

    /// Forward a media engine video frame to the browser as a binary WS frame.
    /// Uses the same fragmentation format as DataChannelRelay, with a 1-byte
    /// channel prefix (0x01=video, 0x02=audio) before the frag header.
    void forwardVideoViaWs(const QByteArray& data, int frameType, int frameNumber);

    /// Forward a media engine audio sample to the browser as a binary WS frame.
    void forwardAudioViaWs(const QByteArray& data);

    bool m_WsFallbackActive = false;
    bool m_AllowWsFallback = true; ///< Default: WS fallback allowed. Auto mode sets false.
    bool m_ShimConnected = false;  ///< media engine signals connected for fallback

    /// ── Members ─────────────────────────────────────────────────────────────

    RelayBase* m_Relay;
    IMediaEngine* m_Shim = nullptr;

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
    // Always overwritten by setStunServer() from Session before a stream
    // starts; the literal is only what a construction that forgot would get,
    // and it names our own server for the same reason every other default
    // does. An instance under its own MW_DOMAIN is covered by the caller.
    QString m_StunServerUrl = QStringLiteral("stun:stream.moonlightweb.top:3478");

    /// Force ICE-TCP candidates (true = UDP + TCP, false = UDP only).
    bool m_ForceIceTcp = false;

    // ── The router hole this stream is reached through ──────────────────────
    //
    // Claimed by the parent process (RouterPortAllocator) BEFORE the worker is
    // spawned and handed down in the config: the worker never touches the
    // router. The external port is what the browser is told, m_MediaPort is
    // what the socket binds — a router forwards one to the other, and the two
    // differ exactly when another MoonlightWeb host on the LAN got to this
    // slot's own number first.

public:
    /// The router forwards publicIp:externalPort to this slot's media port.
    /// Not called (or called with 0) when there is no mapping: STUN only.
    void setPresetMapping(const QString& publicIp, uint16_t externalPort)
    {
        m_UpnpPublicIP = publicIp;
        m_UpnpExternalPort = externalPort;
    }

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

    /// The router-side port this stream is reached on (0 = no mapping).
    uint16_t upnpMappedPort() const { return m_UpnpExternalPort; }

    /// The public IP the router answers on (empty if no mapping).
    QString upnpPublicIP() const { return m_UpnpPublicIP; }

private:
    /// Build the rtc::Configuration with ICE servers and port range.
    /// ICE-TCP is always enabled as fallback. STUN is always present in
    /// Internet mode. A router mapping pins the socket to the media port the
    /// router forwards to, and the relay rewrites host candidates to the
    /// public address.
    static rtc::Configuration buildIceConfig(bool isInternet, bool mapped, uint16_t mediaPort,
                                             const QString& stunServerUrl,
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
    /// Candidates gathered while the offer is still held. They must not
    /// overtake it: a browser that has not yet called setRemoteDescription
    /// cannot accept a candidate, and the ones it rejects are gone for good.
    std::vector<std::pair<std::string, std::string>> m_PendingCandidates; // (candidate, mid)
    bool m_HelloReceived = false;

    /// Distinct WebRTC media UDP port for this slot (base + slot). Keeps
    /// concurrent workers off each other's port; slot 0 keeps 48010. See
    /// setMediaPort() and mw::routerports::kMediaBasePort.
    quint16 m_MediaPort = 48010;
    NetClassify::Kind m_ClientKind = NetClassify::Kind::Public; // see setClientKind
    /// The router hole, as handed down by the parent. See setPresetMapping().
    uint16_t m_UpnpExternalPort = 0;
    QString m_UpnpPublicIP;
};
