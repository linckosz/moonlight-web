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

#include "Session.h"
#include "ClipboardBridge.h"
#include "DataChannelRelay.h"
#include "MediaTrackRelay.h"
#include "SignalingServer.h"
#include "StreamRelay.h"
#include "MoonlightShim.h"
#include "../backend/NvHTTP.h"
#include "../backend/NvComputer.h"
#include "../backend/IdentityManager.h"
#include "../server/AppSettings.h"

extern "C" {
#include "Limelight.h"
}

#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QUrl>
#include <QThread>
#include <QMetaObject>

QSet<QString> StreamSession::s_ActiveUniqueIds;

namespace {
// Move a relay graph (relay + its child signaling server + child MoonlightShim)
// onto a dedicated per-session thread so all media/WS send + fragmentation work
// runs off the Qt main thread (which hosts the HTTP/REST/control plane). This
// isolates the streaming pipeline for both fluidity (no contention with HTTP)
// and stability (a stall in the media path can't freeze the control server).
// Teardown is self-managed: when the relay is destroyed the thread quits and
// deletes itself. The relay must have no QObject parent before this call.
QThread* spawnRelayThread(QObject* relayRoot)
{
    QThread* t = new QThread();
    t->setObjectName(QStringLiteral("RelaySessionThread"));
    relayRoot->moveToThread(t);
    QObject::connect(relayRoot, &QObject::destroyed, t, &QThread::quit);
    QObject::connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
    return t;
}
} // namespace

StreamSession::StreamSession(NvComputer* host, int appId, NvHTTP* http, ResponseCallback respond,
                             quint16 wsPort, const QString& serverHost, VideoCodec videoCodec,
                             bool gamingMode, bool upnpEnabled, const QString& transport,
                             const QString& stunServer, int streamHeight, int streamWidth,
                             int streamFps, int streamBitrateKbps, bool yuv444, bool hdrEnabled,
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
    , m_TransportMode(transport) // default = internal transport; set explicitly for auto mode
    , m_StunServer(stunServer)
    , m_StreamHeight(streamHeight)
    , m_StreamFps(streamFps)
    , m_StreamBitrateKbps(streamBitrateKbps)
{
    // Apply video codec preference from settings (default Auto)
    m_Config.codec = videoCodec;
    qInfo() << "[Session] Video codec preference set to" << static_cast<int>(videoCodec);

    // WebRTC MediaTrack transport carries video over a native RTP video track that
    // libdatachannel only packetizes as H.264 (MediaTrackRelay::createTracksAndChannels
    // uses H264RtpPacketizer, PT=96). Sunshine must therefore encode H.264, otherwise
    // it negotiates its preferred HEVC and the browser's <video> decoder receives HEVC
    // NAL units on an H.264-declared track — the stream never starts. Force H.264 here
    // when the user picked HEVC/AV1/Auto and record the override so the frontend is told
    // the original selection (onShimConnectionStarted reports codecOverridden/originalCodec).
    if (m_Transport == "webrtc-media" && videoCodec != VideoCodec::H264) {
        m_CodecOverridden = true;
        m_OriginalCodec = videoCodec;
        m_Config.codec = VideoCodec::H264;
        qInfo() << "[Session] webrtc-media transport — forcing H.264 (original codec:"
                << static_cast<int>(videoCodec) << ")";
    }

    // YUV 4:4:4 chroma: adds the YUV444 profile flags to the negotiated formats
    // (full chroma resolution, higher bandwidth). Off by default.
    m_Config.chroma = yuv444 ? ChromaSampling::C444 : ChromaSampling::C420;
    qInfo() << "[Session] Chroma 4:4:4" << (yuv444 ? "enabled" : "disabled");

    // HDR: enables HDR10 color space (BT.2020+PQ) and 10-bit codec profiles.
    m_Config.hdrEnabled = hdrEnabled;

    // Width: explicit when provided (ultrawide 21:9 / 32:9), otherwise derived
    // from height using a 16:9 aspect ratio.
    // If height is 0 (Native Host resolution), pass 0 for both width and height
    // so Sunshine uses the display's native resolution.
    m_StreamWidth =
        (streamWidth > 0) ? streamWidth : ((m_StreamHeight > 0) ? (m_StreamHeight * 16 / 9) : 0);

    qInfo() << "[Session] Stream settings:" << m_StreamWidth << "x" << m_StreamHeight << "@"
            << m_StreamFps << "fps, bitrate:" << m_StreamBitrateKbps << "kbps,"
            << "gaming:" << (m_GamingMode ? "on" : "off")
            << "codec:" << static_cast<int>(videoCodec) << "hdr:" << hdrEnabled;
}

StreamSession::~StreamSession()
{
    if (m_LaunchReply) m_LaunchReply->deleteLater();
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

    // Start the app without force-quitting other sessions.
    //
    // Sunshine supports concurrent sessions (e.g. iOS + Moonlight-QT on the same
    // host), keyed per client uniqueid. Launching with our own per-browser
    // uniqueid creates an independent session and leaves any other client's
    // session running. We deliberately do NOT send a pre-start /cancel, which
    // would tear down whatever session is currently active on the host.
    //
    // If this same client already has a live session we launched (e.g. a reload
    // that didn't /quit cleanly), /resume reconnects to OUR own session instead
    // of /launch (which would be rejected with "app already running"). /resume
    // is keyed by uniqueid, so it never touches another client's session.
    if (s_ActiveUniqueIds.contains(effectiveUniqueId())) {
        qInfo() << "[Session] uniqueid already has a session — resuming" << m_Host->name
                << m_Host->activeAddress.address();
        doResumeApp(clientCert, clientKey);
    } else {
        doLaunchApp(clientCert, clientKey);
    }
}

QString StreamSession::effectiveUniqueId() const
{
    return m_ClientUniqueId.isEmpty() ? IdentityManager::get()->getUniqueId() : m_ClientUniqueId;
}

void StreamSession::doResumeApp(const QByteArray& clientCert, const QByteArray& clientKey)
{
    qInfo() << "[Session] Resuming session on" << m_Host->name << "appid=" << m_AppId;
    m_ResumeAttempted = true;
    m_LaunchReply = m_Http->resumeAppAsync(m_Host->activeAddress, m_Host->activeHttpsPort,
                                           effectiveUniqueId(), m_Config.rikey, m_Config.rikeyid,
                                           clientCert, clientKey, m_Config.muteHostAudio ? 0 : 1);

    connect(m_LaunchReply, &QNetworkReply::finished, this, &StreamSession::onLaunchReplyFinished);
}

void StreamSession::doLaunchApp(const QByteArray& clientCert, const QByteArray& clientKey)
{
    qDebug() << "[Session] Launching app" << m_AppId << "on" << m_Host->name;
    qDebug() << "[Session]   address:" << m_Host->activeAddress.address()
             << "port:" << m_Host->activeHttpsPort;
    qDebug() << "[Session]   stream:" << m_StreamWidth << "x" << m_StreamHeight << "@"
             << m_StreamFps << "fps, bitrate:" << m_StreamBitrateKbps << "kbps,"
             << "hdr:" << m_Config.hdrEnabled;

    m_LaunchAttempted = true;
    m_LaunchReply = m_Http->launchAppAsync(m_Host->activeAddress, m_Host->activeHttpsPort, m_AppId,
                                           effectiveUniqueId(), m_Config.rikey, m_Config.rikeyid,
                                           m_StreamWidth, m_StreamHeight, m_StreamFps,
                                           m_StreamBitrateKbps, clientCert, clientKey,
                                           m_Config.hdrEnabled ? 1 : 0,     // hdrMode: 1=HDR, 0=SDR
                                           m_Config.muteHostAudio ? 0 : 1); // localAudioPlayMode

    connect(m_LaunchReply, &QNetworkReply::finished, this, &StreamSession::onLaunchReplyFinished);
}

void StreamSession::quit()
{
    qInfo() << "[Session::quit] ENTER, m_Shim=" << m_Shim << "m_Relay=" << m_Relay
            << "m_MediaTrackRelay=" << m_MediaTrackRelay << "m_Signaling=" << m_Signaling
            << "m_StreamRelay=" << m_StreamRelay
            << "m_Connected=" << (m_Shim ? m_Shim->isConnected() : false)
            << "transport=" << m_Transport;

    // Forget this uniqueid: the session is being torn down (a /cancel to Sunshine
    // follows on the quit paths). A later start() launches fresh rather than
    // resuming. If the hint ends up stale either way, start() self-heals.
    s_ActiveUniqueIds.remove(effectiveUniqueId());

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

    // Delete the relay graph now that the shim's LiStopConnection has been
    // initiated while the relay is still fully alive (stopConnection() calls
    // LiInterruptConnection first, so moonlight stops invoking relay callbacks
    // before destruction — same ordering as the /quit and take-over paths).
    // The SignalingServer and MoonlightShim are children of the relay and are
    // destroyed with it. quit() is the SINGLE teardown owner for the disconnect
    // path: main.cpp's sessionEnded handler only sends the Sunshine /quit and no
    // longer deletes the relay — its deleteLater() used to cancel THIS handler
    // (sender destroyed), deferring LiStopConnection into the relay destructor
    // where late moonlight callbacks hit a half-destroyed relay (UAF crash).
    if (m_StreamRelay) {
        m_StreamRelay->deleteLater();
        m_StreamRelay = nullptr;
    }
    if (m_MediaTrackRelay) {
        m_MediaTrackRelay->deleteLater();
        m_MediaTrackRelay = nullptr;
    }
    if (m_Relay) {
        m_Relay->deleteLater();
        m_Relay = nullptr;
    }
    m_Signaling = nullptr; // child of the relay — deleted with it
    m_Shim = nullptr;      // child of the relay — deleted with it

    qInfo() << "[Session::quit] EXIT";
}

void StreamSession::onLaunchReplyFinished()
{
    QNetworkReply* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    reply->deleteLater();
    m_LaunchReply = nullptr;

    // Drop the launch/resume TLS socket now: leaving it pooled ~120s would hold
    // Sunshine's single-threaded HTTPS server and block new iOS/Qt connections
    // for the whole stream. Polling is suspended during a stream, so this is safe.
    m_Http->dropPooledConnections();

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
        // The registry hint can be stale (orphaned session after a backend
        // restart, or a cleared entry whose Sunshine session is gone). Self-heal
        // by trying the other path once. Both are keyed by our uniqueid, so a
        // /resume can only reconnect to OUR session, never another client's.
        auto* identity = IdentityManager::get();
        if (!m_ResumeAttempted) {
            qWarning() << "[Session] Launch rejected (" << e.what()
                       << ") — falling back to /resume for our uniqueid";
            doResumeApp(identity->getCertificate(), identity->getPrivateKey());
            return;
        }
        if (!m_LaunchAttempted) {
            qWarning() << "[Session] Resume rejected (" << e.what()
                       << ") — falling back to /launch";
            s_ActiveUniqueIds.remove(effectiveUniqueId()); // stale hint
            doLaunchApp(identity->getCertificate(), identity->getPrivateKey());
            return;
        }
        // Sunshine 503 = the host could not initialize video capture/encoding.
        // The most common macOS cause is a missing Screen Recording permission
        // (Sunshine logs "No screen capture permission!"); elsewhere it's a
        // sleeping/disconnected display with no dummy plug. Surface a clear,
        // actionable message instead of the opaque "502 Launch error: HTTP 503".
        QString reason = QString::fromUtf8(e.what());
        if (reason.contains("503")) {
            // Machine-readable `code` lets the frontend show an explicit
            // "grant Screen Recording" dialog (with a button to open the macOS
            // settings pane) instead of a plain toast.
            QJsonObject err;
            err["error"] =
                QString("Host \"%1\" could not start video capture. On macOS, grant "
                        "Sunshine the \"Screen Recording\" permission in System Settings "
                        "> Privacy & Security > Screen Recording, then quit and reopen "
                        "Sunshine. Otherwise, make sure a display is connected and powered "
                        "on (or use an HDMI dummy plug).")
                    .arg(m_Host->name);
            err["code"] = QStringLiteral("video_capture_failed");
            err["status"] = 503;
            m_Respond(HttpResponse::json(err, 503));
        } else {
            m_Respond(HttpResponse::error(502, QString("Launch error: %1").arg(reason)));
        }
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

    // Session is now live on Sunshine for this uniqueid: remember it so a later
    // reconnect (reload) resumes instead of relaunching. Cleared in quit().
    s_ActiveUniqueIds.insert(effectiveUniqueId());

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

    // Pass color space from config (BT.709 SDR)
    params.colorSpace = m_Config.computeColorSpace();

    // Audio: stereo Opus matching StreamConfig
    params.audioConfiguration =
        MAKE_AUDIO_CONFIGURATION(StreamConfig::kAudioChannels, StreamConfig::kAudioChannelMask);
    // Mobile clients request 10ms Opus frames (half the packet rate) to ease
    // transmission on constrained networks.
    params.slowOpus = m_LowAudio;

    // Stream settings from user preferences
    params.width = m_StreamWidth;
    params.height = m_StreamHeight;
    params.fps = m_StreamFps;
    params.bitrateKbps = m_StreamBitrateKbps;

    // Create MoonlightShim BEFORE starting LiStartConnection. The relay must
    // be connected to all signals before frames arrive.
    m_Shim = new MoonlightShim(this);

    // Clipboard sync is only possible when the streamed host is this machine
    // (standard installer deployment: backend runs next to Sunshine) — the
    // backend clipboard is then the host's. For remote Sunshine hosts the
    // relay stays clipboard-silent and the browser keeps today's behavior.
    const bool clipboardLocal = ClipboardBridge::isSelfAddress(m_Host->activeAddress.address());
    qInfo() << "[Session] Clipboard sync" << (clipboardLocal ? "enabled" : "disabled")
            << "(host address:" << m_Host->activeAddress.address() << ")";

    // Branch: WSS (legacy StreamRelay) or WebRTC (DataChannelRelay + SignalingServer)
    if (m_Transport == "wss") {
        // ── Legacy WSS mode: uses plain WebSocket StreamRelay ──────────────
        qInfo() << "[Session] Transport=wss — using legacy StreamRelay";

        auto* streamRelay = new StreamRelay(m_Shim, m_StreamRelayPort, {}, nullptr);
        streamRelay->setServerHost(m_ServerHost);
        streamRelay->setHttpsPort(m_HttpsPort);
        streamRelay->setClipboardEnabled(clipboardLocal);

        connect(m_Shim, &MoonlightShim::connectionStarted, this,
                &StreamSession::onShimConnectionStarted);
        connect(m_Shim, &MoonlightShim::connectionFailed, this,
                &StreamSession::onShimConnectionFailed);

        // The shim migrates to the relay's dedicated thread with it (child).
        m_Shim->setParent(streamRelay);

        connect(streamRelay, &StreamRelay::sessionEnded, this, [this]() {
            quit();
            deleteLater();
        });

        // Move the relay graph onto a dedicated per-session thread: all WS
        // send + fragmentation runs off the Qt main thread (HTTP/control).
        spawnRelayThread(streamRelay);

        // start() binds the WS listener and must run on the thread that owns the
        // socket. Block only to retrieve the success/failure result.
        bool started = false;
        QMetaObject::invokeMethod(
            streamRelay, [&]() { started = streamRelay->start(); }, Qt::BlockingQueuedConnection);
        if (!started) {
            streamRelay->deleteLater(); // also deletes the shim (its child)
            m_Shim = nullptr;
            m_Respond(HttpResponse::error(500, "Failed to start StreamRelay"));
            emit sessionFailed("StreamRelay failed to start");
            deleteLater();
            return;
        }

        m_StreamRelay = streamRelay;
        emit streamRelayCreated(streamRelay);

        qDebug() << "[Session] StreamRelay created, starting LiStartConnection...";
        MoonlightShim* shim = m_Shim;
        QMetaObject::invokeMethod(
            shim, [shim, params]() { shim->startConnection(params); }, Qt::QueuedConnection);
    } else if (m_Transport == "webrtc-media") {
        // ── WebRTC Media Track mode: MediaTrackRelay + SignalingServer ─────────
        qInfo() << "[Session] Transport=webrtc-media — using MediaTrackRelay";

        // MediaTrackRelay: owns the libdatachannel PeerConnection + video track + audio/input DCs.
        // No QObject parent: it is moved onto a dedicated thread below.
        auto* relay = new MediaTrackRelay(m_Shim, nullptr);
        relay->setClipboardEnabled(clipboardLocal);

        // SignalingServer: WebSocket for SDP/ICE exchange only.
        auto* signaling = new SignalingServer(relay, m_WsPort, m_ServerHost, nullptr);
        signaling->setHttpsPort(m_HttpsPort);
        signaling->setUseUPnP(m_UpnpEnabled);
        signaling->setStunServer(m_StunServer);
        signaling->setEnableIceTcp(m_EnableIceTcp);
        signaling->setAllowWsFallback(!m_AutoMode);
        signaling->setClientIsLocal(m_ClientIsLocal);

        // If an explicit WS URL was set (e.g. public tunnel), apply it.
        if (!m_ExplicitWsUrl.isEmpty()) {
            signaling->setOverrideWsUrl(m_ExplicitWsUrl);
            qInfo() << "[Session] Using explicit signaling URL:" << m_ExplicitWsUrl;
        }

        // Connect session-level handlers
        connect(m_Shim, &MoonlightShim::connectionStarted, this,
                &StreamSession::onShimConnectionStarted);
        connect(m_Shim, &MoonlightShim::connectionFailed, this,
                &StreamSession::onShimConnectionFailed);

        // signaling + shim migrate to the relay's dedicated thread (children).
        signaling->setParent(relay);
        m_Shim->setParent(relay);

        // Clean up when relay session ends (media track disconnect, error, etc.)
        connect(relay, &MediaTrackRelay::sessionEnded, this, [this]() {
            quit();
            deleteLater();
        });

        // Move the relay graph onto a dedicated per-session thread.
        spawnRelayThread(relay);

        // signaling->start() binds the WS listener on the relay thread that owns
        // the socket. Block only to retrieve the success/failure result.
        bool started = false;
        QMetaObject::invokeMethod(
            signaling, [&]() { started = signaling->start(); }, Qt::BlockingQueuedConnection);
        if (!started) {
            relay->deleteLater(); // also deletes signaling + shim (its children)
            m_Shim = nullptr;
            m_Respond(HttpResponse::error(500, "Failed to start signaling server"));
            emit sessionFailed("Signaling server failed to start");
            deleteLater();
            return;
        }

        m_MediaTrackRelay = relay;
        m_Signaling = signaling;
        emit mediaTrackRelayCreated(relay);

        qDebug() << "[Session] MediaTrackRelay + Signaling created, starting LiStartConnection...";
        MoonlightShim* shim = m_Shim;
        QMetaObject::invokeMethod(
            shim, [shim, params]() { shim->startConnection(params); }, Qt::QueuedConnection);
    } else {
        // ── WebRTC mode: DataChannelRelay + SignalingServer (default) ─────
        qInfo() << "[Session] Transport=webrtc — using DataChannelRelay";

        // DataChannelRelay: owns the libdatachannel PeerConnection + DataChannels.
        // No QObject parent: it is moved onto a dedicated thread below.
        auto* relay = new DataChannelRelay(m_Shim, nullptr);
        relay->setClipboardEnabled(clipboardLocal);

        // SignalingServer: WebSocket for SDP/ICE exchange only.
        // NonSecure mode: external tunnel or Cloudflare provides TLS termination.
        auto* signaling = new SignalingServer(relay, m_WsPort, m_ServerHost, nullptr);
        signaling->setHttpsPort(m_HttpsPort);
        signaling->setUseUPnP(m_UpnpEnabled);
        signaling->setStunServer(m_StunServer);
        signaling->setEnableIceTcp(m_EnableIceTcp);
        signaling->setAllowWsFallback(!m_AutoMode);
        signaling->setClientIsLocal(m_ClientIsLocal);

        // If an explicit WS URL was set (e.g. public tunnel), apply it.
        if (!m_ExplicitWsUrl.isEmpty()) {
            signaling->setOverrideWsUrl(m_ExplicitWsUrl);
            qInfo() << "[Session] Using explicit signaling URL:" << m_ExplicitWsUrl;
        }

        // Connect the session-level handlers
        connect(m_Shim, &MoonlightShim::connectionStarted, this,
                &StreamSession::onShimConnectionStarted);
        connect(m_Shim, &MoonlightShim::connectionFailed, this,
                &StreamSession::onShimConnectionFailed);

        // signaling + shim migrate to the relay's dedicated thread (children).
        signaling->setParent(relay); // Signaling is a child of relay
        m_Shim->setParent(relay);

        // Clean up when relay session ends (DataChannel disconnect, error, etc.)
        connect(relay, &DataChannelRelay::sessionEnded, this, [this]() {
            quit();
            deleteLater();
        });

        // Provide MoonlightShim reference for WS fallback (ICE timeout → WS data transport)
        signaling->setMoonlightShim(m_Shim);

        // Move the relay graph onto a dedicated per-session thread: video/audio
        // fragmentation + dc->send and the WS-fallback path all run off the main
        // thread (HTTP/control).
        spawnRelayThread(relay);

        // signaling->start() binds the WS listener on the relay thread that owns
        // the socket. Block only to retrieve the success/failure result.
        bool started = false;
        QMetaObject::invokeMethod(
            signaling, [&]() { started = signaling->start(); }, Qt::BlockingQueuedConnection);
        if (!started) {
            relay->deleteLater(); // also deletes signaling + shim (its children)
            m_Shim = nullptr;
            m_Respond(HttpResponse::error(500, "Failed to start signaling server"));
            emit sessionFailed("Signaling server failed to start");
            deleteLater();
            return;
        }

        m_Relay = relay;
        m_Signaling = signaling;
        emit relayCreated(relay);

        qDebug() << "[Session] Relay + Signaling created, starting LiStartConnection...";
        MoonlightShim* shim = m_Shim;
        QMetaObject::invokeMethod(
            shim, [shim, params]() { shim->startConnection(params); }, Qt::QueuedConnection);
    }
}

void StreamSession::onShimConnectionStarted()
{
    qDebug() << "[Session] LiStartConnection succeeded — sending response to browser";

    // Read the negotiated video format set by drSetup during LiStartConnection.
    // This is the codec Sunshine actually selected, NOT the user preference.
    // (VIDEO_FORMAT_* macros come from moonlight-common-c's Limelight.h)
    m_NegotiatedVideoFormat = m_Shim ? m_Shim->negotiatedVideoFormat() : 0;
    if (m_NegotiatedVideoFormat == 0) {
        // Fallback: if drSetup hasn't fired yet (shouldn't happen), use config
        m_NegotiatedVideoFormat = (m_Config.codec == VideoCodec::AV1)    ? VIDEO_FORMAT_AV1_MAIN8
                                  : (m_Config.codec == VideoCodec::HEVC) ? VIDEO_FORMAT_H265
                                                                         : VIDEO_FORMAT_H264;
    }

    // Log the negotiated codec for debugging
    // Use the codec MASKs (not single base bits): the 4:4:4 RExt/High profiles
    // have distinct bits and would otherwise be misdetected as H.264.
    const char* codecName = "h264";
    if (m_NegotiatedVideoFormat & VIDEO_FORMAT_MASK_H265) {
        codecName = "hevc";
    } else if (m_NegotiatedVideoFormat & VIDEO_FORMAT_MASK_AV1) {
        codecName = "av1";
    }
    qInfo() << "[Session] Negotiated video codec:" << codecName
            << "(format=0x" + QString::number(m_NegotiatedVideoFormat, 16) + ")";

    QJsonObject result;
    result["status"] = "streaming";
    // sessionUrl intentionally excluded — it contains the internal IP of Sunshine
    // (e.g. rtspenc://192.168.x.x:port) which must not be exposed to the browser.
    result["transport"] = m_Transport;
    result["transport_mode"] = m_TransportMode;
    // Fallback chain + this attempt's index: the frontend relaunches with the
    // next index when this transport fails to establish a connection.
    result["transport_index"] = m_TransportIndex;
    QJsonArray chainArr;
    for (const QString& m : m_TransportChain)
        chainArr.append(m);
    result["transport_chain"] = chainArr;

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

    // Report the NEGOTIATED video codec (from Sunshine), not the user preference.
    // This ensures the frontend decodes with the correct codec type even when
    // the auto-negotiation falls back from HEVC/AV1 to H.264.
    result["videoCodec"] = QString::fromLatin1(codecName);

    // Report whether YUV 4:4:4 chroma was actually negotiated (vs the default
    // 4:2:0), so the frontend can surface it in the stats overlay.
    result["yuv444"] = (m_NegotiatedVideoFormat & VIDEO_FORMAT_MASK_YUV444) != 0;

    // Audio time-stretch (WSOLA) — file-only setting (settings.json), default
    // false. Read fresh per session so a file edit applies on the next launch.
    {
        AppSettings settings;
        result["audio_time_stretch"] = settings.audioTimeStretch();
    }

    // If the codec was overridden (e.g. HEVC → H.264 for MediaTrack),
    // report the original selection so the frontend can log or adapt.
    if (m_CodecOverridden) {
        result["codecOverridden"] = true;
        switch (m_OriginalCodec) {
        case VideoCodec::HEVC: result["originalCodec"] = QStringLiteral("hevc"); break;
        case VideoCodec::AV1: result["originalCodec"] = QStringLiteral("av1"); break;
        default: result["originalCodec"] = QStringLiteral("auto"); break;
        }
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
