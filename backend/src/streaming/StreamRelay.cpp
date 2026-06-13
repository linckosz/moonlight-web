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
    qInfo() << "[StreamRelay::stop] ENTER, m_Stopping=" << m_Stopping
            << "m_Running=" << m_Running
            << "m_WsClient=" << m_WsClient
            << "m_WsServer=" << m_WsServer
            << "m_Shim=" << m_Shim
            << "frames sent=" << m_FrameCount;

    // Guard against re-entrant calls: closing m_WsClient may fire the
    // disconnected signal synchronously, triggering sessionEnded ->
    // relay->stop() again from the sessionEnded lambda in main.cpp.
    if (m_Stopping) {
        qInfo() << "[StreamRelay::stop] Already stopping, skip re-entrant call";
        return;
    }
    m_Stopping = true;

    m_Running = false;
    qInfo() << "[StreamRelay::stop] m_Running=false, pending video="
            << m_PendingVideoFrames.size() << "pending audio=" << m_PendingAudioFrames.size();

    if (m_Shim) {
        qInfo() << "[StreamRelay::stop] Calling m_Shim->stopConnection() ...";
        m_Shim->stopConnection();
        qInfo() << "[StreamRelay::stop] m_Shim->stopConnection() returned";
    } else {
        qInfo() << "[StreamRelay::stop] No m_Shim to stop";
    }

    if (m_WsClient) {
        qInfo() << "[StreamRelay::stop] Closing WS client, closeCode=" << m_WsClient->closeCode()
                << "state=" << m_WsClient->state();
        qint64 beforeClose = QDateTime::currentMSecsSinceEpoch();
        m_WsClient->close();
        qint64 afterClose = QDateTime::currentMSecsSinceEpoch();
        qInfo() << "[StreamRelay::stop] WS close() took" << (afterClose - beforeClose) << "ms"
                << "new state=" << m_WsClient->state();
        m_WsClient->deleteLater();
        m_WsClient = nullptr;
    } else {
        qInfo() << "[StreamRelay::stop] No WS client to close";
    }

    if (m_WsServer) {
        qInfo() << "[StreamRelay::stop] Closing WS server";
        m_WsServer->close();
        qInfo() << "[StreamRelay::stop] WS server closed";
    } else {
        qInfo() << "[StreamRelay::stop] No WS server to close";
    }

    qInfo() << "[StreamRelay::stop] EXIT";
}

QString StreamRelay::wsUrl() const
{
    // The browser connects via the HttpServer WSS proxy on the unified HTTPS port.
    // The proxy routes /ws/stream to this StreamRelay's local WS port.
    QString host = m_ServerHost;
    if (m_HttpsPort != 443)
        host += ":" + QString::number(m_HttpsPort);
    return QString("wss://%1/ws/stream").arg(host);
}

// --- Fragmented send (same format as DataChannelRelay) -------------------------
// Splits data into chunks of up to kMaxPayloadSize bytes.
// Each WebSocket message: [channel:1][frag_header:17][chunk_payload...]
// Header format (17 bytes total):
//   [frame_id:4][chunk_index:2][total_chunks:2][is_keyframe:1][payload_size:4][backend_ts:4]
// All multi-byte fields in network byte order (big endian).

void StreamRelay::sendVideoFragmentedWss(const QByteArray& data, bool isKeyframe)
{
    if (!m_WsClient || m_Stopping) return;
    if (data.isEmpty()) return;

    int totalSize = data.size();
    int totalChunks = (totalSize + kMaxPayloadSize - 1) / kMaxPayloadSize;
    uint32_t frameId = m_FrameId++;

    // Compute backend timestamp for end-to-end latency
    uint32_t backendTs = static_cast<uint32_t>(
        QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);

    // Log FNV-1a hash for first frames
    if (frameId < 20) {
        uint32_t hash = 0x811c9dc5;
        for (int i = 0; i < totalSize; i++) {
            hash ^= static_cast<unsigned char>(data[i]);
            hash *= 0x01000193;
        }
        qInfo() << "[WS-FRAME] frameId=" << frameId
                << "size=" << totalSize << "keyframe=" << isKeyframe
                << "fnv1a=" << Qt::hex << hash << Qt::dec;
    }

    for (int chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        int offset = chunkIdx * kMaxPayloadSize;
        int payloadSize = std::min(kMaxPayloadSize, totalSize - offset);

        QByteArray msg;
        msg.reserve(1 + kFragHeaderSize + payloadSize);

        // Channel byte (0x01 = video)
        msg.append(static_cast<char>(0x01));

        // Frame ID (4 bytes, big endian)
        msg.append(static_cast<char>((frameId >> 24) & 0xFF));
        msg.append(static_cast<char>((frameId >> 16) & 0xFF));
        msg.append(static_cast<char>((frameId >> 8) & 0xFF));
        msg.append(static_cast<char>(frameId & 0xFF));

        // Chunk index (2 bytes, big endian)
        uint16_t chunkIdx16 = static_cast<uint16_t>(chunkIdx);
        msg.append(static_cast<char>((chunkIdx16 >> 8) & 0xFF));
        msg.append(static_cast<char>(chunkIdx16 & 0xFF));

        // Total chunks (2 bytes, big endian)
        uint16_t totalChunks16 = static_cast<uint16_t>(totalChunks);
        msg.append(static_cast<char>((totalChunks16 >> 8) & 0xFF));
        msg.append(static_cast<char>(totalChunks16 & 0xFF));

        // Is keyframe (1 byte)
        msg.append(static_cast<char>(isKeyframe ? 0x01 : 0x00));

        // Payload size (4 bytes, big endian)
        uint32_t payloadSize32 = static_cast<uint32_t>(payloadSize);
        msg.append(static_cast<char>((payloadSize32 >> 24) & 0xFF));
        msg.append(static_cast<char>((payloadSize32 >> 16) & 0xFF));
        msg.append(static_cast<char>((payloadSize32 >> 8) & 0xFF));
        msg.append(static_cast<char>(payloadSize32 & 0xFF));

        // Backend timestamp (4 bytes, big endian) — same value for all chunks
        msg.append(static_cast<char>((backendTs >> 24) & 0xFF));
        msg.append(static_cast<char>((backendTs >> 16) & 0xFF));
        msg.append(static_cast<char>((backendTs >> 8) & 0xFF));
        msg.append(static_cast<char>(backendTs & 0xFF));

        // Chunk payload
        msg.append(data.constData() + offset, payloadSize);

        m_WsClient->sendBinaryMessage(msg);
    }

    m_FrameCount++;

    if (m_FrameCount <= 3 || m_FrameCount % 300 == 0) {
        qInfo() << "[StreamRelay] Fragmented sent frame #" << m_FrameCount
                << "totalSize=" << totalSize << "chunks=" << totalChunks
                << "isKeyframe=" << isKeyframe << "frameId=" << frameId;
    }
}

// --- Video/audio forwarding -------------------------------------------------

void StreamRelay::onVideoFrame(const QByteArray& data, int frameType, int frameNumber)
{
    // Balance the worker→main pending counter (incremented before each emit).
    // Consume the worker-drop flag: recovery on this transport relies on
    // client-side IDR requests (starvation/stale detection in the browser).
    if (m_Shim) {
        m_Shim->videoFrameDelivered();
        m_Shim->takeWorkerDroppedDelta();
    }

    // Log first few frames only — periodic logging floods the console
    static int logCounter = 0;
    logCounter++;
    bool shouldLog = (logCounter <= 5);

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

    bool isKeyframe = (frameType == 1);

    if (m_UseVideoFragmentation) {
        // Flush pending frames using fragmentation protocol
        while (!m_PendingVideoFrames.isEmpty()) {
            sendVideoFragmentedWss(m_PendingVideoFrames.takeFirst(), true);
        }
        sendVideoFragmentedWss(data, isKeyframe);
        return;  // Fragmented path handles its own logging
    }

    // --- Legacy non-fragmented path (2-byte header) ---

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
    msg.append(static_cast<char>(isKeyframe ? 0x01 : 0x00));
    msg.append(data);

    // Log first WS message sent
    static int wsMsgCount = 0;
    wsMsgCount++;
    if (wsMsgCount <= 2) {
        qInfo() << "[StreamRelay] WS binary msg #" << wsMsgCount
                << "totalSize=" << msg.size()
                << "channel=" << (unsigned char)msg[0]
                << "flags=" << (unsigned char)msg[1]
                << "frameType=" << frameType;
        // Log first 48 bytes of the actual payload (after 2-byte header)
        if (msg.size() > 2) {
            int dumpLen = qMin(48, msg.size() - 2);
            QByteArray hexDump;
            for (int i = 2; i < 2 + dumpLen; i++) {
                hexDump += QString("%1 ").arg((unsigned char)msg[i], 2, 16, QChar('0')).toUtf8();
            }
            qInfo() << "[StreamRelay]   WS payload hex:" << hexDump;
        }

        // Also send a text debug message to the browser with the first 48 bytes hex
        if (m_WsClient && msg.size() > 2) {
            int dumpLen = qMin(48, msg.size() - 2);
            QByteArray hexDump;
            for (int i = 2; i < 2 + dumpLen; i++) {
                hexDump += QString::asprintf("%02x ", (unsigned char)msg[i]).toUtf8();
            }
            hexDump.chop(1); // remove trailing space
            QJsonObject dbgMsg;
            dbgMsg["type"] = "debug_hex";
            dbgMsg["payload"] = QString::fromUtf8(hexDump);
            QJsonDocument dbgDoc(dbgMsg);
            m_WsClient->sendTextMessage(QString::fromUtf8(dbgDoc.toJson(QJsonDocument::Compact)));
        }
    }

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

    // Helper to send a single audio frame with 17-byte frag header (matching _onAudioChunk)
    auto sendAudioFragmented = [this](const QByteArray& audioData) {
        uint32_t frameId = m_FrameId++;
        uint32_t backendTs = static_cast<uint32_t>(
            QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);

        QByteArray msg;
        msg.reserve(1 + kFragHeaderSize + audioData.size());

        // Channel byte (0x02 = audio)
        msg.append(static_cast<char>(0x02));

        // Frame ID (4 bytes, big endian)
        msg.append(static_cast<char>((frameId >> 24) & 0xFF));
        msg.append(static_cast<char>((frameId >> 16) & 0xFF));
        msg.append(static_cast<char>((frameId >> 8) & 0xFF));
        msg.append(static_cast<char>(frameId & 0xFF));

        // Chunk index (2 bytes, always 0 for single-chunk audio)
        msg.append(static_cast<char>(0x00));
        msg.append(static_cast<char>(0x00));

        // Total chunks (2 bytes, always 1 for single-chunk audio)
        msg.append(static_cast<char>(0x00));
        msg.append(static_cast<char>(0x01));

        // Is keyframe (1 byte, always 0 for audio)
        msg.append(static_cast<char>(0x00));

        // Payload size (4 bytes, big endian)
        uint32_t payloadSize32 = static_cast<uint32_t>(audioData.size());
        msg.append(static_cast<char>((payloadSize32 >> 24) & 0xFF));
        msg.append(static_cast<char>((payloadSize32 >> 16) & 0xFF));
        msg.append(static_cast<char>((payloadSize32 >> 8) & 0xFF));
        msg.append(static_cast<char>(payloadSize32 & 0xFF));

        // Backend timestamp (4 bytes, big endian)
        msg.append(static_cast<char>((backendTs >> 24) & 0xFF));
        msg.append(static_cast<char>((backendTs >> 16) & 0xFF));
        msg.append(static_cast<char>((backendTs >> 8) & 0xFF));
        msg.append(static_cast<char>(backendTs & 0xFF));

        // Audio payload
        msg.append(audioData);

        m_WsClient->sendBinaryMessage(msg);
    };

    if (m_UseVideoFragmentation) {
        // Flush pending audio using fragmented format
        while (!m_PendingAudioFrames.isEmpty()) {
            sendAudioFragmented(m_PendingAudioFrames.takeFirst());
        }
        sendAudioFragmented(data);
        return;
    }

    // --- Legacy non-fragmented path (2-byte header) ---

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
    connect(m_WsClient, &QWebSocket::errorOccurred,
            [](QAbstractSocket::SocketError err) {
        qWarning() << "[StreamRelay] WebSocket error:" << err;
    });

    qInfo() << "[StreamRelay] WebSocket client connected OK, m_StreamStarted=" << m_StreamStarted
            << "pending video=" << m_PendingVideoFrames.size();

    // Drop frames buffered before the client connected: flushing them would
    // send stale deltas mislabeled as keyframes, poisoning strict mobile
    // decoders and saturating their decode queue. Start clean from a fresh IDR.
    if (!m_PendingVideoFrames.isEmpty() || !m_PendingAudioFrames.isEmpty()) {
        qInfo() << "[StreamRelay] Discarding" << m_PendingVideoFrames.size()
                << "stale video /" << m_PendingAudioFrames.size()
                << "audio frames, requesting fresh IDR";
        m_PendingVideoFrames.clear();
        m_PendingAudioFrames.clear();
    }
    if (m_Shim) {
        m_Shim->requestIdrFrame();
    }

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
        // Absolute mouse position (non-gaming mode) — same handling as
        // DataChannelRelay, otherwise the host cursor never moves in WSS.
        if (msg.contains("x") && msg.contains("y") &&
            msg.contains("referenceWidth") && msg.contains("referenceHeight")) {
            short x = static_cast<short>(msg["x"].toInt(0));
            short y = static_cast<short>(msg["y"].toInt(0));
            short refW = static_cast<short>(msg["referenceWidth"].toInt(0));
            short refH = static_cast<short>(msg["referenceHeight"].toInt(0));
            m_Shim->sendMousePosition(x, y, refW, refH);
        } else {
            // Gaming mode: relative mouse movement
            short dx = static_cast<short>(msg["dx"].toInt(0));
            short dy = static_cast<short>(msg["dy"].toInt(0));
            m_Shim->sendMouseMove(dx, dy);
        }
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
    else if (type == "requestidr") {
        // Browser lost its reference picture — forward to Sunshine (throttled).
        // Without this, a single decode-queue overflow freezes the WSS stream
        // forever (deltas dropped until a keyframe that never comes).
        if (!m_IdrCooldownTimer.isValid() || m_IdrCooldownTimer.elapsed() >= 300) {
            m_IdrCooldownTimer.restart();
            qInfo() << "[StreamRelay] IDR request from browser — forwarding to Sunshine";
            m_Shim->requestIdrFrame();
        } else {
            // Swallowed by cooldown — the browser retries every 1s, so log only.
            qInfo() << "[StreamRelay] IDR request throttled (cooldown)";
        }
    }
    else if (type == "ping") {
        // Echo a pong so the browser can compute its RTT (stats overlay),
        // matching the DataChannelRelay behaviour.
        if (m_WsClient && m_WsClient->state() == QAbstractSocket::ConnectedState) {
            QJsonObject pong;
            pong["type"] = "pong";
            pong["seq"] = msg["seq"].toInt(0);
            pong["ts"] = msg["ts"].toDouble(0);
            m_WsClient->sendTextMessage(
                QString::fromUtf8(QJsonDocument(pong).toJson(QJsonDocument::Compact)));
        }
    }
    else {
        qWarning() << "[StreamRelay] Unknown input type:" << type;
    }
}

void StreamRelay::onWsDisconnected()
{
    qInfo() << "[StreamRelay::onWsDisconnected] ENTER, m_Running=" << m_Running
            << "m_Stopping=" << m_Stopping
            << "m_StreamStarted=" << m_StreamStarted
            << "frames sent=" << m_FrameCount
            << "pending video=" << m_PendingVideoFrames.size()
            << "pending audio=" << m_PendingAudioFrames.size();

    QString disconnectSource;
    QWebSocketProtocol::CloseCode closeCode = QWebSocketProtocol::CloseCodeNormal;
    QString closeReason;
    if (m_WsClient) {
        closeCode = m_WsClient->closeCode();
        closeReason = m_WsClient->closeReason();
        qInfo() << "[StreamRelay::onWsDisconnected] WS state=" << m_WsClient->state()
                << "closeCode=" << closeCode
                << "closeReason=" << closeReason;
        m_WsClient->deleteLater();
        m_WsClient = nullptr;
    } else {
        qInfo() << "[StreamRelay::onWsDisconnected] m_WsClient already null";
    }

    m_StreamStarted = false;
    m_PendingVideoFrames.clear();
    m_PendingAudioFrames.clear();

    qInfo() << "[StreamRelay::onWsDisconnected] Emitting clientDisconnected ...";
    emit clientDisconnected();

    qInfo() << "[StreamRelay::onWsDisconnected] Emitting sessionEnded ...";
    emit sessionEnded();

    qInfo() << "[StreamRelay::onWsDisconnected] EXIT";
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
    qInfo() << "[StreamRelay::onShimConnectionTerminated] ENTER, errorCode=" << errorCode
            << "m_Running=" << m_Running << "m_Stopping=" << m_Stopping
            << "frames sent=" << m_FrameCount;
    m_StreamStarted = false;

    // Always emit sessionEnded so cleanup runs, regardless of error code.
    // Graceful termination (code 0) still needs relay/shim cleanup.
    // The re-entrant guard in stop() prevents double-free if sessionEnded
    // fires again from other paths (e.g. onWsDisconnected).
    qInfo() << "[StreamRelay::onShimConnectionTerminated] Emitting sessionEnded ...";
    emit sessionEnded();
    qInfo() << "[StreamRelay::onShimConnectionTerminated] EXIT";
}
