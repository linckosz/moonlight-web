#include "Session.h"
#include "DataChannelRelay.h"
#include "MediaTrackRelay.h"
#include "SignalingServer.h"
#include "StreamRelay.h"
#include "MoonlightShim.h"
#include "../backend/NvHTTP.h"
#include "../backend/NvComputer.h"
#include "../backend/IdentityManager.h"

extern "C" {
#include "Limelight.h"
}

#include <QCoreApplication>
#include <QJsonObject>
#include <QDebug>
#include <QUrl>

StreamSession::StreamSession(NvComputer* host, int appId,
                               NvHTTP* http, ResponseCallback respond,
                               quint16 wsPort,
                               const QString& serverHost,
                               VideoCodec videoCodec,
                               bool gamingMode,
                               bool upnpEnabled,
                               const QString& transport,
                               const QString& stunServer,
                               int streamHeight,
                               int streamFps,
                               int streamBitrateKbps,
                               QObject* parent)
    : QObject(parent)
    , m_Host(host)
    , m_AppId(appId)
    , m_Http(http)
    , m_Respond(std::move(respond))
    , m_WsPort(wsPort)
    , m_ServerHost(serverHost)
    , m_GamingMode(gamingMode)
    , m_UpnpEnabled(upnpEnabled)
    , m_Transport(transport)
    , m_StunServer(stunServer)
    , m_StreamHeight(streamHeight)
    , m_StreamFps(streamFps)
    , m_StreamBitrateKbps(streamBitrateKbps)
{
    // Apply video codec preference from settings (default Auto)
    m_Config.codec = videoCodec;
    qInfo() << "[Session] Video codec preference set to" << static_cast<int>(videoCodec);

    // Calculate width from height using 16:9 aspect ratio.
    // If height is 0 (Native Host resolution), pass 0 for both width and height
    // so Sunshine uses the display's native resolution.
    m_StreamWidth = (m_StreamHeight > 0) ? (m_StreamHeight * 16 / 9) : 0;

    qInfo() << "[Session] Stream settings:" << m_StreamWidth << "x" << m_StreamHeight
            << "@" << m_StreamFps << "fps, bitrate:" << m_StreamBitrateKbps << "kbps,"
            << "gaming:" << (m_GamingMode ? "on" : "off")
            << "codec:" << static_cast<int>(videoCodec);
}

StreamSession::~StreamSession()
{
    if (m_LaunchReply)
        m_LaunchReply->deleteLater();
}

void StreamSession::start()
{
    if (!m_Host) {
        m_Respond(HttpResponse::error(404, "Host not found"));
        emit sessionFailed("Host not found");
        deleteLater();
        return;
    }

    if (m_Host->pairState != NvComputer::PS_PAIRED) {
        m_Respond(HttpResponse::error(403, "Host is not paired"));
        emit sessionFailed("Host is not paired");
        deleteLater();
        return;
    }

    // Generate per-session encryption keys
    m_Config.generateKeys();

    auto* identity = IdentityManager::get();
    QByteArray clientCert = identity->getCertificate();
    QByteArray clientKey = identity->getPrivateKey();

    // Step 1: Force-quit any stale session on Sunshine before launching.
    //
    // If the browser tab was closed without a clean /quit (e.g. crash, tab kill),
    // Sunshine may still believe a session is active and reject the new /launch.
    // Sending /cancel first makes the start flow idempotent: any orphaned
    // session is torn down before we attempt to start a new one.
    //
    // HTTPS requests to Sunshine are serialized per host (the same QNAManager
    // is used for both quit and launch), so the quit MUST complete before the
    // launch begins. We chain them via the reply's finished signal.
    //
    // The quit is best-effort: if it fails (no session running, network blip,
    // etc.) we log a warning and proceed with the launch anyway.
    qInfo() << "[Session] Pre-start: force-quitting any stale session on"
            << m_Host->name << m_Host->activeAddress.address();

    QNetworkReply* quitReply = m_Http->quitAppAsync(
        m_Host->activeAddress, m_Host->activeHttpsPort,
        clientCert, clientKey);

    connect(quitReply, &QNetworkReply::finished, this,
            [this, quitReply, clientCert, clientKey]() {
        quitReply->deleteLater();

        if (quitReply->error() != QNetworkReply::NoError) {
            qWarning() << "[Session] Pre-start quit: network error"
                       << quitReply->errorString()
                       << "(non-fatal, continuing with launch)";
        } else {
            QByteArray body = quitReply->readAll();
            qInfo() << "[Session] Pre-start quit: OK, response:"
                    << body.left(200);
        }

        // Step 2: Launch the app (HTTPS serialized: quit finished first)
        doLaunchApp(clientCert, clientKey);
    });
}

void StreamSession::doLaunchApp(const QByteArray& clientCert,
                                const QByteArray& clientKey)
{
    qDebug() << "[Session] Launching app" << m_AppId << "on" << m_Host->name;
    qDebug() << "[Session]   address:" << m_Host->activeAddress.address()
             << "port:" << m_Host->activeHttpsPort;
    qDebug() << "[Session]   stream:" << m_StreamWidth << "x" << m_StreamHeight
             << "@" << m_StreamFps << "fps, bitrate:" << m_StreamBitrateKbps << "kbps";

    m_LaunchReply = m_Http->launchAppAsync(
        m_Host->activeAddress, m_Host->activeHttpsPort,
        m_AppId, IdentityManager::get()->getUniqueId(),
        m_Config.rikey, m_Config.rikeyid,
        m_StreamWidth, m_StreamHeight, m_StreamFps,
        m_StreamBitrateKbps,
        clientCert, clientKey,
        (m_Config.hdr == HdrMode::HDR) ? 1 : 0);

    connect(m_LaunchReply, &QNetworkReply::finished,
            this, &StreamSession::onLaunchReplyFinished);
}

void StreamSession::quit()
{
    qInfo() << "[Session::quit] ENTER, m_Shim=" << m_Shim
            << "m_Relay=" << m_Relay << "m_MediaTrackRelay=" << m_MediaTrackRelay
            << "m_Signaling=" << m_Signaling
            << "m_StreamRelay=" << m_StreamRelay
            << "m_Connected=" << (m_Shim ? m_Shim->isConnected() : false)
            << "transport=" << m_Transport;

    if (m_Transport == "wss") {
        // WSS mode: stop StreamRelay (closes WS server + client)
        if (m_StreamRelay) {
            qInfo() << "[Session::quit] Calling m_StreamRelay->stop() ...";
            m_StreamRelay->stop();
            qInfo() << "[Session::quit] m_StreamRelay->stop() returned";
        } else {
            qInfo() << "[Session::quit] No m_StreamRelay to stop";
        }
    } else if (m_Transport == "webrtc-media") {
        // WebRTC Media Track mode: stop SignalingServer first
        if (m_Signaling) {
            qInfo() << "[Session::quit] Calling m_Signaling->stop() ...";
            m_Signaling->stop();
            qInfo() << "[Session::quit] m_Signaling->stop() returned";
        } else {
            qInfo() << "[Session::quit] No m_Signaling to stop";
        }

        // Stop MediaTrackRelay (closes PeerConnection + tracks)
        if (m_MediaTrackRelay) {
            qInfo() << "[Session::quit] Calling m_MediaTrackRelay->stop() ...";
            m_MediaTrackRelay->stop();
            qInfo() << "[Session::quit] m_MediaTrackRelay->stop() returned";
        } else {
            qInfo() << "[Session::quit] No m_MediaTrackRelay to stop";
        }
    } else {
        // WebRTC DataChannel mode: stop SignalingServer first (closes WS, stops signaling)
        if (m_Signaling) {
            qInfo() << "[Session::quit] Calling m_Signaling->stop() ...";
            m_Signaling->stop();
            qInfo() << "[Session::quit] m_Signaling->stop() returned";
        } else {
            qInfo() << "[Session::quit] No m_Signaling to stop";
        }

        // Stop DataChannelRelay (closes PeerConnection + DataChannels)
        if (m_Relay) {
            qInfo() << "[Session::quit] Calling m_Relay->stop() ...";
            m_Relay->stop();
            qInfo() << "[Session::quit] m_Relay->stop() returned";
        } else {
            qInfo() << "[Session::quit] No m_Relay to stop";
        }
    }

    // Stop MoonlightShim last (calls LiStopConnection)
    if (m_Shim) {
        qInfo() << "[Session::quit] Calling m_Shim->stopConnection() ...";
        m_Shim->stopConnection();
        qInfo() << "[Session::quit] m_Shim->stopConnection() returned";
    } else {
        qInfo() << "[Session::quit] No m_Shim to stop";
    }

    qInfo() << "[Session::quit] EXIT";
}

void StreamSession::onLaunchReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    reply->deleteLater();
    m_LaunchReply = nullptr;

    if (reply->error() != QNetworkReply::NoError) {
        int code = static_cast<int>(reply->error());
        qWarning() << "[Session] Launch error code:" << code << "-" << reply->errorString();

        QString detail = QString("Launch failed: [code=%1] %2 (target: %3:%4)")
                             .arg(code)
                             .arg(reply->errorString())
                             .arg(m_Host->activeAddress.address())
                             .arg(m_Host->activeHttpsPort);
        m_Respond(HttpResponse::error(502, detail));
        emit sessionFailed(reply->errorString());
        deleteLater();
        return;
    }

    QByteArray data = reply->readAll();
    QString xml = QString::fromUtf8(data);

    try {
        NvHTTP::verifyResponseStatus(xml);
    } catch (const std::runtime_error& e) {
        m_Respond(HttpResponse::error(502, QString("Launch error: %1").arg(e.what())));
        emit sessionFailed(e.what());
        deleteLater();
        return;
    }

    QString sessionUrl = NvHTTP::parseSessionUrl(xml);
    if (sessionUrl.isEmpty()) {
        m_Respond(HttpResponse::error(502, "No session URL in launch response"));
        emit sessionFailed("No session URL");
        deleteLater();
        return;
    }

    QUrl rtspUrl(sessionUrl);
    if (!rtspUrl.isValid()) {
        m_Respond(HttpResponse::error(502, "Invalid RTSP URL: " + sessionUrl));
        emit sessionFailed("Invalid RTSP URL");
        deleteLater();
        return;
    }

    m_SessionUrl = sessionUrl;

    // Build InitParams for MoonlightShim
    MoonlightShim::InitParams params;
    params.hostAddress = m_Host->activeAddress.address();
    params.appVersion = m_Host->appVersion;
    params.gfeVersion = m_Host->gfeVersion;
    params.rtspSessionUrl = m_SessionUrl;
    params.serverCodecModeSupport = m_Host->serverCodecModeSupport;
    params.aesKey = m_Config.rikey;
    params.rikeyid = m_Config.rikeyid;

    // Video formats: codec preference bitmap computed from StreamConfig.
    // Default is HEVC preferred with H.264 fallback.
    params.supportedVideoFormats = m_Config.computeVideoFormats();

    // Pass color space from config (BT.709 SDR or BT.2020 HDR)
    params.colorSpace = m_Config.computeColorSpace();

    // Audio: stereo Opus matching StreamConfig
    params.audioConfiguration = MAKE_AUDIO_CONFIGURATION(
        StreamConfig::kAudioChannels,
        StreamConfig::kAudioChannelMask);

    // Create MoonlightShim BEFORE starting LiStartConnection. The relay must
    // be connected to all signals before frames arrive.
    m_Shim = new MoonlightShim(this);

    // Branch: WSS (legacy StreamRelay) or WebRTC (DataChannelRelay + SignalingServer)
    if (m_Transport == "wss") {
        // ── Legacy WSS mode: uses plain WebSocket StreamRelay ──────────────
        qInfo() << "[Session] Transport=wss — using legacy StreamRelay";

        auto* streamRelay = new StreamRelay(m_Shim, m_StreamRelayPort, {}, this);
        streamRelay->setServerHost(m_ServerHost);
        streamRelay->setHttpsPort(m_HttpsPort);

        if (!streamRelay->start()) {
            delete streamRelay;
            delete m_Shim;
            m_Shim = nullptr;
            m_Respond(HttpResponse::error(500, "Failed to start StreamRelay"));
            emit sessionFailed("StreamRelay failed to start");
            deleteLater();
            return;
        }

        connect(m_Shim, &MoonlightShim::connectionStarted,
                this, &StreamSession::onShimConnectionStarted);
        connect(m_Shim, &MoonlightShim::connectionFailed,
                this, &StreamSession::onShimConnectionFailed);

        // StreamRelay must outlive StreamSession
        streamRelay->setParent(QCoreApplication::instance());
        m_Shim->setParent(streamRelay);

        connect(streamRelay, &StreamRelay::sessionEnded, this, [this]() {
            quit();
            deleteLater();
        });

        m_StreamRelay = streamRelay;
        emit streamRelayCreated(streamRelay);

        qDebug() << "[Session] StreamRelay created, starting LiStartConnection...";
        m_Shim->startConnection(params);
    } else if (m_Transport == "webrtc-media") {
        // ── WebRTC Media Track mode: MediaTrackRelay + SignalingServer ─────────
        qInfo() << "[Session] Transport=webrtc-media — using MediaTrackRelay";

        // MediaTrackRelay: owns the libdatachannel PeerConnection + video track + audio/input DCs
        auto* relay = new MediaTrackRelay(m_Shim, this);

        // SignalingServer: WebSocket for SDP/ICE exchange only.
        auto* signaling = new SignalingServer(relay, m_WsPort, m_ServerHost, this);
        signaling->setHttpsPort(m_HttpsPort);
        signaling->setUseUPnP(m_UpnpEnabled);
        signaling->setStunServer(m_StunServer);

        // If an explicit WS URL was set (e.g. public tunnel), apply it.
        if (!m_ExplicitWsUrl.isEmpty()) {
            signaling->setOverrideWsUrl(m_ExplicitWsUrl);
            qInfo() << "[Session] Using explicit signaling URL:" << m_ExplicitWsUrl;
        }

        if (!signaling->start()) {
            delete signaling;
            delete relay;
            delete m_Shim;
            m_Shim = nullptr;
            m_Respond(HttpResponse::error(500, "Failed to start signaling server"));
            emit sessionFailed("Signaling server failed to start");
            deleteLater();
            return;
        }

        // Connect session-level handlers
        connect(m_Shim, &MoonlightShim::connectionStarted,
                this, &StreamSession::onShimConnectionStarted);
        connect(m_Shim, &MoonlightShim::connectionFailed,
                this, &StreamSession::onShimConnectionFailed);

        // Relay must outlive StreamSession
        relay->setParent(QCoreApplication::instance());
        signaling->setParent(relay);
        m_Shim->setParent(relay);

        // Clean up when relay session ends (media track disconnect, error, etc.)
        connect(relay, &MediaTrackRelay::sessionEnded, this, [this]() {
            quit();
            deleteLater();
        });

        m_MediaTrackRelay = relay;
        m_Signaling = signaling;
        emit mediaTrackRelayCreated(relay);

        qDebug() << "[Session] MediaTrackRelay + Signaling created, starting LiStartConnection...";
        m_Shim->startConnection(params);
    } else {
        // ── WebRTC mode: DataChannelRelay + SignalingServer (default) ─────
        qInfo() << "[Session] Transport=webrtc — using DataChannelRelay";

        // DataChannelRelay: owns the libdatachannel PeerConnection + DataChannels
        auto* relay = new DataChannelRelay(m_Shim, this);

        // SignalingServer: WebSocket for SDP/ICE exchange only.
        // NonSecure mode: external tunnel or Cloudflare provides TLS termination.
        auto* signaling = new SignalingServer(relay, m_WsPort, m_ServerHost, this);
        signaling->setHttpsPort(m_HttpsPort);
        signaling->setUseUPnP(m_UpnpEnabled);
        signaling->setStunServer(m_StunServer);

        // If an explicit WS URL was set (e.g. public tunnel), apply it.
        if (!m_ExplicitWsUrl.isEmpty()) {
            signaling->setOverrideWsUrl(m_ExplicitWsUrl);
            qInfo() << "[Session] Using explicit signaling URL:" << m_ExplicitWsUrl;
        }

        if (!signaling->start()) {
            delete signaling;
            delete relay;
            delete m_Shim;
            m_Shim = nullptr;
            m_Respond(HttpResponse::error(500, "Failed to start signaling server"));
            emit sessionFailed("Signaling server failed to start");
            deleteLater();
            return;
        }

        // Now connect the session-level handlers
        connect(m_Shim, &MoonlightShim::connectionStarted,
                this, &StreamSession::onShimConnectionStarted);
        connect(m_Shim, &MoonlightShim::connectionFailed,
                this, &StreamSession::onShimConnectionFailed);

        // Relay must outlive StreamSession
        relay->setParent(QCoreApplication::instance());
        signaling->setParent(relay);  // Signaling is a child of relay
        m_Shim->setParent(relay);

        // Clean up when relay session ends (DataChannel disconnect, error, etc.)
        connect(relay, &DataChannelRelay::sessionEnded, this, [this]() {
            quit();
            deleteLater();
        });

        // Provide MoonlightShim reference for WS fallback (ICE timeout → WS data transport)
        signaling->setMoonlightShim(m_Shim);

        m_Relay = relay;
        m_Signaling = signaling;
        emit relayCreated(relay);

        qDebug() << "[Session] Relay + Signaling created, starting LiStartConnection...";
        m_Shim->startConnection(params);
    }
}

void StreamSession::onShimConnectionStarted()
{
    qDebug() << "[Session] LiStartConnection succeeded — sending response to browser";

    QJsonObject result;
    result["status"] = "streaming";
    result["sessionUrl"] = m_SessionUrl;
    result["transport"] = m_Transport;

    if (m_Transport == "wss") {
        // WSS mode: provide the StreamRelay WS URL directly
        result["wsUrl"] = m_StreamRelay ? m_StreamRelay->wsUrl() : QString();
        result["signalingUrl"] = QString();
    } else {
        // WebRTC mode: provide the signaling WS URL for SDP/ICE exchange
        result["signalingUrl"] = m_Signaling ? m_Signaling->wsUrl() : QString();
    }

    // Pass gaming mode preference to the frontend
    result["gamingMode"] = m_GamingMode;

    // UPnP NAT traversal status
    if (m_Signaling) {
        bool upnpAvailable = (m_Signaling->upnpMappedPort() > 0);
        result["upnpAvailable"] = upnpAvailable;
        if (upnpAvailable) {
            result["upnpPublicIP"] = m_Signaling->upnpPublicIP();
            result["upnpPort"] = static_cast<int>(m_Signaling->upnpMappedPort());
        }
    }

    // Report the negotiated video codec back to the browser
    switch (m_Config.codec) {
    case VideoCodec::H264: result["videoCodec"] = "h264"; break;
    case VideoCodec::HEVC: result["videoCodec"] = "hevc"; break;
    case VideoCodec::AV1:  result["videoCodec"] = "av1";  break;
    default:               result["videoCodec"] = "auto";  break;
    }

    m_Respond(HttpResponse::json(result));
    emit sessionStarted();

    // StreamSession is ephemeral — relay lives on
    deleteLater();
}

void StreamSession::onShimConnectionFailed(const QString& error)
{
    qWarning() << "[Session] LiStartConnection failed:" << error;

    // Best-effort quit via HTTPS
    quit();

    m_Respond(HttpResponse::error(502, error));
    emit sessionFailed(error);
    deleteLater();
}
