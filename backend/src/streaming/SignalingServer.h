#pragma once

#include <QObject>
#include <QWebSocketServer>
#include <QWebSocket>
#include <QSslConfiguration>
#include <memory>
#include <atomic>

class DataChannelRelay;

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
};
