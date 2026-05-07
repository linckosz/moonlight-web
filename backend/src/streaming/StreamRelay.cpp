#include "StreamRelay.h"
#include "EnetControlStream.h"
#include "InputEncoder.h"
#include "InputCrypto.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

StreamRelay::StreamRelay(QUdpSocket* videoSocket, QUdpSocket* audioSocket,
                           QUdpSocket* controlUdpSocket,
                           const RtspClient::SessionInfo& info,
                           quint16 wsPort, QObject* parent)
    : QObject(parent)
    , m_VideoSocket(videoSocket)
    , m_AudioSocket(audioSocket)
    , m_ControlUdpSocket(controlUdpSocket)
    , m_SessionInfo(info)
    , m_WsPort(wsPort)
{
    if (m_VideoSocket)  m_VideoSocket->setParent(this);
    if (m_AudioSocket)  m_AudioSocket->setParent(this);
    if (m_ControlUdpSocket) m_ControlUdpSocket->setParent(this);

    // AES-128-GCM encryption for input data (required by Sunshine)
    if (!info.rikey.isEmpty())
        m_Crypto = std::make_unique<InputCrypto>(info.rikey, info.rikeyid);

    m_WsServer = new QWebSocketServer(
        QString("Moonlight-Relay"),
        QWebSocketServer::NonSecureMode, this);
}

StreamRelay::~StreamRelay()
{
    stop();
}

bool StreamRelay::start()
{
    if (!m_WsServer->listen(QHostAddress::Any, m_WsPort)) {
        qWarning() << "[StreamRelay] WebSocket server failed to listen on port" << m_WsPort;
        return false;
    }

    // UDP → WS
    if (m_VideoSocket)
        connect(m_VideoSocket, &QUdpSocket::readyRead, this, &StreamRelay::onVideoReadyRead);
    if (m_AudioSocket)
        connect(m_AudioSocket, &QUdpSocket::readyRead, this, &StreamRelay::onAudioReadyRead);

    // WS server
    connect(m_WsServer, &QWebSocketServer::newConnection,
            this, &StreamRelay::onNewWsConnection);

    // ENet control channel to Sunshine (replaces legacy TCP)
    m_EnetControl = new EnetControlStream(
        m_SessionInfo.host,
        m_SessionInfo.controlPort,
        m_SessionInfo.controlConnectData,
        this);

    connect(m_EnetControl, &EnetControlStream::connected,
            this, &StreamRelay::onEnetConnected);
    connect(m_EnetControl, &EnetControlStream::connectionFailed,
            this, &StreamRelay::onEnetFailed);
    connect(m_EnetControl, &EnetControlStream::disconnected,
            this, &StreamRelay::onEnetDisconnected);

    qDebug() << "[StreamRelay] Starting ENet control channel to"
             << m_SessionInfo.host << ":" << m_SessionInfo.controlPort;
    m_EnetControl->start(); // blocking — may take a few seconds

    m_Running = true;
    return true;
}

void StreamRelay::stop()
{
    m_Running = false;

    if (m_EnetControl) {
        m_EnetControl->stop();
    }

    if (m_WsClient) {
        m_WsClient->close();
        m_WsClient->deleteLater();
        m_WsClient = nullptr;
    }

    if (m_WsServer) {
        m_WsServer->close();
    }
}

QString StreamRelay::wsUrl() const
{
    return QString("ws://localhost:%1").arg(m_WsPort);
}

void StreamRelay::forwardUdp(QUdpSocket* udp, quint8 channel)
{
    if (!m_WsClient || !udp)
        return;

    while (udp->hasPendingDatagrams()) {
        QByteArray datagram;
        datagram.resize(static_cast<int>(udp->pendingDatagramSize()));
        udp->readDatagram(datagram.data(), datagram.size());

        QByteArray msg;
        msg.append(static_cast<char>(channel));
        msg.append(datagram);
        m_WsClient->sendBinaryMessage(msg);
    }
}

void StreamRelay::sendToControl(const QByteArray& packet)
{
    if (!m_EnetControl || !m_EnetControl->isConnected())
        return;

    // Determine channel from the packet magic (at offset 4, after the BE size)
    uint32_t magic = qFromLittleEndian<uint32_t>(packet.constData() + 4);
    quint8 channel;
    switch (magic) {
    case 0x03: case 0x04:  // keyboard down/up
        channel = 0x02;
        break;
    case 0x06: case 0x07:  // mouse move
    case 0x08: case 0x09:  // mouse button
    case 0x0A:              // mouse scroll
        channel = 0x03;
        break;
    default:
        channel = 0x00; // generic
        break;
    }

    // AES-128-GCM encrypt (Sunshine requires encrypted input data)
    QByteArray payload;
    if (m_Crypto)
        payload = m_Crypto->wrapAndEncrypt(packet);
    else
        payload = packet; // fallback (shouldn't happen)

    if (payload.isEmpty()) return;
    m_EnetControl->sendInput(payload, channel);
}

// --- Slots ------------------------------------------------------------------

void StreamRelay::onVideoReadyRead()
{
    forwardUdp(m_VideoSocket, 0x01);
}

void StreamRelay::onAudioReadyRead()
{
    forwardUdp(m_AudioSocket, 0x02);
}

void StreamRelay::onNewWsConnection()
{
    if (m_WsClient) {
        QWebSocket* rejected = m_WsServer->nextPendingConnection();
        rejected->close(QWebSocketProtocol::CloseCodeNormal, "Single client only");
        rejected->deleteLater();
        return;
    }

    m_WsClient = m_WsServer->nextPendingConnection();
    connect(m_WsClient, &QWebSocket::textMessageReceived,
            this, &StreamRelay::onWsTextMessage);
    connect(m_WsClient, &QWebSocket::disconnected,
            this, &StreamRelay::onWsDisconnected);

    qDebug() << "[StreamRelay] WebSocket client connected";
    emit clientConnected();
}

void StreamRelay::onWsTextMessage(const QString& message)
{
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        qWarning() << "[StreamRelay] Invalid JSON from client";
        return;
    }

    QByteArray packet = InputEncoder::encodeFromJson(doc.object());
    if (!packet.isEmpty())
        sendToControl(packet);
}

void StreamRelay::onWsDisconnected()
{
    qDebug() << "[StreamRelay] WebSocket client disconnected";

    if (m_WsClient) {
        m_WsClient->deleteLater();
        m_WsClient = nullptr;
    }
    emit clientDisconnected();
    emit sessionEnded();
}

void StreamRelay::onEnetConnected()
{
    qDebug() << "[StreamRelay] ENet control channel connected — input enabled";
}

void StreamRelay::onEnetFailed(const QString& error)
{
    qWarning() << "[StreamRelay] ENet control channel failed:" << error;
    qWarning() << "[StreamRelay]   Input (keyboard/mouse) will NOT work.";
}

void StreamRelay::onEnetDisconnected()
{
    qWarning() << "[StreamRelay] ENet control channel disconnected";
    emit sessionEnded();
}
