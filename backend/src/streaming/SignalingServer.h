#pragma once

#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QSslConfiguration>
#include <memory>
#include <atomic>

namespace rtc {
struct Configuration;
}

class DataChannelRelay;
class UPNPClient;

// Minimal WebSocket server for WebRTC signaling only.
// Exchanges SDP offer/answer and ICE candidates between the browser
// and the DataChannelRelay's libdatachannel PeerConnection.
//
// Protocol (JSON over WebSocket):
//   browser -> server: {"type":"sdp","sdp":"..."}          // SDP answer (after receiving offer from us)
//   server -> browser: {"type":"sdp","sdp":"..."}          // SDP offer
//   browser -> server: {"type":"ice","candidate":"...","mid":"0"}
//   server -> browser: {"type":"ice","candidate":"...","mid":"0"}
class SignalingServer : public QObject
{
    Q_OBJECT

public:
    SignalingServer(DataChannelRelay* relay,
                    quint16 wsPort,
                    const QString& serverHost = "localhost",
                    QObject* parent = nullptr);
    ~SignalingServer();

    bool start();
    void stop();

    void setServerHost(const QString& host) { m_ServerHost = host; }
    void setHttpsPort(quint16 port) { m_HttpsPort = port; }
    quint16 port() const { return m_WsPort; }

    /// Returns the WebSocket URL for browser connections.
    /// If an override URL is set (e.g. for nport tunnel), returns that instead.
    QString wsUrl() const;

    /// Override the WS URL (used when nport provides a public WSS endpoint).
    /// The browser will connect to this URL instead of the local ws://... one.
    void setOverrideWsUrl(const QString& url) { m_OverrideWsUrl = url; }

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

private:
    bool isPrivateAddress(const QString& ip) const;

    DataChannelRelay* m_Relay;

    QWebSocketServer* m_WsServer = nullptr;
    QWebSocket* m_WsClient = nullptr;
    quint16 m_WsPort = 0;
    quint16 m_HttpsPort = 443;
    QString m_ServerHost;
    bool m_Running = false;
    std::atomic<bool> m_Stopping{false};
    bool m_SignalingComplete = false;
    bool m_DataChannelsOpen = false;

    int m_ClientPort = 0;

    /// If non-empty, wsUrl() returns this URL instead of constructing one.
    QString m_OverrideWsUrl;

    // ── UPnP NAT traversal ──────────────────────────────────────────────────

public:
    /// Enable/disable UPnP port mapping for NAT traversal.
    /// Called before start() to set the preference from settings.
    void setUseUPnP(bool enable) { m_UseUPnP = enable; }

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
    /// UPnP-aware: if a mapping is active, portRange is fixed and STUN
    /// may be omitted (UPnP-provided public IP is used instead).
    static rtc::Configuration buildIceConfig(bool isInternet,
                                              uint16_t upnpMappedPort);

    bool m_UseUPnP = true;
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
