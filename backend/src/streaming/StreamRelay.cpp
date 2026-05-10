#include "StreamRelay.h"
#include "MoonlightShim.h"

#include <QCoreApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QDateTime>
#include <QMap>

StreamRelay::StreamRelay(MoonlightShim* shim,
                           quint16 wsPort,
                           const QSslConfiguration& sslConfig,
                           QObject* parent)
    : QObject(parent)
    , m_Shim(shim)
    , m_WsPort(wsPort)
{
    qInfo() << "[StreamRelay] Created, wsPort=" << wsPort;

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

    qInfo() << "[StreamRelay] Constructor done, secure=" << secure;
}

StreamRelay::~StreamRelay()
{
    qInfo() << "[StreamRelay] Destructor";
    stop();
}

bool StreamRelay::start()
{
    qInfo() << "[StreamRelay] Starting WS server on port" << m_WsPort;

    if (!m_WsServer->listen(QHostAddress::Any, m_WsPort)) {
        qWarning() << "[StreamRelay] WebSocket server failed to listen on port" << m_WsPort
                   << "error:" << m_WsServer->errorString();
        return false;
    }

    connect(m_WsServer, &QWebSocketServer::newConnection,
            this, &StreamRelay::onNewWsConnection);

    qInfo() << "[StreamRelay] WS server listening OK, m_StreamStarted=" << m_StreamStarted;
    m_Running = true;
    return true;
}

void StreamRelay::stop()
{
    qInfo() << "[StreamRelay] stop(), running=" << m_Running << "frames sent=" << m_FrameCount
            << "pending video=" << m_PendingVideoFrames.size()
            << "pending audio=" << m_PendingAudioFrames.size();
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

// --- Video/audio forwarding -------------------------------------------------

void StreamRelay::onVideoFrame(const QByteArray& data, int frameType, int frameNumber)
{
    // Log first few frames and then every 120
    static int logCounter = 0;
    logCounter++;
    bool shouldLog = (logCounter <= 5) || (logCounter % 120 == 0);

    if (shouldLog) {
        qint64 now = QDateTime::currentMSecsSinceEpoch();
        qInfo() << "[StreamRelay] onVideoFrame #" << logCounter
                << "frameNum=" << frameNumber
                << "type=" << frameType
                << "size=" << data.size()
                << "WSclient=" << (m_WsClient != nullptr)
                << "running=" << m_Running
                << "streamStarted=" << m_StreamStarted
                << "pending=" << m_PendingVideoFrames.size()
                << "time=" << now;
    }

    if (!m_WsClient || !m_Running || !m_StreamStarted) {
        // Buffer while waiting for WS client
        if (m_Running && m_PendingVideoFrames.size() < 120) {
            m_PendingVideoFrames.append(data);
            if (m_PendingVideoFrames.size() <= 3 || m_PendingVideoFrames.size() % 60 == 0) {
                qInfo() << "[StreamRelay] Buffering video frame, pending=" << m_PendingVideoFrames.size();
            }
        }
        return;
    }

    // Flush pending frames (assume they could be keyframes)
    if (!m_PendingVideoFrames.isEmpty()) {
        qInfo() << "[StreamRelay] Flushing" << m_PendingVideoFrames.size() << "pending video frames";
        while (!m_PendingVideoFrames.isEmpty()) {
            QByteArray msg;
            msg.append(static_cast<char>(0x01));
            msg.append(static_cast<char>(0x01));  // flags: assume keyframe
            msg.append(m_PendingVideoFrames.takeFirst());
            m_WsClient->sendBinaryMessage(msg);
            m_FrameCount++;
        }
    }

    // Protocol: [channel:1][flags:1][data:N]
    // flags bit0: 1=IDR keyframe, 0=P-frame
    QByteArray msg;
    msg.append(static_cast<char>(0x01));
    msg.append(static_cast<char>(frameType == 1 ? 0x01 : 0x00));
    msg.append(data);
    m_WsClient->sendBinaryMessage(msg);
    m_FrameCount++;
}

void StreamRelay::onAudioSample(const QByteArray& data)
{
    static int audioCount = 0;
    audioCount++;
    if (audioCount <= 3) {
        qInfo() << "[StreamRelay] onAudioSample #" << audioCount << "size=" << data.size()
                << "WSclient=" << (m_WsClient != nullptr)
                << "streamStarted=" << m_StreamStarted;
    }

    if (!m_WsClient || !m_Running || !m_StreamStarted) {
        if (m_Running && m_PendingAudioFrames.size() < 120) {
            m_PendingAudioFrames.append(data);
        }
        return;
    }

    while (!m_PendingAudioFrames.isEmpty()) {
        QByteArray msg;
        msg.append(static_cast<char>(0x02));
        msg.append(static_cast<char>(0x00));  // flags unused
        msg.append(m_PendingAudioFrames.takeFirst());
        m_WsClient->sendBinaryMessage(msg);
    }

    QByteArray msg;
    msg.append(static_cast<char>(0x02));
    msg.append(static_cast<char>(0x00));  // flags unused
    msg.append(data);
    m_WsClient->sendBinaryMessage(msg);
}

// --- WebSocket handling ------------------------------------------------------

void StreamRelay::onNewWsConnection()
{
    qInfo() << "[StreamRelay] onNewWsConnection, existing client=" << (m_WsClient != nullptr);

    if (m_WsClient) {
        QWebSocket* rejected = m_WsServer->nextPendingConnection();
        qWarning() << "[StreamRelay] Rejecting second client";
        rejected->close(QWebSocketProtocol::CloseCodeNormal, "Single client only");
        rejected->deleteLater();
        return;
    }

    m_WsClient = m_WsServer->nextPendingConnection();
    if (!m_WsClient) {
        qWarning() << "[StreamRelay] nextPendingConnection returned null";
        return;
    }

    qInfo() << "[StreamRelay] WS client created, connecting signals...";

    connect(m_WsClient, &QWebSocket::textMessageReceived,
            this, &StreamRelay::onWsTextMessage);
    connect(m_WsClient, &QWebSocket::disconnected,
            this, &StreamRelay::onWsDisconnected);

    // Log WS errors
    connect(m_WsClient, QOverload<QAbstractSocket::SocketError>::of(&QWebSocket::error),
            [](QAbstractSocket::SocketError err) {
        qWarning() << "[StreamRelay] WebSocket error:" << err;
    });

    qInfo() << "[StreamRelay] WebSocket client connected OK, m_StreamStarted=" << m_StreamStarted
            << "pending video=" << m_PendingVideoFrames.size();
    emit clientConnected();
}

void StreamRelay::onWsTextMessage(const QString& message)
{
    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        qWarning() << "[StreamRelay] Invalid JSON from client:" << message.left(100);
        return;
    }

    QJsonObject msg = doc.object();
    QString type = msg["type"].toString();

    // Log first few input messages of each type
    static QMap<QString, int> inputCounts;
    int& count = inputCounts[type];
    count++;
    if (count <= 2) {
        qInfo() << "[StreamRelay] Input #" << count << "type=" << type;
    }

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
    qInfo() << "[StreamRelay] WebSocket client disconnected, frames sent=" << m_FrameCount
            << "pending video=" << m_PendingVideoFrames.size();

    if (m_WsClient) {
        qInfo() << "[StreamRelay] WS close code=" << m_WsClient->closeCode()
                << "reason=" << m_WsClient->closeReason();
        m_WsClient->deleteLater();
        m_WsClient = nullptr;
    }
    m_StreamStarted = false;
    m_PendingVideoFrames.clear();
    m_PendingAudioFrames.clear();
    emit clientDisconnected();
    emit sessionEnded();
}

// --- Connection management ---------------------------------------------------

void StreamRelay::onShimConnectionStarted()
{
    qInfo() << "[StreamRelay] onShimConnectionStarted — ENet + video/audio streams established";
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
    qInfo() << "[StreamRelay] Stream terminated, code=" << errorCode
            << "frames sent=" << m_FrameCount;
    m_StreamStarted = false;
    if (errorCode != 0)
        emit sessionEnded();
}
