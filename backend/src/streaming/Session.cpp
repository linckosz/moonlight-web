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
#include "HostAudioSink.h"
#include "DataChannelRelay.h"
#include "MediaTrackRelay.h"
#include "SignalingServer.h"
#include "StreamRelay.h"
#include "MoonlightShim.h"
#include "NativeMediaEngine.h"
#include "../backend/NvHTTP.h"
#include "../backend/NvComputer.h"
#include "../backend/IdentityManager.h"
#include "../backend/streambackend/MediaDescriptor.h"
#include "../server/AppSettings.h"
#include "../LatencyFlag.h"

extern "C" {
#include "Limelight.h"
}

#include <QCoreApplication>
#include <QJsonObject>
#include <QJsonArray>
#include <QDebug>
#include <QUrl>
#include <QThread>
#include <QTimer>
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

    // The pair state belongs to the host itself, which is the right question for
    // a GameStream host and for Wolf — both pair the machine. A backend that
    // provisions seats does not: its machine authenticates its control API with
    // a key and is never a GameStream client of anything, while each seat is
    // paired separately, under its own certificate. Asking the machine here
    // would refuse every MultiSeat stream before the backend is even consulted.
    const bool hostIsThePairingSubject = !m_Backend || !m_Backend->capabilities().provisioning;
    if (hostIsThePairingSubject && m_Host->pairState != NvComputer::PS_PAIRED) {
        m_Respond(HttpResponse::error(403, "Host is not paired"));
        emit sessionFailed("Host is not paired");
        deleteLater();
        return;
    }

    // Generate per-session encryption keys
    m_Config.generateKeys();

    if (!m_Backend) {
        // A programming error, not a user-facing condition: every construction
        // site sets a backend before start().
        m_Respond(HttpResponse::error(500, "No stream backend for this session"));
        emit sessionFailed("No stream backend");
        deleteLater();
        return;
    }

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
    if (m_PreferResume || s_ActiveUniqueIds.contains(effectiveUniqueId())) {
        qInfo() << "[Session] Resuming (preferResume=" << m_PreferResume << ") on" << m_Host->name
                << m_Host->activeAddress.address();
        doResumeApp();
    } else {
        doLaunchApp();
    }
}

QString StreamSession::effectiveUniqueId() const
{
    return m_ClientUniqueId.isEmpty() ? IdentityManager::get()->getUniqueId() : m_ClientUniqueId;
}

LaunchRequest StreamSession::buildLaunchRequest() const
{
    LaunchRequest req;
    req.appId = m_AppId;
    req.width = m_StreamWidth;
    req.height = m_StreamHeight;
    req.fps = m_StreamFps;
    req.bitrateKbps = m_StreamBitrateKbps;
    req.hdrEnabled = m_Config.hdrEnabled;
    req.muteHostAudio = m_Config.muteHostAudio;
    req.rideOutLoss = m_RideOutLoss;
    req.rikey = m_Config.rikey;
    req.rikeyid = m_Config.rikeyid;
    // Empty means "the provider's default identity"; effectiveUniqueId() would
    // resolve it here and rob a multi-seat backend of the chance to substitute
    // the seat's own.
    req.clientUniqueId = m_ClientUniqueId;
    return req;
}

int StreamSession::launchTimeoutMs()
{
    if (!m_LaunchBudget.isValid()) m_LaunchBudget.start();

    // The cancel-and-retry in onLaunchResult() fires only for a timed-out
    // /launch that has not already retried and has not fallen back to /resume.
    // While it is still possible, keep its share of the budget back; once it is
    // gone this attempt may spend everything that is left.
    const bool retryAvailable = m_LaunchAttempted && !m_ResumeAttempted && !m_LaunchTimeoutRetried;
    const qint64 left =
        kLaunchBudgetMs - m_LaunchBudget.elapsed() - (retryAvailable ? kLaunchRetryReserveMs : 0);

    return static_cast<int>(
        qBound<qint64>(qint64{kLaunchMinTimeoutMs}, left, qint64{NvHTTP::LAUNCH_TIMEOUT_MS}));
}

void StreamSession::doResumeApp()
{
    qInfo() << "[Session] Resuming session on" << m_Host->name << "appid=" << m_AppId;
    m_ResumeAttempted = true;

    LaunchRequest req = buildLaunchRequest();
    req.timeoutMs = launchTimeoutMs();

    QPointer<StreamSession> self(this);
    m_Backend->resume(m_Host->uuid, req,
                      [self](bool ok, const BackendError& err, const MediaDescriptor& media) {
                          // The backend answers through a plain std::function, which
                          // — unlike a Qt connection — does not detach when this
                          // object dies mid-request.
                          if (!self) return;
                          self->onLaunchResult(ok, err, media);
                      });
}

void StreamSession::doLaunchApp()
{
    qDebug() << "[Session] Launching app" << m_AppId << "on" << m_Host->name;
    qDebug() << "[Session]   address:" << m_Host->activeAddress.address()
             << "port:" << m_Host->activeHttpsPort;
    qDebug() << "[Session]   stream:" << m_StreamWidth << "x" << m_StreamHeight << "@"
             << m_StreamFps << "fps, bitrate:" << m_StreamBitrateKbps << "kbps,"
             << "hdr:" << m_Config.hdrEnabled;

    m_LaunchAttempted = true;

    LaunchRequest req = buildLaunchRequest();
    req.timeoutMs = launchTimeoutMs();

    QPointer<StreamSession> self(this);
    m_Backend->launch(m_Host->uuid, req,
                      [self](bool ok, const BackendError& err, const MediaDescriptor& media) {
                          if (!self) return;
                          self->onLaunchResult(ok, err, media);
                      });
}

void StreamSession::quit()
{
    qInfo() << "[Session::quit] ENTER, m_Engine=" << m_Engine << "m_Relay=" << m_Relay
            << "m_MediaTrackRelay=" << m_MediaTrackRelay << "m_Signaling=" << m_Signaling
            << "m_StreamRelay=" << m_StreamRelay
            << "m_Connected=" << (m_Engine ? m_Engine->isConnected() : false)
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
    if (m_Engine) {
        qInfo() << "[Session::quit] Calling m_Engine->stopConnection() ...";
        m_Engine->stopConnection();
        qInfo() << "[Session::quit] m_Engine->stopConnection() returned";
    } else {
        qInfo() << "[Session::quit] No m_Engine to stop";
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
    m_Engine = nullptr;    // child of the relay — deleted with it

    qInfo() << "[Session::quit] EXIT";
}

void StreamSession::onLaunchResult(bool ok, const BackendError& err, const MediaDescriptor& media)
{
    // Drop the launch/resume TLS socket now: leaving it pooled ~120s would hold
    // Sunshine's single-threaded HTTPS server and block new iOS/Qt connections
    // for the whole stream. Polling is suspended during a stream, so this is safe.
    m_Http->dropPooledConnections();

    if (!ok) {
        qWarning() << "[Session] Launch failed:" << err.message << "kind=" << int(err.kind)
                   << "status=" << err.httpStatus;

        // A /launch that TIMES OUT (the host never answers) almost always means
        // it still holds a half-torn-down session from a just-cancelled stream:
        // the app is "running", so /launch blocks instead of erroring. The
        // rejection self-heal below cannot help — a timeout yields no answer to
        // read. Cancel our own session once, scoped to our uniqueid so it never
        // touches another client's, then relaunch. Bounded to a single retry.
        //
        // Only while the budget still affords it: a retry that runs past the
        // browser's own /start deadline is worse than no retry, because the
        // browser relaunches on top of it and both workers then wait on the
        // same unresponsive host.
        const bool budgetLeft = m_LaunchBudget.isValid() &&
                                (kLaunchBudgetMs - m_LaunchBudget.elapsed()) >= kLaunchMinTimeoutMs;
        if (err.kind == BackendError::Timeout && m_LaunchAttempted && !m_ResumeAttempted &&
            !m_LaunchTimeoutRetried && budgetLeft) {
            m_LaunchTimeoutRetried = true;
            qWarning() << "[Session] Launch timed out — cancelling stale session and retrying "
                          "/launch once ("
                       << (kLaunchBudgetMs - m_LaunchBudget.elapsed()) << "ms of budget left)";
            QPointer<StreamSession> self(this);
            m_Backend->quit(m_Host->uuid, effectiveUniqueId(), [self](bool, const BackendError&) {
                if (self) self->doLaunchApp();
            });
            return;
        }

        // The registry hint can be stale (orphaned session after a backend
        // restart, or a cleared entry whose host session is gone). Self-heal by
        // trying the other verb once. Both are keyed by our uniqueid, so a
        // /resume can only reconnect to OUR session, never another client's.
        //
        // Protocol also covers a 2xx that carried no session URL. That is
        // vanishingly rare, and one extra attempt of the other verb is harmless
        // self-healing rather than a wrong answer.
        if (err.kind == BackendError::Protocol) {
            if (!m_ResumeAttempted) {
                qWarning() << "[Session] Launch rejected (" << err.message
                           << ") — falling back to /resume for our uniqueid";
                doResumeApp();
                return;
            }
            if (!m_LaunchAttempted) {
                qWarning() << "[Session] Resume rejected (" << err.message
                           << ") — falling back to /launch";
                s_ActiveUniqueIds.remove(effectiveUniqueId()); // stale hint
                doLaunchApp();
                return;
            }
        }

        // Sunshine 503 = the host could not initialize video capture/encoding.
        // Almost always a display problem on the HOST: no display connected, or
        // the session is locked with the screen asleep — a blanked output stops
        // producing frames, so KMS/DXGI/CoreGraphics capture has nothing to grab
        // and Sunshine's encoder probe fails. On macOS it can also be a missing
        // Screen Recording permission. Surface a clear, actionable message
        // instead of the opaque "502 Launch error: HTTP 503".
        if (err.httpStatus == 503 || err.message.contains(QStringLiteral("503"))) {
            // Machine-readable `code` lets the frontend show an explicit dialog
            // instead of a plain toast. `local_host` + `local_os` tell it whose
            // machine failed: the repair buttons it offers (open the macOS
            // privacy pane, restart Sunshine) act on the machine running
            // MoonlightWeb, so they are only ever right when the failing host
            // IS that machine — on a remote host they would poke the wrong PC.
            QJsonObject errObj;
            errObj["error"] =
                QString("Host \"%1\" could not start video capture. On that machine, make "
                        "sure a display is connected and awake (a locked session with the "
                        "screen asleep has nothing to capture) or use an HDMI dummy plug. "
                        "On macOS, also grant Sunshine the \"Screen Recording\" permission "
                        "in System Settings > Privacy & Security, then restart Sunshine.")
                    .arg(m_Host->name);
            errObj["code"] = QStringLiteral("video_capture_failed");
            errObj["status"] = 503;
            errObj["local_host"] = m_Host->isLocalMachine();
#if defined(Q_OS_WIN)
            errObj["local_os"] = QStringLiteral("Windows");
#elif defined(Q_OS_MACOS)
            errObj["local_os"] = QStringLiteral("macOS");
#else
            errObj["local_os"] = QStringLiteral("Linux");
#endif
            m_Respond(HttpResponse::json(errObj, 503));
        } else if (err.httpStatus == 403 ||
                   err.message.contains(QStringLiteral("Permission denied"))) {
            // Apollo — and so every MultiSeat seat — grants full rights only to
            // the first device that pairs with it; everyone after gets "view the
            // list" and nothing else. The refusal is a 403 that says only
            // "Permission denied", which reads like a MoonlightWeb bug unless we
            // say whose decision it was and where to change it.
            m_Respond(HttpResponse::error(
                403, QString("\"%1\" has not given this device permission to launch apps. Open "
                             "that host's own web interface and grant this device the rights it "
                             "needs — a host that hands out limited permissions gives the first "
                             "device that pairs everything, and later ones almost nothing.")
                         .arg(m_Host->name)));
        } else if (err.kind == BackendError::Protocol) {
            m_Respond(HttpResponse::error(502, QString("Launch error: %1").arg(err.message)));
        } else {
            m_Respond(HttpResponse::error(502, QString("Launch failed: %1 (target: %2:%3)")
                                                   .arg(err.message)
                                                   .arg(m_Host->activeAddress.address())
                                                   .arg(m_Host->activeHttpsPort)));
        }

        emit sessionFailed(err.message);
        deleteLater();
        return;
    }

    // Only GameStream has an RTSP session to validate. A native descriptor
    // names a display and carries no URL at all, so validating one would
    // reject every native launch — which is exactly what it did until this
    // check was scoped to the media type it belongs to.
    if (media.type == MediaType::GameStreamRtsp) {
        const QString sessionUrl = media.gameStream.rtspSessionUrl;
        if (!QUrl(sessionUrl).isValid()) {
            m_Respond(HttpResponse::error(502, "Invalid RTSP URL: " + sessionUrl));
            emit sessionFailed("Invalid RTSP URL");
            deleteLater();
            return;
        }
        m_SessionUrl = sessionUrl;
    }

    // Session is now live on the host for this uniqueid: remember it so a later
    // reconnect (reload) resumes instead of relaunching. Cleared in quit().
    s_ActiveUniqueIds.insert(effectiveUniqueId());

    // ── Media engine selection ───────────────────────────────────────────────
    // The single place a media engine is chosen. Adding one means a case here
    // plus an IMediaEngine implementation — the relays below speak to the
    // interface's signals, never to GameStream, so nothing downstream changes.
    MoonlightShim::InitParams params;
    switch (media.type) {
    case MediaType::GameStreamRtsp:
        params.hostAddress = media.gameStream.hostAddress;
        params.appVersion = media.gameStream.appVersion;
        params.gfeVersion = media.gameStream.gfeVersion;
        params.rtspSessionUrl = media.gameStream.rtspSessionUrl;
        params.serverCodecModeSupport = media.gameStream.serverCodecModeSupport;
        params.aesKey = media.gameStream.aesKey;
        params.rikeyid = media.gameStream.rikeyid;
        break;

    case MediaType::NativeHost:
        // Nothing to fill in: a native descriptor carries no address, no
        // session URL and no key, because there is nothing to dial. What it
        // does carry — the display, the codec mask, the HDR request — is read
        // where the engine is built, a few lines below.
        break;
    }

    // Session-level settings below are transport-agnostic: they describe what
    // the browser asked for, not how we reach the host.

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

    // Create the engine BEFORE starting it. The relays must be connected to all
    // of its signals before frames start arriving.
    //
    // `startEngine` is how the transport branches below start it without
    // knowing which one they got: starting takes engine-specific parameters
    // (an RTSP URL and an AES key for GameStream, a display for native), and
    // this is the one place that knows them.
    std::function<void()> startEngine;

    if (media.type == MediaType::NativeHost) {
        auto* native = new NativeMediaEngine(this);
        m_Engine = native;

        NativeMediaEngine::StartParams nativeParams;
        nativeParams.displayId = media.nativeHost.displayId;
        nativeParams.width = m_StreamWidth;
        nativeParams.height = m_StreamHeight;
        nativeParams.fps = m_StreamFps;
        nativeParams.bitrateKbps = m_StreamBitrateKbps;
        // The same codec bitmask the GameStream path negotiates with, so the
        // browser's decoder expectations are identical either way.
        nativeParams.clientVideoFormats = params.supportedVideoFormats;
        nativeParams.hdr = media.nativeHost.hdrRequested;
        nativeParams.yuv444 = m_Config.chroma == ChromaSampling::C444;
        // Intra-refresh is only worth its cost when the client will decode
        // through the damage, so the client's word is what turns it on.
        nativeParams.intraRefresh = media.nativeHost.rideOutLoss;

        startEngine = [native, nativeParams]() { native->startCapture(nativeParams); };
    } else {
        auto* shim = new MoonlightShim(this);
        m_Engine = shim;
        startEngine = [shim, params]() { shim->startConnection(params); };
    }

    // Clipboard sync is only possible when the streamed host is this machine
    // (standard installer deployment: backend runs next to Sunshine) — the
    // backend clipboard is then the host's. For remote Sunshine hosts the
    // relay stays clipboard-silent and the browser keeps today's behavior.
    //
    // A restricted session (an invited player) never gets it either, whatever
    // the address: the host's clipboard is an exfiltration channel, and it is
    // not a guest's to read.
    const bool hostIsSelf = ClipboardBridge::isSelfAddress(m_Host->activeAddress.address());
    const bool clipboardLocal = hostIsSelf && m_InputPolicy.unrestricted();
    qInfo() << "[Session] Clipboard sync" << (clipboardLocal ? "enabled" : "disabled")
            << "(host address:" << m_Host->activeAddress.address()
            << ", restricted:" << !m_InputPolicy.unrestricted() << ")";
    // Same gate for toggle-lock sync: when the streamed host is this machine,
    // snapshot its real NumLock/CapsLock/ScrollLock state so the browser's
    // 'locksync' aligns instead of assuming the host starts with locks off.
    m_Engine->captureHostLockState(hostIsSelf);

    // Same gate again: Sunshine loopback-captures its virtual sink POST-volume,
    // and that sink is the host's default output during the stream — volume
    // keys pressed on the host mid-stream silently attenuate every client and
    // the value persists. Normalize it back to 100% at each stream start.
    if (hostIsSelf) {
        HostAudioSink::ensureFullVolume();
    }

    // Scroll: hold sub-notch amounts back only for a host that would throw them
    // away. Linux does (inputtino floors REL_WHEEL to whole notches with no
    // accumulator); Windows and macOS keep them, and quantizing those turns a
    // trackpad or a high-res wheel into a ratchet. An OS we could not work out
    // is quantized too — see HostOsProbe.h on why that is the safe way to be
    // wrong. Not final: the media path can still identify the host mid-stream,
    // and setScrollQuantization takes that at any time.
    {
        const HostOsProbe::HostOs os = m_Host->hostOs();
        const bool quantize = !HostOsProbe::keepsSubNotchScroll(os);
        m_Engine->setScrollQuantization(quantize);
        qInfo() << "[Session] Host OS" << HostOsProbe::toString(os) << "- scroll"
                << (quantize ? "quantized to whole notches" : "sent at full resolution");
    }

    // Concurrent sessions each start their controller numbering at 0, which on
    // the host collapses every player's gamepad onto the same virtual pad.
    m_Engine->setControllerOffset(m_GamepadOffset);
    // A guest who was not given keyboard/mouse must not move the host pointer,
    // not even the 1px the encoder wake-up uses to unstick a still screen.
    m_Engine->setWakeNudgeAllowed(m_InputPolicy.keyboardMouse);

    // Branch: WSS (legacy StreamRelay) or WebRTC (DataChannelRelay + SignalingServer)
    if (m_Transport == "wss") {
        // ── Legacy WSS mode: uses plain WebSocket StreamRelay ──────────────
        qInfo() << "[Session] Transport=wss — using legacy StreamRelay";

        auto* streamRelay = new StreamRelay(m_Engine, m_StreamRelayPort, {}, nullptr);
        streamRelay->setServerHost(m_ServerHost);
        streamRelay->setHttpsPort(m_HttpsPort);
        streamRelay->setWsPath(m_WsPath);
        streamRelay->setClipboardEnabled(clipboardLocal);
        streamRelay->setInputPolicy(m_InputPolicy);

        connect(m_Engine, &IMediaEngine::connectionStarted, this,
                &StreamSession::onShimConnectionStarted);
        connect(m_Engine, &IMediaEngine::connectionFailed, this,
                &StreamSession::onShimConnectionFailed);

        // The shim migrates to the relay's dedicated thread with it (child).
        m_Engine->setParent(streamRelay);

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
            m_Engine = nullptr;
            m_Respond(HttpResponse::error(500, "Failed to start StreamRelay"));
            emit sessionFailed("StreamRelay failed to start");
            deleteLater();
            return;
        }

        m_StreamRelay = streamRelay;
        emit streamRelayCreated(streamRelay);

        qDebug() << "[Session] StreamRelay created, starting the media engine...";
        QMetaObject::invokeMethod(m_Engine, startEngine, Qt::QueuedConnection);
    } else if (m_Transport == "webrtc-media") {
        // ── WebRTC Media Track mode: MediaTrackRelay + SignalingServer ─────────
        qInfo() << "[Session] Transport=webrtc-media — using MediaTrackRelay";

        // MediaTrackRelay: owns the libdatachannel PeerConnection + video track + audio/input DCs.
        // No QObject parent: it is moved onto a dedicated thread below.
        auto* relay = new MediaTrackRelay(m_Engine, nullptr);
        relay->setClipboardEnabled(clipboardLocal);
        relay->setInputPolicy(m_InputPolicy);

        // SignalingServer: WebSocket for SDP/ICE exchange only.
        auto* signaling = new SignalingServer(relay, m_WsPort, m_ServerHost, nullptr);
        signaling->setHttpsPort(m_HttpsPort);
        signaling->setWsPath(m_WsPath);
        signaling->setUseUPnP(m_UpnpEnabled);
        signaling->setStunServer(m_StunServer);
        signaling->setEnableIceTcp(m_EnableIceTcp);
        signaling->setAllowWsFallback(!m_AutoMode);
        signaling->setClientKind(m_ClientKind);
        signaling->setPairingIdentity(m_MwBindHostId, m_MwBindHostKey, m_MwBindBrowserKey);
        signaling->setMediaPort(m_MediaPort);

        // If an explicit WS URL was set (e.g. public tunnel), apply it.
        if (!m_ExplicitWsUrl.isEmpty()) {
            signaling->setOverrideWsUrl(m_ExplicitWsUrl);
            qInfo() << "[Session] Using explicit signaling URL:" << m_ExplicitWsUrl;
        }

        // Connect session-level handlers
        connect(m_Engine, &IMediaEngine::connectionStarted, this,
                &StreamSession::onShimConnectionStarted);
        connect(m_Engine, &IMediaEngine::connectionFailed, this,
                &StreamSession::onShimConnectionFailed);

        // signaling + shim migrate to the relay's dedicated thread (children).
        signaling->setParent(relay);
        m_Engine->setParent(relay);

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
            m_Engine = nullptr;
            m_Respond(HttpResponse::error(500, "Failed to start signaling server"));
            emit sessionFailed("Signaling server failed to start");
            deleteLater();
            return;
        }

        m_MediaTrackRelay = relay;
        m_Signaling = signaling;
        emit mediaTrackRelayCreated(relay);

        qDebug() << "[Session] MediaTrackRelay + Signaling created, starting LiStartConnection...";
        QMetaObject::invokeMethod(m_Engine, startEngine, Qt::QueuedConnection);
    } else {
        // ── WebRTC mode: DataChannelRelay + SignalingServer (default) ─────
        qInfo() << "[Session] Transport=webrtc — using DataChannelRelay";

        // DataChannelRelay: owns the libdatachannel PeerConnection + DataChannels.
        // No QObject parent: it is moved onto a dedicated thread below.
        auto* relay = new DataChannelRelay(m_Engine, nullptr);
        relay->setClipboardEnabled(clipboardLocal);
        relay->setInputPolicy(m_InputPolicy);

        // The client's half of the bargain: it says it will decode through the
        // damage. The engine's half — whether the stream really refreshes — is
        // asked at the moment a gap happens, because the engine has not started
        // yet at this point and would answer no to anything asked now.
        relay->setRideOutLoss(media.type == MediaType::NativeHost && media.nativeHost.rideOutLoss);

        // SignalingServer: WebSocket for SDP/ICE exchange only.
        // NonSecure mode: external tunnel or Cloudflare provides TLS termination.
        auto* signaling = new SignalingServer(relay, m_WsPort, m_ServerHost, nullptr);
        signaling->setHttpsPort(m_HttpsPort);
        signaling->setWsPath(m_WsPath);
        signaling->setUseUPnP(m_UpnpEnabled);
        signaling->setStunServer(m_StunServer);
        signaling->setEnableIceTcp(m_EnableIceTcp);
        signaling->setAllowWsFallback(!m_AutoMode);
        signaling->setClientKind(m_ClientKind);
        signaling->setPairingIdentity(m_MwBindHostId, m_MwBindHostKey, m_MwBindBrowserKey);
        signaling->setMediaPort(m_MediaPort);

        // If an explicit WS URL was set (e.g. public tunnel), apply it.
        if (!m_ExplicitWsUrl.isEmpty()) {
            signaling->setOverrideWsUrl(m_ExplicitWsUrl);
            qInfo() << "[Session] Using explicit signaling URL:" << m_ExplicitWsUrl;
        }

        // Connect the session-level handlers
        connect(m_Engine, &IMediaEngine::connectionStarted, this,
                &StreamSession::onShimConnectionStarted);
        connect(m_Engine, &IMediaEngine::connectionFailed, this,
                &StreamSession::onShimConnectionFailed);

        // signaling + shim migrate to the relay's dedicated thread (children).
        signaling->setParent(relay); // Signaling is a child of relay
        m_Engine->setParent(relay);

        // Clean up when relay session ends (DataChannel disconnect, error, etc.)
        connect(relay, &DataChannelRelay::sessionEnded, this, [this]() {
            quit();
            deleteLater();
        });

        // Provide MoonlightShim reference for WS fallback (ICE timeout → WS data transport)
        signaling->setMediaEngine(m_Engine);

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
            m_Engine = nullptr;
            m_Respond(HttpResponse::error(500, "Failed to start signaling server"));
            emit sessionFailed("Signaling server failed to start");
            deleteLater();
            return;
        }

        m_Relay = relay;
        m_Signaling = signaling;
        emit relayCreated(relay);

        qDebug() << "[Session] Relay + Signaling created, starting LiStartConnection...";
        QMetaObject::invokeMethod(m_Engine, startEngine, Qt::QueuedConnection);
    }
}

void StreamSession::applyInputPolicy(const InputMsg::Policy& policy)
{
    m_InputPolicy = policy;

    // Every object below lives on the relay thread. The policy is read there on
    // each inbound message, so it has to be written there too — a plain setter
    // from this thread would be a data race on the hot path.
    // StreamRelay is not a RelayBase (the legacy WSS path predates it) but
    // carries the same setter, so the templated lambda covers all three.
    auto push = [policy](auto* relay) {
        if (!relay) return;
        QMetaObject::invokeMethod(
            relay, [relay, policy]() { relay->setInputPolicy(policy); }, Qt::QueuedConnection);
    };
    push(m_Relay);
    push(m_MediaTrackRelay);
    push(m_StreamRelay);

    if (IMediaEngine* shim = m_Engine) {
        QMetaObject::invokeMethod(
            shim,
            [shim, policy]() {
                shim->setWakeNudgeAllowed(policy.keyboardMouse);
                // Release everything, then let the client's next heartbeat put
                // back whatever it still holds *and* is still allowed to hold
                // (filterHeldState runs against the new policy). Anything else
                // leaves a key down on the host that nobody can lift: the guest
                // can no longer send the key-up, and the owner never pressed it.
                // Blunter than releasing only what was revoked, and right for
                // the same reason — a demotion is rare, a stuck key is not
                // recoverable.
                shim->releaseHeldInputs(true);
            },
            Qt::QueuedConnection);
    }

    qInfo() << "[Session] Input policy changed — gamepad:" << policy.gamepad
            << "keyboard/mouse:" << policy.keyboardMouse;
}

// Read the TTL of the host's own packets and, if it settles an OS we could not
// work out before the stream started, apply it. Runs every 250 ms for the first
// few seconds: the first video datagram usually lands within one tick, but a
// host that is slow to send anything must not cost us the answer for the whole
// session. Giving up is normal — a platform that cannot report a TTL leaves it
// at 0 forever, and the safe default already covers that.
void StreamSession::sampleHostIpTtl()
{
    if (!m_Engine) return;

    const int ttl = m_Engine->hostIpTtl();
    if (ttl == 0) {
        if (--m_TtlSamplesLeft > 0) QTimer::singleShot(250, this, &StreamSession::sampleHostIpTtl);
        return;
    }

    emit hostIpTtlObserved(ttl);

    // Never overturns a verdict that was reached from something stronger: the
    // parent's answer, when it had one, was built on facts (our own address
    // list, an API that identified itself) that a fingerprint cannot beat.
    if (m_Host->hostOs() != HostOsProbe::HostOs::Unknown) return;

    const HostOsProbe::HostOs os = HostOsProbe::fromInitialTtl(ttl);
    if (os == HostOsProbe::HostOs::Unknown) {
        // A TTL of 64 lands here: Linux and macOS are indistinguishable at that
        // point and want opposite treatment, so nothing changes.
        qInfo() << "[Session] Host IP TTL" << ttl << "- OS still undetermined, scroll stays"
                << "quantized";
        return;
    }

    const bool quantize = !HostOsProbe::keepsSubNotchScroll(os);
    m_Host->observedIpTtl = ttl;
    m_Engine->setScrollQuantization(quantize);
    qInfo() << "[Session] Host IP TTL" << ttl << "- host is" << HostOsProbe::toString(os)
            << "- scroll now" << (quantize ? "quantized to whole notches" : "at full resolution");
}

void StreamSession::onShimConnectionStarted()
{
    qDebug() << "[Session] LiStartConnection succeeded — sending response to browser";

    // The host's OS may still be unknown at this point — nothing on the control
    // plane names it. Now that its packets are arriving, the TTL they carry can
    // settle it. Start looking; sampleHostIpTtl() stops itself once it has an
    // answer or once it is clear none is coming.
    m_TtlSamplesLeft = 20;
    sampleHostIpTtl();

    // Read the negotiated video format set by drSetup during LiStartConnection.
    // This is the codec Sunshine actually selected, NOT the user preference.
    // (VIDEO_FORMAT_* macros come from moonlight-common-c's Limelight.h)
    m_NegotiatedVideoFormat = m_Engine ? m_Engine->negotiatedVideoFormat() : 0;
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
    // A transport the user forced by name that never made it into the chain.
    // Present only in that case — its absence means the request was honoured.
    if (!m_TransportDropped.isEmpty()) result["transport_dropped"] = m_TransportDropped;

    if (m_Transport == "wss") {
        // WSS mode: provide the StreamRelay WS URL directly
        result["wsUrl"] = m_StreamRelay ? m_StreamRelay->wsUrl() : QString();
        result["signalingUrl"] = QString();
    } else if (m_Signaling) {
        // WebRTC mode: provide the signaling WS URL for SDP/ICE exchange.
        //
        // A path when the browser came down the tunnel. It has no HTTP route
        // here, so an absolute URL could only name the rendezvous under this
        // machine's local port — a door that answers nowhere. The page resolves
        // the path against its own origin, and the tunnel carries the socket.
        result["signalingUrl"] = m_TunnelArrival ? m_Signaling->wsPath() : m_Signaling->wsUrl();
    } else {
        result["signalingUrl"] = QString();
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

    // Whether the stream really repairs itself by intra-refresh — asked for by
    // the browser, GRANTED by the encoder, and reported here so the browser can
    // stop demanding keyframes on a gap. Sent as what the stream does, never as
    // what was requested: a receiver that suppressed its keyframe requests
    // against a stream that has no refresh wave would sit on a corrupt picture
    // indefinitely.
    result["intra_refresh"] = m_Engine && m_Engine->intraRefreshActive();

    // Audio time-stretch (WSOLA) — file-only setting (settings.json), default
    // false. Read fresh per session so a file edit applies on the next launch.
    {
        AppSettings settings;
        result["audio_time_stretch"] = settings.audioTimeStretch();
        // Click-to-photon probe: tells the browser the host will raise its
        // flag on every click, so it installs its measuring side (and nothing
        // is installed otherwise). Read from the setting, not the live thread:
        // this runs in the worker process, the overlay lives in the server's.
        result["latency_flag"] = LatencyFlag::isSupported() && settings.latencyFlagEnabled();
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
