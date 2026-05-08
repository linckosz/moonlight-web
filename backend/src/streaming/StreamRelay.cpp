#include "StreamRelay.h"
#include "MoonlightShim.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>

StreamRelay::StreamRelay(MoonlightShim* shim,
                           quint16 wsPort,
                           const QSslConfiguration& sslConfig,
                           QObject* parent)
    : QObject(parent)
    , m_Shim(shim)
    , m_WsPort(wsPort)
{
    // Connect to MoonlightShim data signals (queued — cross-thread from moonlight internal threads)
    connect(m_Shim, &MoonlightShim::videoFrameReady,
            this, &StreamRelay::onVideoFrame);
    connect(m_Shim, &MoonlightShim::audioSampleReady,
            this, &StreamRelay::onAudioSample);
    connect(m_Shim, &MoonlightShim::connectionStarted,
            this, &StreamRelay::onShimConnectionStarted);
    connect(m_Shim, &MoonlightShim::connectionFailed,
            this, &StreamRelay::onShimConnectionFailed);
    connect(m_Shim, &MoonlightShim::connectionTerminated,
            this, &StreamRelay::onShimConnectionTerminated);

    bool secure = !sslConfig.isNull();
    m_WsServer = new QWebSocketServer(
        QString("Moonlight-Relay"),
        secure ? QWebSocketServer::SecureMode : QWebSocketServer::NonSecureMode,
        this);
    if (secure)
        m_WsServer->setSslConfiguration(sslConfig);
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

    connect(m_WsServer, &QWebSocketServer::newConnection,
            this, &StreamRelay::onNewWsConnection);

    qDebug() << "[StreamRelay] WebSocket server listening on port" << m_WsPort;
    m_Running = true;
    return true;
}

void StreamRelay::stop()
{
    m_Running = false;

    if (m_Shim)
        m_Shim->stopConnection();

    if (m_WsClient) {
        m_WsClient->close();
        m_WsClient->deleteLater();
        m_WsClient = nullptr;
    }

    if (m_WsServer)
        m_WsServer->close();
}

QString StreamRelay::wsUrl() const
{
    bool secure = m_WsServer && m_WsServer->secureMode() == QWebSocketServer::SecureMode;
    return QString("%1://%2:%3")
        .arg(secure ? "wss" : "ws")
        .arg(m_ServerHost)
        .arg(m_WsPort);
}

// --- Video/audio forwarding (data from moonlight-common-c callbacks) ---------

void StreamRelay::onVideoFrame(const QByteArray& data, int frameType, int frameNumber)
{
    Q_UNUSED(frameType);
    Q_UNUSED(frameNumber);

    if (!m_WsClient || !m_Running || !m_StreamStarted) {
        // Buffer the frame while waiting for the WS client to connect
        if (m_Running && m_PendingVideoFrames.size() < 120) {
            m_PendingVideoFrames.append(data);
        }
        return;
    }

    // Flush pending frames first
    while (!m_PendingVideoFrames.isEmpty()) {
        QByteArray msg;
        msg.append(static_cast<char>(0x01));
        msg.append(m_PendingVideoFrames.takeFirst());
        m_WsClient->sendBinaryMessage(msg);
    }

    QByteArray msg;
    msg.append(static_cast<char>(0x01));
    msg.append(data);
    m_WsClient->sendBinaryMessage(msg);
}

void StreamRelay::onAudioSample(const QByteArray& data)
{
    if (!m_WsClient || !m_Running || !m_StreamStarted) {
        if (m_Running && m_PendingAudioFrames.size() < 120) {
            m_PendingAudioFrames.append(data);
        }
        return;
    }

    while (!m_PendingAudioFrames.isEmpty()) {
        QByteArray msg;
        msg.append(static_cast<char>(0x02));
        msg.append(m_PendingAudioFrames.takeFirst());
        m_WsClient->sendBinaryMessage(msg);
    }

    QByteArray msg;
    msg.append(static_cast<char>(0x02));
    msg.append(data);
    m_WsClient->sendBinaryMessage(msg);
}

// --- WebSocket handling ------------------------------------------------------

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

    QJsonObject msg = doc.object();
    QString type = msg["type"].toString();

    if (type == "keydown" || type == "keyup") {
        bool down = (type == "keydown");
        int vk = msg["keyCode"].toInt(0);
        char mods = 0;
        if (msg["ctrlKey"].toBool(false))  mods |= 0x02;
        if (msg["shiftKey"].toBool(false)) mods |= 0x01;
        if (msg["altKey"].toBool(false))   mods |= 0x04;
        if (msg["metaKey"].toBool(false))  mods |= 0x08;
        m_Shim->sendKeyEvent(static_cast<short>(vk), down, mods, 0);
    }
    else if (type == "mousemove") {
        short dx = static_cast<short>(msg["dx"].toInt(0));
        short dy = static_cast<short>(msg["dy"].toInt(0));
        m_Shim->sendMouseMove(dx, dy);
    }
    else if (type == "mousedown" || type == "mouseup") {
        bool down = (type == "mousedown");
        int button = msg["button"].toInt(1);
        m_Shim->sendMouseButton(down, button);
    }
    else if (type == "mousewheel") {
        short delta = static_cast<short>(msg["delta"].toInt(0));
        m_Shim->sendMouseScroll(delta);
    }
    else {
        qWarning() << "[StreamRelay] Unknown input type:" << type;
    }
}

void StreamRelay::onWsDisconnected()
{
    qDebug() << "[StreamRelay] WebSocket client disconnected";

    if (m_WsClient) {
        m_WsClient->deleteLater();
        m_WsClient = nullptr;
    }
    m_StreamStarted = false;
    emit clientDisconnected();
    emit sessionEnded();
}

// --- Connection management ---------------------------------------------------

void StreamRelay::onShimConnectionStarted()
{
    qDebug() << "[StreamRelay] ENet + video/audio streams established";
    m_StreamStarted = true;
}

void StreamRelay::onShimConnectionFailed(const QString& error)
{
    qWarning() << "[StreamRelay] Stream connection failed:" << error;
    m_Running = false;
    emit sessionEnded();
}

void StreamRelay::onShimConnectionTerminated(int errorCode)
{
    qWarning() << "[StreamRelay] Stream terminated, code:" << errorCode;
    m_StreamStarted = false;
    if (errorCode != 0)  // 0 = graceful termination
        emit sessionEnded();
}
