#include "SignalingServer.h"
#include "DataChannelRelay.h"

#include <rtc/rtc.hpp>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QHostAddress>
#include <memory>

SignalingServer::SignalingServer(DataChannelRelay* relay,
                                 quint16 wsPort,
                                 const QString& serverHost,
                                 QObject* parent)
    : QObject(parent)
    , m_Relay(relay)
    , m_WsPort(wsPort)
    , m_ServerHost(serverHost)
{
    qInfo() << "[SignalingServer] Created, wsPort=" << wsPort
            << "mode=NonSecure (zrok handles TLS)";

    // Always NonSecure — TLS is terminated by zrok for remote access.
    // Local LAN clients connect via ws://localhost:<port> directly.
    m_WsServer = new QWebSocketServer(
        QString("Moonlight-Signaling"),
        QWebSocketServer::NonSecureMode,
        this);

    qInfo() << "[SignalingServer] Constructor done";
}

SignalingServer::~SignalingServer()
{
    qInfo() << "[SignalingServer] Destructor";
    stop();
}

bool SignalingServer::start()
{
    qInfo() << "[SignalingServer] Starting on port" << m_WsPort;

    if (!m_WsServer->listen(QHostAddress::Any, m_WsPort)) {
        qWarning() << "[SignalingServer] Failed to listen on port" << m_WsPort
                   << "error:" << m_WsServer->errorString();
        return false;
    }

    connect(m_WsServer, &QWebSocketServer::newConnection,
            this, &SignalingServer::onNewWsConnection);

    // Connect DataChannelRelay signaling signals -> WS forwarding
    connect(m_Relay, &DataChannelRelay::signalingSdpReady,
            this, &SignalingServer::onLocalSdp);
    connect(m_Relay, &DataChannelRelay::signalingIceCandidate,
            this, &SignalingServer::onLocalIceCandidate);
    // Track when DataChannels are fully open (SCTP established)
    connect(m_Relay, &DataChannelRelay::dataChannelsOpen,
            this, &SignalingServer::onDataChannelsOpen);

    qInfo() << "[SignalingServer] Listening OK";
    m_Running = true;
    return true;
}

void SignalingServer::stop()
{
    if (m_Stopping.exchange(true, std::memory_order_acq_rel)) {
        qInfo() << "[SignalingServer::stop] Already stopping";
        return;
    }

    qInfo() << "[SignalingServer::stop] ENTER, signalingComplete=" << m_SignalingComplete
            << "dataChannelsOpen=" << m_DataChannelsOpen;
    m_Running = false;

    if (m_WsClient) {
        qInfo() << "[SignalingServer] Closing WS client"
                << "(state=" << m_WsClient->state()
                << ", error=" << m_WsClient->errorString() << ")";
        m_WsClient->close();
        m_WsClient->deleteLater();
        m_WsClient = nullptr;
    }

    if (m_WsServer) {
        qInfo() << "[SignalingServer] Closing WS server";
        m_WsServer->close();
    }

    m_SignalingComplete = false;
    m_DataChannelsOpen = false;
    qInfo() << "[SignalingServer::stop] EXIT";
}

QString SignalingServer::wsUrl() const
{
    // If an override URL is set (e.g. for zrok tunnel), return it as-is.
    // The browser connects to wss://<zrok-url> directly.
    if (!m_OverrideWsUrl.isEmpty())
        return m_OverrideWsUrl;

    // Default: construct from serverHost + port (local LAN).
    return QString("ws://%1:%2").arg(m_ServerHost).arg(m_WsPort);
}

// --- New WS client connected ---

void SignalingServer::onNewWsConnection()
{
    qInfo() << "[SignalingServer] onNewWsConnection, existing client=" << (m_WsClient != nullptr);

    if (m_WsClient) {
        QWebSocket* rejected = m_WsServer->nextPendingConnection();
        qWarning() << "[SignalingServer] Rejecting second client";
        rejected->close(QWebSocketProtocol::CloseCodeNormal, "Single client only");
        rejected->deleteLater();
        return;
    }

    m_WsClient = m_WsServer->nextPendingConnection();
    if (!m_WsClient) {
        qWarning() << "[SignalingServer] nextPendingConnection returned null";
        return;
    }

    // Check if this is a local or remote connection (for STUN)
    QHostAddress peerAddr = m_WsClient->peerAddress();
    bool isInternet = !isPrivateAddress(peerAddr.toString());
    qInfo() << "[SignalingServer] Client connected from" << peerAddr.toString()
            << "(isInternet=" << isInternet << ")";

    connect(m_WsClient, &QWebSocket::textMessageReceived,
            this, &SignalingServer::onWsTextMessage);
    connect(m_WsClient, &QWebSocket::disconnected,
            this, &SignalingServer::onWsDisconnected);

    connect(m_WsClient, &QWebSocket::errorOccurred,
            [](QAbstractSocket::SocketError err) {
        qWarning() << "[SignalingServer] WS error:" << err;
    });

    // Prepare libdatachannel PeerConnection with appropriate ICE config
    rtc::Configuration config;
    config.iceTransportPolicy = rtc::TransportPolicy::All;
    config.forceMediaTransport = true;  // DataChannel only, no media tracks

    if (isInternet) {
        qInfo() << "[SignalingServer] Adding STUN server for Internet client";
        rtc::IceServer stun("stun:stun.l.google.com:19302");
        config.iceServers.push_back(stun);
    } else {
        qInfo() << "[SignalingServer] LAN client: no STUN needed";
    }

    // Prepare the PeerConnection + DataChannels
    // This will trigger signalingSdpReady with the SDP offer to send to browser
    m_Relay->prepare(config, isInternet);

    qInfo() << "[SignalingServer] Client connected, waiting for SDP offer from libdatachannel";
    emit clientConnected();
}

// --- WS text message received (SDP answer or ICE candidate from browser) ---

void SignalingServer::onWsTextMessage(const QString& message)
{
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        qWarning() << "[SignalingServer] Invalid JSON:" << message.left(100);
        return;
    }

    QJsonObject msg = doc.object();
    QString type = msg["type"].toString();

    static int msgCount = 0;
    msgCount++;
    if (msgCount <= 3) {
        qInfo() << "[SignalingServer] WS msg #" << msgCount << "type=" << type;
    }

    if (type == "sdp") {
        QString sdp = msg["sdp"].toString();
        qInfo() << "[SignalingServer] Received SDP answer, length=" << sdp.size();

        // Feed the answer into libdatachannel PeerConnection
        if (!m_Relay->setRemoteDescription(sdp.toStdString())) {
            qWarning() << "[SignalingServer] Failed to set remote SDP description";
        } else {
            m_SignalingComplete = true;
            qInfo() << "[SignalingServer] SDP answer set successfully";
        }
    }
    else if (type == "ice") {
        QString candidate = msg["candidate"].toString();
        QString mid = msg["mid"].toString();
        qInfo() << "[SignalingServer] Received ICE candidate, mid=" << mid;

        m_Relay->addRemoteCandidate(candidate.toStdString(), mid.toStdString());
    }
    else {
        qWarning() << "[SignalingServer] Unknown message type:" << type;
    }
}

void SignalingServer::onWsDisconnected()
{
    QString closeReasonStr;
    QWebSocketProtocol::CloseCode closeCode = QWebSocketProtocol::CloseCodeNormal;
    if (m_WsClient) {
        closeCode = m_WsClient->closeCode();
        closeReasonStr = m_WsClient->closeReason();
        qInfo() << "[SignalingServer::onWsDisconnected] WS closed: code="
                << static_cast<int>(closeCode)
                << "reason=" << closeReasonStr
                << "error=" << m_WsClient->errorString();
    }

    qInfo() << "[SignalingServer::onWsDisconnected] ENTER, m_Running=" << m_Running
            << "m_Stopping=" << m_Stopping
            << "m_SignalingComplete=" << m_SignalingComplete
            << "m_DataChannelsOpen=" << m_DataChannelsOpen;

    if (m_WsClient) {
        m_WsClient->deleteLater();
        m_WsClient = nullptr;
    }

    bool wasComplete = m_SignalingComplete;
    m_SignalingComplete = false;

    qInfo() << "[SignalingServer::onWsDisconnected] Emitting clientDisconnected";
    emit clientDisconnected();

    // Only emit sessionEnded if signaling was NOT yet complete AND
    // DataChannels were NOT yet open.  Once the SDP answer has been
    // received (m_SignalingComplete) OR the DataChannels are established
    // (m_DataChannelsOpen), the signaling WS is no longer needed — the
    // browser intentionally closes it after all 3 DataChannels are open.
    // Emitting sessionEnded in that case would terminate the streaming
    // session prematurely.
    bool dcReady = wasComplete || m_DataChannelsOpen;
    if (!dcReady) {
        qInfo() << "[SignalingServer::onWsDisconnected] Signaling NOT complete & DCs NOT open — emitting sessionEnded";
        emit sessionEnded();
    } else {
        qInfo() << "[SignalingServer::onWsDisconnected] Signaling complete or DCs open — NOT emitting sessionEnded (expected WS close)";
    }

    qInfo() << "[SignalingServer::onWsDisconnected] EXIT";
}

// --- DataChannelRelay signals forwarded to WS client ---

void SignalingServer::onLocalSdp(const std::string& sdp)
{
    if (!m_WsClient || !m_WsClient->isValid()) {
        qWarning() << "[SignalingServer] No WS client to send SDP offer";
        return;
    }

    qInfo() << "[SignalingServer] Forwarding SDP offer to browser, length=" << sdp.size();

    QJsonObject msg;
    msg["type"] = "sdp";
    msg["sdp"] = QString::fromStdString(sdp);
    QJsonDocument doc(msg);
    m_WsClient->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

void SignalingServer::onLocalIceCandidate(const std::string& candidate, const std::string& mid)
{
    if (!m_WsClient || !m_WsClient->isValid()) return;

    QJsonObject msg;
    msg["type"] = "ice";
    msg["candidate"] = QString::fromStdString(candidate);
    msg["mid"] = QString::fromStdString(mid);
    QJsonDocument doc(msg);
    m_WsClient->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
}

void SignalingServer::onDataChannelsOpen()
{
    qInfo() << "[SignalingServer] DataChannels are open (SCTP established)";
    m_DataChannelsOpen = true;

    // The browser side will close the signaling WS once it detects
    // all DCs are open.  From this point on, WS closure is expected
    // and should NOT trigger sessionEnded.
    //
    // If the browser side never closes the WS (e.g. because its
    // _closeSignalingWs wasn't triggered), the WS stays open and
    // is harmless — it will be cleaned up when the session ends.
}

// --- Private helpers ---

bool SignalingServer::isPrivateAddress(const QString& ip) const
{
    // Localhost
    if (ip == "127.0.0.1" || ip == "::1" || ip == "localhost") return true;

    // Parse IPv4
    QHostAddress addr(ip);
    if (addr.protocol() == QAbstractSocket::IPv4Protocol) {
        quint32 ipv4 = addr.toIPv4Address();
        // 10.0.0.0/8
        if ((ipv4 & 0xFF000000) == 0x0A000000) return true;
        // 172.16.0.0/12
        if ((ipv4 & 0xFFF00000) == 0xAC100000) return true;
        // 192.168.0.0/16
        if ((ipv4 & 0xFFFF0000) == 0xC0A80000) return true;
        // 169.254.0.0/16 (link-local)
        if ((ipv4 & 0xFFFF0000) == 0xA9FE0000) return true;
    }

    return false;
}
