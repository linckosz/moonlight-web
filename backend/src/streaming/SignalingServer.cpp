#include "SignalingServer.h"
#include "RelayBase.h"
#include "DataChannelRelay.h"
#include "MediaTrackRelay.h"

extern "C" {
#include "Limelight.h"
}
#include "MoonlightShim.h"
#include "network/UPNPClient.h"

#include <rtc/rtc.hpp>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDebug>
#include <QHostAddress>
#include <QThread>
#include <QMetaObject>
#include <QTimer>
#include <QMap>
#include <memory>

SignalingServer::SignalingServer(RelayBase* relay,
                                 quint16 wsPort,
                                 const QString& serverHost,
                                 QObject* parent)
    : QObject(parent)
    , m_Relay(relay)
    , m_WsPort(wsPort)
    , m_ServerHost(serverHost)
{
    qInfo() << "[SignalingServer] Created, wsPort=" << wsPort
            << "mode=NonSecure (tunnel/Cloudflare handles TLS)";

    // Always NonSecure — TLS is terminated by the external tunnel or Cloudflare.
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

    // Bind with a short bounded retry. The signaling port is a fixed singleton;
    // on take-over the previous session's SignalingServer may still be releasing
    // it on its own thread when this new one binds. Retry briefly (this runs on
    // the relay thread during startup, nothing else uses it yet). Non-AddressInUse
    // errors fail immediately. Normally listen() succeeds on the first attempt.
    int bindAttempts = 0;
    while (!m_WsServer->listen(QHostAddress::Any, m_WsPort)) {
        if (++bindAttempts > 20) {
            qWarning() << "[SignalingServer] Failed to listen on port" << m_WsPort
                       << "error:" << m_WsServer->errorString()
                       << "after" << bindAttempts << "attempt(s)";
            return false;
        }
        qInfo() << "[SignalingServer] Port" << m_WsPort << "busy:" << m_WsServer->errorString()
                << "(take-over in progress?), retrying" << bindAttempts << "/20";
        QThread::msleep(50);  // max ~1s; on the relay thread
    }

    connect(m_WsServer, &QWebSocketServer::newConnection,
            this, &SignalingServer::onNewWsConnection);

    // Connect relay signaling signals -> WS forwarding
    connect(m_Relay, &RelayBase::signalingSdpReady,
            this, &SignalingServer::onLocalSdp);
    connect(m_Relay, &RelayBase::signalingIceCandidate,
            this, &SignalingServer::onLocalIceCandidate);
    // Track when DataChannels/Tracks are fully open
    connect(m_Relay, &RelayBase::dataChannelsOpen,
            this, &SignalingServer::onDataChannelsOpen);

    // ICE timeout → WebSocket fallback (when UDP is blocked)
    if (auto* dcRelay = qobject_cast<DataChannelRelay*>(m_Relay)) {
        connect(dcRelay, &DataChannelRelay::iceTimedOut,
                this, &SignalingServer::onRelayIceTimedOut);
    } else if (auto* mtRelay = qobject_cast<MediaTrackRelay*>(m_Relay)) {
        connect(mtRelay, &MediaTrackRelay::iceTimedOut,
                this, &SignalingServer::onRelayIceTimedOut);
    }

    qInfo() << "[SignalingServer] Listening OK";
    m_Running = true;

    // Async UPnP discovery: kicks off IGD discovery without blocking start().
    // The browser doesn't connect immediately (user still clicks "Launch"), so
    // UPnP has time to complete before the PeerConnection is created.
    if (m_UseUPnP && m_ServerHost != "localhost") {
        QTimer::singleShot(0, this, [this]() {
            setupUPnP();
        });
    }

    return true;
}

void SignalingServer::stop()
{
    // Marshal onto the signaling thread when called cross-thread (main:
    // Session::quit). The QWebSocketServer/QWebSocket are bound to this thread.
    if (QThread::currentThread() != this->thread()) {
        QMetaObject::invokeMethod(this, [this]() { stop(); }, Qt::QueuedConnection);
        return;
    }

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

    // Disconnect WS fallback signal handlers (if any)
    if (m_ShimConnected && m_Shim) {
        disconnect(m_Shim, &MoonlightShim::videoFrameReady,
                   this, &SignalingServer::forwardVideoViaWs);
        disconnect(m_Shim, &MoonlightShim::audioSampleReady,
                   this, &SignalingServer::forwardAudioViaWs);
        m_ShimConnected = false;
    }
    m_WsFallbackActive = false;

    cleanupUPnP();

    m_SignalingComplete = false;
    m_DataChannelsOpen = false;
    qInfo() << "[SignalingServer::stop] EXIT";
}

QString SignalingServer::wsUrl() const
{
    if (!m_OverrideWsUrl.isEmpty()) {
        // Override URL (e.g. from a public tunnel endpoint)
        QString url = m_OverrideWsUrl;
        // Replace https:// with wss:// for WebSocket protocol
        if (url.startsWith("https://"))
            url.replace(0, 8, "wss://");
        // Ensure /ws path for the proxy on the unified port
        if (!url.endsWith("/ws"))
            url += "/ws";
        return url;
    }

    // LAN: WebSocket goes through the same HTTPS port (443) via HttpServer proxy.
    // The browser connects to wss://<host>[:<port>]/ws, which triggers a WebSocket
    // upgrade detection in HttpServer that proxies to the local signaling server.
    QString host = m_ServerHost;
    if (m_HttpsPort != 443)
        host += ":" + QString::number(m_HttpsPort);
    return QString("wss://%1/ws").arg(host);
}

// --- ICE configuration push to browser ---

void SignalingServer::sendIceConfig()
{
    if (!m_WsClient || !m_WsClient->isValid()) {
        qWarning() << "[SignalingServer] Cannot send ice-config: no WS client";
        return;
    }

    QJsonObject iceServer;
    iceServer["urls"] = m_StunServerUrl;
    QJsonArray iceServers;
    iceServers.append(iceServer);

    QJsonObject msg;
    msg["type"] = "ice-config";
    msg["iceServers"] = iceServers;

    QJsonDocument doc(msg);
    m_WsClient->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    qInfo() << "[SignalingServer] Sent ice-config: stun=" << m_StunServerUrl;
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
    // Normalize IPv4-mapped IPv6 (::ffff:x.x.x.x -> x.x.x.x) so that
    // isPrivateAddress() sees a clean IPv4 string instead of an IPv6 one.
    QString peerAddrStr = peerAddr.toString();
    if (peerAddrStr.startsWith("::ffff:") && peerAddrStr.count('.') == 3)
        peerAddrStr = peerAddrStr.mid(7);
    bool isInternet = !isPrivateAddress(peerAddrStr);
    // Force LAN mode for loopback (more robust than classification alone)
    if (peerAddrStr == "127.0.0.1" || peerAddrStr == "::1")
        isInternet = false;
    qInfo() << "[SignalingServer] Client connected from" << peerAddrStr
            << "(isInternet=" << isInternet << ")";

    connect(m_WsClient, &QWebSocket::textMessageReceived,
            this, &SignalingServer::onWsTextMessage);
    connect(m_WsClient, &QWebSocket::disconnected,
            this, &SignalingServer::onWsDisconnected);

    connect(m_WsClient, &QWebSocket::errorOccurred,
            [](QAbstractSocket::SocketError err) {
        qWarning() << "[SignalingServer] WS error:" << err;
    });

    // Send ICE server configuration to the browser first, so the frontend
    // knows which STUN server to use before creating its RTCPeerConnection.
    sendIceConfig();

    // Build ICE configuration: STUN + optionally UPnP-aware fixed port
    // m_ForceIceTcp controls whether ICE-TCP candidates are generated
    // (true = UDP + TCP, false = UDP only).
    rtc::Configuration config = buildIceConfig(isInternet, m_UpnpMappedPort, m_StunServerUrl, m_ForceIceTcp);

    // If UPnP is active, tell the relay to rewrite host candidates with the
    // public IP and mapped port so the browser sees a "host" candidate at
    // PUBLIC_IP:48010 instead of 192.168.x.x:48010.
    if (!m_UpnpPublicIP.isEmpty() && m_UpnpMappedPort > 0) {
        m_Relay->setPublicAddress(
            m_UpnpPublicIP.toStdString(),
            m_UpnpMappedPort);
        m_Relay->setForceHostCandidatePublic(true);
        // Suppress IPv6 candidates so ICE is forced to use the IPv4 UPnP path.
        // Residential IPv6 often has firewall rules that block unsolicited
        // inbound traffic, causing DTLS/SCTP to fail silently.
        m_Relay->setSuppressIPv6Candidates(true);
        qInfo() << "[SignalingServer] UPnP: relaying host candidate as"
                << m_UpnpPublicIP << ":" << m_UpnpMappedPort;
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

    // In WS fallback mode, text messages from the browser carry input commands
    // (keydown, mousemove, etc.) instead of signaling (sdp, ice).
    if (m_WsFallbackActive) {
        handleWsFallbackInput(message);
        return;
    }

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
    else if (type == "fallback-ws-request") {
        if (m_WsFallbackActive) {
            qWarning() << "[SignalingServer] Fallback WS already active, ignoring duplicate request";
            return;
        }
        if (!m_AllowWsFallback) {
            qWarning() << "[SignalingServer] Auto mode: ignoring browser fallback-ws-request,"
                       << "letting auto chain decide next transport";
            return;
        }
        qWarning() << "[SignalingServer] Browser requested WS fallback (ICE disconnected/failed before connected)";
        // The browser detected ICE failure before ever reaching "connected"
        // (UDP blocked by corporate firewall). Transition to WS fallback
        // immediately — don't wait for the libdatachannel ICE timeout.
        startWsFallback();
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

// ── WS Fallback — ICE timeout → WebSocket data transport ──────────────────────

void SignalingServer::onRelayIceTimedOut()
{
    if (m_Stopping.load() || m_WsFallbackActive) {
        qInfo() << "[SignalingServer] onRelayIceTimedOut ignored: stopping="
                << m_Stopping.load() << "fallbackActive=" << m_WsFallbackActive;
        return;
    }

    if (!m_AllowWsFallback) {
        // Auto mode: WS fallback is disabled so the auto fallback chain
        // can try the next transport. sessionEnded() triggers relay tracking
        // which calls tryNext().
        qWarning() << "[SignalingServer] ICE timeout — WS fallback disabled (auto mode),"
                    << "emitting sessionEnded for fallback chain";
        emit sessionEnded();
        return;
    }

    qWarning() << "[SignalingServer] ICE timeout — starting WebSocket fallback";
    startWsFallback();
}

void SignalingServer::startWsFallback()
{
    if (!m_WsClient || !m_WsClient->isValid() || !m_Shim) {
        qWarning() << "[SignalingServer] Cannot start WS fallback:"
                    << "WS client valid=" << (m_WsClient && m_WsClient->isValid())
                    << "Shim set=" << (m_Shim != nullptr);
        // If we can't fallback, end the session
        emit sessionEnded();
        return;
    }

    qInfo() << "[SignalingServer] === STARTING WS FALLBACK ===";
    qInfo() << "[SignalingServer] ICE failed — routing video/audio over signaling WebSocket";

    m_WsFallbackActive = true;

    // Step 1: Save any buffered keyframe BEFORE stopping the relay.
    // DataChannelRelay buffers the initial IDR from Sunshine (arrives before
    // DataChannels open). When stop() is called, the buffer is cleared. If we
    // don't save it here, the fallback WS starts with delta frames and the
    // browser's VideoDecoder can never configure → permanent black screen.
    QByteArray savedKeyframe;
    if (auto* dcRelay = qobject_cast<DataChannelRelay*>(m_Relay)) {
        savedKeyframe = dcRelay->takeBufferedKeyframe();
        if (!savedKeyframe.isEmpty()) {
            qInfo() << "[SignalingServer] Saved buffered keyframe for WS fallback, size="
                    << savedKeyframe.size();
        }
    }

    // Step 2: Stop the DataChannelRelay (sets m_Stopping, its signal handlers
    // become no-ops but remain connected to avoid Qt disconnect issues).
    m_Relay->stop();

    // Step 3: Connect MoonlightShim video/audio signals to WS forwarding slots.
    // These fire in ADDITION to DataChannelRelay's slots (which are no-ops now).
    if (!m_ShimConnected) {
        connect(m_Shim, &MoonlightShim::videoFrameReady,
                this, &SignalingServer::forwardVideoViaWs);
        connect(m_Shim, &MoonlightShim::audioSampleReady,
                this, &SignalingServer::forwardAudioViaWs);
        m_ShimConnected = true;
    }

    // Step 4: If we saved a buffered keyframe, send it as the first WS frame
    // immediately (before any delta frames from the shim). This guarantees the
    // browser's NAL parser extracts SPS/PPS and VideoDecoder configures on the
    // very first frame, even if the shim is currently in a delta-only window.
    if (!savedKeyframe.isEmpty()) {
        qInfo() << "[SignalingServer] Sending saved keyframe as first WS fallback frame";
        forwardVideoViaWs(savedKeyframe, 1, 0);  // frameType=1 = keyframe
    }

    // Step 5: Send fallback notification to browser — tells the frontend to
    // repurpose this WS for data transport (binary frames arrive immediately).
    QJsonObject fallbackMsg;
    fallbackMsg["type"] = "fallback-ws";
    QJsonDocument doc(fallbackMsg);
    m_WsClient->sendTextMessage(QString::fromUtf8(doc.toJson(QJsonDocument::Compact)));
    qInfo() << "[SignalingServer] Sent fallback-ws notification to browser";

    // Step 6: Emit dataChannelsOpen — signals StreamView that the transport
    // is ready (same as when WebRTC DataChannels open normally).
    // This triggers the browser to start rendering.
    onDataChannelsOpen();

    qInfo() << "[SignalingServer] === WS FALLBACK ACTIVE ===";
    qInfo() << "[SignalingServer] Video frames and audio samples will now be"
            << "sent as binary WebSocket messages";
}

// ── WS Fallback: fragment video/audio and send as binary WS frames ────────────
//
// Uses the same fragmentation format as DataChannelRelay (17-byte header + payload),
// with an additional 1-byte channel prefix:
//   [channel:1][frag_header:17][payload...]
//
// Channel byte: 0x01 = video, 0x02 = audio
//
// The frontend strips the channel byte and dispatches to the existing
// _onVideoChunk / _onAudioChunk reassembly logic (unchanged format).
//
// We don't use backpressure here — WebSocket over TCP handles buffering
// transparently (unlike SCTP DataChannels with limited send buffers).

static constexpr int kFallbackFragHeaderSize = 17;
static constexpr int kFallbackMaxPayloadSize = 14000;
static constexpr std::byte kChannelVideo{0x01};
static constexpr std::byte kChannelAudio{0x02};

void SignalingServer::forwardVideoViaWs(const QByteArray& data, int frameType, int)
{
    if (m_Stopping.load() || !m_WsFallbackActive || !m_WsClient || !m_WsClient->isValid())
        return;
    if (data.isEmpty()) return;

    static int fallbackFrameCount = 0;
    fallbackFrameCount++;

    int totalSize = data.size();
    int totalChunks = (totalSize + kFallbackMaxPayloadSize - 1) / kFallbackMaxPayloadSize;
    uint32_t backendTs = static_cast<uint32_t>(
        QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);

    // Generate a monotonic frame ID for this frame (same scheme as DataChannelRelay)
    // Use a static counter scoped to this function
    static uint32_t wsFrameId = 0;
    uint32_t frameId = wsFrameId++;

    for (int chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        int offset = chunkIdx * kFallbackMaxPayloadSize;
        int payloadSize = std::min(kFallbackMaxPayloadSize, totalSize - offset);

        // Total message: 1 (channel) + 17 (frag header) + payload
        QByteArray msg(1 + kFallbackFragHeaderSize + payloadSize, Qt::Uninitialized);

        // Channel byte (1 byte)
        msg[0] = static_cast<char>(kChannelVideo);

        // Frame ID (4 bytes, big endian)
        msg[1] = static_cast<char>((frameId >> 24) & 0xFF);
        msg[2] = static_cast<char>((frameId >> 16) & 0xFF);
        msg[3] = static_cast<char>((frameId >> 8) & 0xFF);
        msg[4] = static_cast<char>(frameId & 0xFF);

        // Chunk index (2 bytes, big endian)
        uint16_t chunkIdx16 = static_cast<uint16_t>(chunkIdx);
        msg[5] = static_cast<char>((chunkIdx16 >> 8) & 0xFF);
        msg[6] = static_cast<char>(chunkIdx16 & 0xFF);

        // Total chunks (2 bytes, big endian)
        uint16_t totalChunks16 = static_cast<uint16_t>(totalChunks);
        msg[7] = static_cast<char>((totalChunks16 >> 8) & 0xFF);
        msg[8] = static_cast<char>(totalChunks16 & 0xFF);

        // Is keyframe (1 byte)
        bool isKeyframe = (frameType == 1);
        msg[9] = static_cast<char>(isKeyframe ? 0x01 : 0x00);

        // Payload size (4 bytes, big endian)
        uint32_t payloadSize32 = static_cast<uint32_t>(payloadSize);
        msg[10] = static_cast<char>((payloadSize32 >> 24) & 0xFF);
        msg[11] = static_cast<char>((payloadSize32 >> 16) & 0xFF);
        msg[12] = static_cast<char>((payloadSize32 >> 8) & 0xFF);
        msg[13] = static_cast<char>(payloadSize32 & 0xFF);

        // Backend timestamp (4 bytes, big endian)
        msg[14] = static_cast<char>((backendTs >> 24) & 0xFF);
        msg[15] = static_cast<char>((backendTs >> 16) & 0xFF);
        msg[16] = static_cast<char>((backendTs >> 8) & 0xFF);
        msg[17] = static_cast<char>(backendTs & 0xFF);

        // Payload
        if (payloadSize > 0) {
            memcpy(msg.data() + 1 + kFallbackFragHeaderSize,
                   data.constData() + offset,
                   static_cast<size_t>(payloadSize));
        }

        try {
            m_WsClient->sendBinaryMessage(msg);
        } catch (const std::exception& e) {
            qWarning() << "[SignalingServer] WS fallback send error:" << e.what();
            return;
        }
    }

    if (fallbackFrameCount <= 3 || fallbackFrameCount % 300 == 0) {
        qInfo() << "[SignalingServer] Fallback video frame #" << fallbackFrameCount
                << "size=" << totalSize << "chunks=" << totalChunks
                << "keyframe=" << (frameType == 1);
    }
}

void SignalingServer::forwardAudioViaWs(const QByteArray& data)
{
    if (m_Stopping.load() || !m_WsFallbackActive || !m_WsClient || !m_WsClient->isValid())
        return;
    if (data.isEmpty()) return;

    static int fallbackAudioCount = 0;
    fallbackAudioCount++;

    int totalSize = data.size();
    int totalChunks = (totalSize + kFallbackMaxPayloadSize - 1) / kFallbackMaxPayloadSize;
    uint32_t backendTs = static_cast<uint32_t>(
        QDateTime::currentMSecsSinceEpoch() & 0xFFFFFFFF);

    static uint32_t wsFrameId = 0;
    uint32_t frameId = wsFrameId++;

    for (int chunkIdx = 0; chunkIdx < totalChunks; chunkIdx++) {
        int offset = chunkIdx * kFallbackMaxPayloadSize;
        int payloadSize = std::min(kFallbackMaxPayloadSize, totalSize - offset);

        QByteArray msg(1 + kFallbackFragHeaderSize + payloadSize, Qt::Uninitialized);

        // Channel byte: audio
        msg[0] = static_cast<char>(kChannelAudio);

        // Frame ID (4 bytes, big endian)
        msg[1] = static_cast<char>((frameId >> 24) & 0xFF);
        msg[2] = static_cast<char>((frameId >> 16) & 0xFF);
        msg[3] = static_cast<char>((frameId >> 8) & 0xFF);
        msg[4] = static_cast<char>(frameId & 0xFF);

        // Chunk index (2 bytes, big endian)
        uint16_t chunkIdx16 = static_cast<uint16_t>(chunkIdx);
        msg[5] = static_cast<char>((chunkIdx16 >> 8) & 0xFF);
        msg[6] = static_cast<char>(chunkIdx16 & 0xFF);

        // Total chunks (2 bytes, big endian)
        uint16_t totalChunks16 = static_cast<uint16_t>(totalChunks);
        msg[7] = static_cast<char>((totalChunks16 >> 8) & 0xFF);
        msg[8] = static_cast<char>(totalChunks16 & 0xFF);

        // Is keyframe: always 0 for audio
        msg[9] = static_cast<char>(0x00);

        // Payload size (4 bytes, big endian)
        uint32_t payloadSize32 = static_cast<uint32_t>(payloadSize);
        msg[10] = static_cast<char>((payloadSize32 >> 24) & 0xFF);
        msg[11] = static_cast<char>((payloadSize32 >> 16) & 0xFF);
        msg[12] = static_cast<char>((payloadSize32 >> 8) & 0xFF);
        msg[13] = static_cast<char>(payloadSize32 & 0xFF);

        // Backend timestamp (4 bytes, big endian)
        msg[14] = static_cast<char>((backendTs >> 24) & 0xFF);
        msg[15] = static_cast<char>((backendTs >> 16) & 0xFF);
        msg[16] = static_cast<char>((backendTs >> 8) & 0xFF);
        msg[17] = static_cast<char>(backendTs & 0xFF);

        // Payload
        if (payloadSize > 0) {
            memcpy(msg.data() + 1 + kFallbackFragHeaderSize,
                   data.constData() + offset,
                   static_cast<size_t>(payloadSize));
        }

        try {
            m_WsClient->sendBinaryMessage(msg);
        } catch (const std::exception& e) {
            qWarning() << "[SignalingServer] WS fallback audio send error:" << e.what();
            return;
        }
    }

    if (fallbackAudioCount <= 3) {
        qInfo() << "[SignalingServer] Fallback audio #" << fallbackAudioCount
                << "size=" << totalSize << "chunks=" << totalChunks;
    }
}

void SignalingServer::handleWsFallbackInput(const QString& message)
{
    if (m_Stopping.load() || !m_Shim) return;

    QJsonDocument doc = QJsonDocument::fromJson(message.toUtf8());
    if (!doc.isObject()) {
        qWarning() << "[SignalingServer] Fallback input: invalid JSON";
        return;
    }

    QJsonObject msg = doc.object();
    QString type = msg["type"].toString();

    // Same input handling as DataChannelRelay::onInputMessage
    if (type == "keydown" || type == "keyup") {
        bool down = (type == "keydown");
        int vk = msg["keyCode"].toInt(0);
        QString code = msg["code"].toString();
        char mods = 0;
        if (msg["ctrlKey"].toBool(false))  mods |= 0x02;
        if (msg["shiftKey"].toBool(false)) mods |= 0x01;
        if (msg["altKey"].toBool(false))   mods |= 0x04;
        if (msg["metaKey"].toBool(false))  mods |= 0x08;

        short keyCode;
        char flags = 0;

        // International keys without standard US VK equivalents:
        // IntlBackslash (ISO key next to left Shift) and IntlRo (JIS \ key)
        // need raw scancode mode so Sunshine interprets them by physical
        // position instead of VK mapping.
        if (code == "IntlBackslash") {
            keyCode = 0x56;  // Windows Set 1 scancode for IntlBackslash
            flags = SS_KBE_FLAG_NON_NORMALIZED;
        } else if (code == "IntlRo") {
            keyCode = 0x73;  // Windows Set 1 scancode for IntlRo
            flags = SS_KBE_FLAG_NON_NORMALIZED;
        } else {
            keyCode = static_cast<short>(vk);
            flags = 0;
        }
        m_Shim->sendKeyEvent(keyCode, down, mods, flags);
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
    else if (type == "requestidr") {
        qInfo() << "[SignalingServer] Fallback input: requesting IDR frame";
        m_Shim->requestIdrFrame();
    }
    else {
        static int unknownInputCount = 0;
        if (++unknownInputCount <= 5) {
            qWarning() << "[SignalingServer] Fallback input: unknown type:" << type;
        }
    }
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

    // Check IPv4-mapped IPv6 addresses (::ffff:x.x.x.x).
    // QHostAddress reports IPv6Protocol for these, but toIPv4Address()
    // correctly returns the embedded IPv4 address portion.
    // Without this check, clients connecting via IPv6 from a private IPv4
    // subnet (e.g. a tunnel relaying IPv6 to localhost) would be classified
    // as "internet", forcing STUN/TURN unnecessarily.
    if (addr.protocol() == QAbstractSocket::IPv6Protocol) {
        quint32 ipv4 = addr.toIPv4Address();
        if (ipv4 != 0) {
            // Same private range checks against the embedded IPv4 address
            if ((ipv4 & 0xFF000000) == 0x0A000000) return true;
            if ((ipv4 & 0xFFF00000) == 0xAC100000) return true;
            if ((ipv4 & 0xFFFF0000) == 0xC0A80000) return true;
            if ((ipv4 & 0xFFFF0000) == 0xA9FE0000) return true;
        }
    }

    return false;
}

// ── UPnP NAT traversal ─────────────────────────────────────────────────────────

rtc::Configuration SignalingServer::buildIceConfig(bool isInternet, uint16_t upnpMappedPort, const QString& stunServerUrl, bool forceIceTcp)
{
    rtc::Configuration config;
    config.iceTransportPolicy = rtc::TransportPolicy::All;

    // forceMediaTransport defaults to false (libdatachannel default).
    // When true, it forces DTLS-SRTP (media transport) even for
    // DataChannel-only connections. The browser's answer has no media
    // tracks, so the SRTP-specific DTLS handshake would fail, causing
    // the PeerConnection to transition to "Failed" immediately after
    // ICE connectivity is established.
    //
    // Both relay types work correctly with the default:
    //   - DataChannelRelay: libdatachannel creates DTLS for SCTP automatically
    //   - MediaTrackRelay: the video track triggers SRTP negotiation implicitly
    // config.forceMediaTransport is NOT set here.

    // When UPnP is active, force libdatachannel to bind to the mapped port
    // in ALL modes (not just TCP or WAN). Without this, libdatachannel picks
    // an ephemeral port that the router's UPnP mapping cannot reach, and the
    // host candidate rewrite will point to a port nobody is listening on.
    if (upnpMappedPort > 0) {
        config.portRangeBegin = upnpMappedPort;
        config.portRangeEnd = upnpMappedPort;
    }

    if (forceIceTcp) {
        // TCP mode: enable ICE-TCP candidates so the browser has a TCP
        // fallback path if UDP is blocked by a restrictive firewall.
        // STUN is included to discover srflx addresses as well.
        config.enableIceTcp = true;
        config.iceServers.emplace_back(stunServerUrl.toStdString());

        qInfo() << "[SignalingServer] ICE config: TCP mode"
                << (upnpMappedPort > 0 ? "+ UPnP port=" + QString::number(upnpMappedPort) : "")
                << stunServerUrl;
    } else if (isInternet) {
        // UDP-only mode on WAN: no ICE-TCP. The auto fallback chain will
        // retry with TCP mode (forceIceTcp=true) if UDP-only fails.
        // STUN discovers the public srflx address.
        config.enableIceTcp = false;

        config.iceServers.emplace_back(stunServerUrl.toStdString());

        if (upnpMappedPort > 0) {
            qInfo() << "[SignalingServer] Internet ICE (UDP-only): UPnP port="
                    << upnpMappedPort << " + STUN";
        } else {
            qInfo() << "[SignalingServer] Internet ICE (UDP-only): STUN only (no UPnP)";
        }
    } else {
        // LAN: no STUN, no ICE-TCP. Host candidates are sufficient for
        // loopback/LAN connectivity. ICE-TCP is unnecessary and was found
        // to cause PeerConnection failures in some libdatachannel versions.
        config.enableIceTcp = false;
        if (upnpMappedPort > 0) {
            qInfo() << "[SignalingServer] LAN ICE: UPnP port=" << upnpMappedPort
                    << "(no STUN, no ICE-TCP)";
        } else {
            qInfo() << "[SignalingServer] LAN ICE: no STUN, no UPnP, no ICE-TCP";
        }
    }

    return config;
}

bool SignalingServer::setupUPnP()
{
    if (m_Upnp) {
        qInfo() << "[UPNP] Already set up";
        return true;
    }

    qInfo() << "[UPNP] Setting UPnP for signaling server (host=" << m_ServerHost << ")";

    auto* upnp = new UPNPClient(this);
    if (!upnp->discover(2000)) {
        qInfo() << "[UPNP] No IGD found — STUN fallback only";
        delete upnp;
        return false;
    }

    // Get public IP via UPnP (more reliable than STUN srflx for routing)
    std::string pubIP = upnp->getExternalIPAddress();
    if (!pubIP.empty()) {
        m_UpnpPublicIP = QString::fromStdString(pubIP);
        qInfo() << "[UPNP] Public IP:" << m_UpnpPublicIP;
    }

    // Try to add port mapping with fallback on port range
    uint16_t port = kUpnpPort;
    bool mappingOk = false;
    for (int attempt = 0; attempt < kUpnpMaxPortAttempts; attempt++) {
        uint16_t tryPort = port + static_cast<uint16_t>(attempt);
        if (upnp->addPortMapping(tryPort, tryPort,
                                 kUpnpLeaseDurationSec,
                                 "Moonlight-Web WebRTC")) {
            m_UpnpMappedPort = tryPort;
            mappingOk = true;
            break;
        }
        qWarning() << "[UPNP] Port" << tryPort << "failed, trying next...";
    }

    if (!mappingOk) {
        qWarning() << "[UPNP] All ports in range" << kUpnpPort << "-"
                   << (kUpnpPort + kUpnpMaxPortAttempts - 1) << "failed";
        delete upnp;
        return false;
    }

    // Store the UPNPClient instance — it will be cleaned up in cleanupUPnP()
    m_Upnp = upnp;

    // Schedule periodic renewal for routers without persistent mappings
    m_UpnpRenewTimer = new QTimer(this);
    connect(m_UpnpRenewTimer, &QTimer::timeout, this, [this]() {
        if (m_Upnp && m_UpnpMappedPort > 0) {
            qInfo() << "[UPNP] Renewing port mapping (every"
                    << (kUpnpRenewIntervalMs / 60000) << "min)";
            m_Upnp->addPortMapping(m_UpnpMappedPort, m_UpnpMappedPort,
                                   kUpnpLeaseDurationSec,
                                   "Moonlight-Web WebRTC");
        }
    });
    m_UpnpRenewTimer->start(kUpnpRenewIntervalMs);

    qInfo() << "[UPNP] Setup complete: port" << m_UpnpMappedPort
            << "mapped, public IP:" << m_UpnpPublicIP;
    return true;
}

void SignalingServer::cleanupUPnP()
{
    if (m_UpnpRenewTimer) {
        m_UpnpRenewTimer->stop();
        m_UpnpRenewTimer->deleteLater();
        m_UpnpRenewTimer = nullptr;
    }

    if (m_Upnp && m_UpnpMappedPort > 0) {
        qInfo() << "[UPNP] Removing port mapping" << m_UpnpMappedPort;
        m_Upnp->removePortMapping(m_UpnpMappedPort);
    }

    if (m_Upnp) {
        m_Upnp->deleteLater();
        m_Upnp = nullptr;
    }

    m_UpnpMappedPort = 0;
    m_UpnpPublicIP.clear();
}
