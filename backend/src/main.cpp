#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include <QTimer>
#include <QDateTime>
#include <QRandomGenerator>
#include <functional>
#include "server/AppSettings.h"
#include "server/HttpServer.h"
#include "server/RestRouter.h"
#include "common/Logger.h"
#include "backend/ComputerManager.h"
#include "backend/IdentityManager.h"
#include "streaming/Session.h"
#include "streaming/MoonlightShim.h"
#include "streaming/DataChannelRelay.h"
#include "streaming/MediaTrackRelay.h"
#include "streaming/StreamRelay.h"
#include "network/InternetAccessManager.h"
#include "TrayManager.h"

#include <openssl/ssl.h>
#include <openssl/err.h>

// Load KEY=VALUE pairs from a .env file into the process environment.
// Supports PEM blocks: if a value starts with "-----BEGIN", lines are
// accumulated until "-----END" is found (multi-line key support).
// Called early in main() so that env vars (e.g. MW_CERT_KEY) are available
// before any certificate loading happens.
static void loadEnvFile()
{
    QString path = QCoreApplication::applicationDirPath() + "/.env";
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return;

    int count = 0;
    while (!f.atEnd()) {
        QByteArray line = f.readLine().trimmed();
        if (line.isEmpty() || line.startsWith('#'))
            continue;

        int eq = line.indexOf('=');
        if (eq <= 0)
            continue;

        QByteArray key = line.left(eq).trimmed();
        QByteArray value = line.mid(eq + 1).trimmed();

        // Strip surrounding quotes
        if (value.size() >= 2 && value.startsWith('"') && value.endsWith('"'))
            value = value.mid(1, value.size() - 2);

        // PEM block: accumulate BEGIN..END lines
        if (value.startsWith("-----BEGIN")) {
            QByteArray pem = value + '\n';
            while (!f.atEnd()) {
                QByteArray next = f.readLine().trimmed();
                pem += next + '\n';
                if (next.startsWith("-----END"))
                    break;
            }
            qputenv(key, pem);
        } else {
            qputenv(key, value);
        }
        count++;
    }

    Logger::info(QString("[.env] Loaded %1 variables from %2").arg(count).arg(path));
}

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("Moonlight-Web");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Moonlight-Web");

    // Load .env file before anything reads environment variables
    loadEnvFile();

    // Parse command line
    QCommandLineParser parser;
    parser.setApplicationDescription("Moonlight-Web streaming server");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOption("port", "HTTP server port", "port", "80");
    parser.addOption(portOption);

    QCommandLineOption logOption("log", "Log file path", "path");
    parser.addOption(logOption);

    QCommandLineOption wsPortOption("ws-port", "WebRTC signaling WebSocket port", "port", "48001");
    parser.addOption(wsPortOption);

    parser.process(app);

    // Configure logging
    if (parser.isSet(logOption))
        Logger::instance()->setLogFile(parser.value(logOption));

    Logger::info("Moonlight-Web server starting...");
    Logger::info("Version: " + QCoreApplication::applicationVersion());

    // Read HTTP/HTTPS port preferences from persisted settings.
    // CLI --port overrides the persisted HTTP port when explicitly provided.
    AppSettings appSettings;
    quint16 httpPort = appSettings.httpPort(80);
    if (parser.isSet("port"))
        httpPort = parser.value("port").toUShort();

    HttpServer server(httpPort);

    // Pass domain and cert path to HttpServer so loadCert() can find
    // the correct certificate by CN matching (or load an explicit cert).
    server.setDomain(appSettings.domain());
    server.setCertPath(appSettings.certPath());

    // Initialize ComputerManager (Phase 2: host discovery)
    ComputerManager computerManager(&app);
    computerManager.init();

    // Phase 3: Initialize pairing identity (generates RSA keypair if needed)
    IdentityManager::get();
    Logger::info("Pairing identity initialized");

    // Force OpenSSL init before any PeerConnection is created.
    // libdatachannel inits OpenSSL lazily; if the first DTLS handshake
    // triggers a race on the static init mutex, SSL_ERROR_SYSCALL can occur.
    // Doing a synchronous init here avoids the race entirely.
    {
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS
                         | OPENSSL_INIT_LOAD_CRYPTO_STRINGS
                         | OPENSSL_INIT_NO_ATEXIT, nullptr);
        OPENSSL_init_crypto(OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
        Logger::info("OpenSSL initialized");
    }

    // Read remaining persistent settings
    quint16 httpsPort = appSettings.httpsPort(443);
    VideoCodec preferredCodec = appSettings.videoCodec();
    bool upnpEnabled = appSettings.upnpEnabled();
    QString stunServer = appSettings.stunServer();
    Logger::info("[main] Settings: http_port=" + QString::number(httpPort)
                 + ", https_port=" + QString::number(httpsPort)
                 + ", video_codec=" + AppSettings::videoCodecToString(preferredCodec)
                 + ", upnp_enabled=" + (upnpEnabled ? "true" : "false")
                 + ", stun_server=" + stunServer);

    // Phase 5b: WebRTC DataChannel relay + signaling tracking
    quint16 signalingPort = parser.value("ws-port").toUShort();
    QPointer<DataChannelRelay> g_ActiveRelay;
    QPointer<MediaTrackRelay> g_ActiveMediaTrackRelay;
    QPointer<StreamRelay> g_ActiveStreamRelay;
    InternetAccessManager internetAccess(&appSettings);

    // Hot-reload TLS when certificate is renewed (no server restart needed)
    QObject::connect(&internetAccess, &InternetAccessManager::certificateChanged,
        [&server](const QString& certPath) {
            qInfo() << "[main] Certificate renewed, reloading TLS:" << certPath;
            // Update the explicit cert path so reloadTls() loads the new file directly
            server.setCertPath(certPath);
            if (!server.reloadTls()) {
                qWarning() << "[main] TLS reload failed — restart may be required";
            }
        });

    // Register API routes
    server.router()->get("/api/health", [](const HttpRequest&) {
        QJsonObject obj;
        obj["status"] = "ok";
        obj["version"] = QCoreApplication::applicationVersion();
        return HttpResponse::json(obj);
    });

    server.router()->get("/api/server/status", [&server](const HttpRequest&) {
        QJsonObject obj;
        obj["status"] = "running";
        obj["version"] = QCoreApplication::applicationVersion();
        obj["http_port"] = static_cast<int>(server.httpPort());
        obj["https_port"] = static_cast<int>(server.activeHttpsPort());
        return HttpResponse::json(obj);
    });

    server.router()->get("/api/hosts", [&computerManager](const HttpRequest&) {
        QJsonObject obj;
        obj["hosts"] = computerManager.getHostsJson();
        return HttpResponse::json(obj);
    });

    server.router()->post("/api/hosts/scan", [&computerManager](const HttpRequest&) {
        computerManager.handleScanRequest();
        QJsonObject obj;
        obj["status"] = "scanning";
        return HttpResponse::json(obj);
    });

    server.router()->post("/api/hosts/manual", [&computerManager](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        QString address = body["address"].toString();
        if (address.isEmpty())
            return HttpResponse::error(400, "Missing 'address' field");

        auto [status, result] = computerManager.handleAddManualHost(address);
        return HttpResponse::json(result, status);
    });

    server.router()->del("/api/hosts/:id", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty())
            return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleDeleteHost(uuid);
        return HttpResponse::json(result, status);
    });

    // Phase 3: Pairing routes
    server.router()->get("/api/hosts/:id/pair", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty())
            return HttpResponse::error(400, "Missing host ID");
        auto [status, result] = computerManager.handleStartPairing(uuid);
        return HttpResponse::json(result, status);
    });

    server.router()->post("/api/hosts/:id/pair", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty())
            return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleSubmitPin(uuid);
        return HttpResponse::json(result, status);
    });

    // Phase 4: App list (async — fetches from Sunshine via HTTPS)
    server.router()->getAsync("/api/hosts/:id/apps",
        [&computerManager](const HttpRequest& req, ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) {
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }
        computerManager.handleGetAppList(uuid, std::move(respond));
    });

    // Phase 4: App asset proxy — PNG (async, fetches on demand if not cached)
    server.router()->getAsync("/api/hosts/:id/appasset",
        [&computerManager](const HttpRequest& req, ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) {
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }

        bool ok;
        int appId = req.queryParams.value("appid").toInt(&ok);
        if (!ok || appId <= 0) {
            respond(HttpResponse::error(400, "Missing or invalid appid parameter"));
            return;
        }

        computerManager.handleGetBoxArt(uuid, appId, std::move(respond));
    });

    // Phase 5: Start streaming — launch app + RTSP handshake
    auto effectiveUpnpEnabled = upnpEnabled;  // Capture by value for the lambda

    server.router()->postAsync("/api/hosts/:id/start",
        [&computerManager, signalingPort, &g_ActiveRelay, &g_ActiveStreamRelay, &g_ActiveMediaTrackRelay, &server, &appSettings, effectiveUpnpEnabled, stunServer](const HttpRequest& req, ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) {
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }

        NvComputer* host = computerManager.getHost(uuid);
        if (!host) {
            respond(HttpResponse::error(404, "Host not found"));
            return;
        }

        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        int appId = body["appId"].toInt(0);
        if (appId <= 0) {
            respond(HttpResponse::error(400, "Missing or invalid appId"));
            return;
        }

        // Determine signaling host from the browser's Host header.
        // Works for both LAN (localhost:443) and remote access via moonlightweb.top.
        QString serverHost = req.headers.value("host");
        int colon = serverHost.indexOf(':');
        if (colon >= 0)
            serverHost = serverHost.left(colon);

        // ================================================================
        // Resolve transport mode (from AppSettings).
        //
        // transport_mode values:
        //   "auto"                 → automatic fallback chain (see below)
        //   "webrtc-media-udp"     → MediaTrack + UDP only
        //   "webrtc-dc-udp"       → DataChannel + UDP only
        //   "webrtc-media-tcp"    → MediaTrack + UDP+TCP
        //   "webrtc-dc-tcp"       → DataChannel + UDP+TCP
        //   "wss"                 → StreamRelay (WebSocket, always works)
        // ================================================================
        QString transportMode = appSettings.transportMode();
        if (transportMode.isEmpty())
            transportMode = "auto";

        bool isAutoMode = (transportMode == "auto");

        // ── Auto-mode helpers: priority-ordered transport list ─────────
        static const QStringList kAutoTransportOrder = {
            QStringLiteral("webrtc-media-udp"),
            QStringLiteral("webrtc-dc-udp"),
            QStringLiteral("webrtc-media-tcp"),
            QStringLiteral("webrtc-dc-tcp"),
            QStringLiteral("wss")
        };

        // Helper: does the host support a given codec?
        auto hostSupportsCodec = [](NvComputer* h, VideoCodec c) -> bool {
            int support = h->serverCodecModeSupport;
            switch (c) {
            case VideoCodec::H264: return (support & 0x01) != 0; // SERVER_CODEC_MODE_H264
            case VideoCodec::HEVC: return (support & 0x02) != 0; // SERVER_CODEC_MODE_H265
            case VideoCodec::AV1:  return (support & 0x20) != 0; // SERVER_CODEC_MODE_AV1
            default:               return true;
            }
        };

        // Helper: filter transport list by codec compatibility.
        //
        // Rules:
        //   - H.264 works on all transports.
        //   - HEVC works on all transports (depends on host/browser support).
        //   - AV1 is skipped for MediaTrack modes (browser <video> AV1
        //     support varies across platforms; DataChannel WebCodecs
        //     is more reliable).
        //   - If the host doesn't support the selected codec, try H.264
        //     instead (if host supports it).
        auto filterTransportsByCodec = [&](const QStringList& transports,
                                            VideoCodec codec,
                                            NvComputer* h) -> QStringList {
            // Determine effective codec (resolve Auto → HEVC if host supports)
            VideoCodec effective = codec;
            if (effective == VideoCodec::Auto) {
                effective = hostSupportsCodec(h, VideoCodec::HEVC)
                    ? VideoCodec::HEVC : VideoCodec::H264;
            }

            // If host doesn't support the effective codec, fall back to H.264
            if (!hostSupportsCodec(h, effective)) {
                if (hostSupportsCodec(h, VideoCodec::H264)) {
                    qInfo() << "[Auto] Codec" << static_cast<int>(effective)
                            << "not supported by host, falling back to H.264";
                    effective = VideoCodec::H264;
                } else {
                    qWarning() << "[Auto] Host supports NO video codec!";
                    return transports; // Return all, let it fail naturally
                }
            }

            qInfo() << "[Auto] Effective codec:" << static_cast<int>(effective);

            QStringList result;
            for (const auto& t : transports) {
                if (effective == VideoCodec::AV1 && t.startsWith("webrtc-media")) {
                    qInfo() << "[Auto] Skipping" << t << "(AV1 + MediaTrack not reliable)";
                    continue; // Skip AV1 on MediaTrack
                }
                result.append(t);
            }
            return result;
        };

        // ── Non-auto mode: resolve to internal transport + ICE config ─
        QString internalTransport;
        bool enableIceTcp = false;

        auto resolveExplicitTransport = [&](const QString& mode) {
            if (mode == "webrtc-media-udp") {
                internalTransport = "webrtc-media";
                enableIceTcp = false;
            } else if (mode == "webrtc-dc-udp") {
                internalTransport = "webrtc";
                enableIceTcp = false;
            } else if (mode == "webrtc-media-tcp") {
                internalTransport = "webrtc-media";
                enableIceTcp = true;
            } else if (mode == "webrtc-dc-tcp") {
                internalTransport = "webrtc";
                enableIceTcp = true;
            } else if (mode == "wss") {
                internalTransport = "wss";
                enableIceTcp = false;
            } else {
                // Unknown mode → fall back to legacy transport setting
                QString legacy = appSettings.transport();
                internalTransport = (legacy == "wss") ? "wss" : "webrtc";
                enableIceTcp = false;
            }
        };

        // Build ordered attempt list for auto mode (declared here so it's
        // visible to both the log block below and the fallback chain block).
        QStringList orderedTransports;
        if (!isAutoMode) {
            resolveExplicitTransport(transportMode);
            qInfo() << "[Session] Transport: explicit mode=" << transportMode
                    << "internal=" << internalTransport
                    << "iceTcp=" << enableIceTcp;
        } else {
            qInfo() << "[Session] Transport: auto mode";
            orderedTransports = filterTransportsByCodec(
                kAutoTransportOrder, appSettings.videoCodec(), host);
            qInfo() << "[Auto] Ordered transports after codec filter:" << orderedTransports;
        }

        // ── Helper: attach lifecycle relay tracking for a new session ───────────
        // Adds the standard relay-created and session-ended connections that
        // maintain the global relay pointers (g_ActiveRelay, etc.) and send a
        // best-effort HTTPS quit to Sunshine when a session ends unexpectedly.
        auto attachRelayTracking = [&](StreamSession* s) {
            // WSS mode: StreamRelay tracking
            QObject::connect(s, &StreamSession::streamRelayCreated,
                [&g_ActiveStreamRelay, &computerManager, host](StreamRelay* r) {
                    qInfo() << "[main] streamRelayCreated, relay=" << r;
                    g_ActiveStreamRelay = r;

                    QObject::connect(r, &StreamRelay::sessionEnded,
                        [r, &g_ActiveStreamRelay, &computerManager, host]() {
                            qInfo() << "[main] StreamRelay sessionEnded";
                            auto* identity = IdentityManager::get();
                            auto* quitReply = computerManager.http()->quitAppAsync(
                                host->activeAddress, host->activeHttpsPort,
                                identity->getCertificate(), identity->getPrivateKey());
                            QObject::connect(quitReply, &QNetworkReply::finished, quitReply, &QNetworkReply::deleteLater);
                            r->stop();
                            r->deleteLater();
                            if (g_ActiveStreamRelay == r) g_ActiveStreamRelay = nullptr;
                        });
                });

            // WebRTC DataChannel mode: DataChannelRelay tracking
            QObject::connect(s, &StreamSession::relayCreated,
                [&g_ActiveRelay, &computerManager, host](DataChannelRelay* r) {
                    qInfo() << "[main] relayCreated, relay=" << r;
                    g_ActiveRelay = r;

                    QObject::connect(r, &DataChannelRelay::sessionEnded,
                        [r, &g_ActiveRelay, &computerManager, host]() {
                            qInfo() << "[main] sessionEnded fired, relay=" << r;
                            auto* identity = IdentityManager::get();
                            auto* quitReply = computerManager.http()->quitAppAsync(
                                host->activeAddress, host->activeHttpsPort,
                                identity->getCertificate(), identity->getPrivateKey());
                            QObject::connect(quitReply, &QNetworkReply::finished, quitReply, &QNetworkReply::deleteLater);
                            r->stop();
                            r->deleteLater();
                            if (g_ActiveRelay == r) {
                                g_ActiveRelay = nullptr;
                            }
                        });
                });

            // WebRTC Media Track mode: MediaTrackRelay tracking
            QObject::connect(s, &StreamSession::mediaTrackRelayCreated,
                [&g_ActiveMediaTrackRelay, &computerManager, host](MediaTrackRelay* r) {
                    qInfo() << "[main] mediaTrackRelayCreated, relay=" << r;
                    g_ActiveMediaTrackRelay = r;

                    QObject::connect(r, &MediaTrackRelay::sessionEnded,
                        [r, &g_ActiveMediaTrackRelay, &computerManager, host]() {
                            qInfo() << "[main] MediaTrackRelay sessionEnded, relay=" << r;
                            auto* identity = IdentityManager::get();
                            auto* quitReply = computerManager.http()->quitAppAsync(
                                host->activeAddress, host->activeHttpsPort,
                                identity->getCertificate(), identity->getPrivateKey());
                            QObject::connect(quitReply, &QNetworkReply::finished, quitReply, &QNetworkReply::deleteLater);
                            r->stop();
                            r->deleteLater();
                            if (g_ActiveMediaTrackRelay == r) g_ActiveMediaTrackRelay = nullptr;
                        });
                });
        };

        // ── Helper: create a session with the given transport mode and attach tracking ─
        // transportMode: full mode string ("webrtc-media-udp", "wss", etc.)
        // iceTcp: whether to enable ICE-TCP candidates
        auto createSession = [&](const QString& transportMode, bool iceTcp,
                                  ResponseCallback rsp) -> StreamSession* {
            // Map transport mode to internal transport string
            QString internal;
            if (transportMode == "webrtc-media-udp" || transportMode == "webrtc-media-tcp")
                internal = "webrtc-media";
            else if (transportMode == "webrtc-dc-udp" || transportMode == "webrtc-dc-tcp")
                internal = "webrtc";
            else
                internal = transportMode; // "wss"

            auto* s = new StreamSession(
                host, appId,
                computerManager.http(),
                std::move(rsp),
                signalingPort,
                serverHost,
                appSettings.videoCodec(),
                appSettings.gamingMode(),
                effectiveUpnpEnabled,
                internal,
                stunServer,
                appSettings.streamHeight(),
                appSettings.streamFps(),
                appSettings.streamBitrate()
            );
            s->setHttpsPort(server.activeHttpsPort());
            s->setStreamRelayPort(signalingPort + 1);
            s->setTransportMode(transportMode); // Full mode for response
            s->setEnableIceTcp(iceTcp);
            attachRelayTracking(s);
            return s;
        };

        // ═════════════════════════════════════════════════════════════════════
        // Fallback chain: auto mode tries transports in priority order,
        // failing forward to the next on sessionFailed.
        //
        // Priority order: 1. MediaTrack UDP  2. DataChannel UDP
        //                 3. MediaTrack TCP  4. DataChannel TCP  5. WSS
        //
        // Codec compatibility filters remove unsupported combos above.
        // ═════════════════════════════════════════════════════════════════════
        if (!isAutoMode) {
            // ── Explicit transport: single attempt ──────────────────────────
            auto* session = createSession(transportMode, enableIceTcp, std::move(respond));
            session->start();
        } else {
            // ── Auto mode: try each transport in priority order ─────────────
            struct AutoFallbackState {
                ResponseCallback respond;
                bool responded = false;
                int currentAttempt = 0;
                QStringList attempts;
                NvComputer* host = nullptr;
                int appId = 0;
                HttpResponse deferredResponse;  // Kept for compilation, no longer populated
                QString currentMode;
            };

            auto fbState = std::make_shared<AutoFallbackState>();
            fbState->respond = std::move(respond);
            fbState->attempts = orderedTransports;
            fbState->host = host;
            fbState->appId = appId;

            // Use shared_ptr for tryNext so that relay sessionEnded handlers
            // from orphan relays (parented to QApp, surviving the session) don't
            // capture a dangling reference. The shared_ptr outlives the local scope.
            auto tryNextFn = std::make_shared<std::function<void()>>();
            std::function<void()> tryNext;
            tryNext = [fbState, &computerManager, signalingPort, serverHost,
                       &appSettings, effectiveUpnpEnabled, stunServer,
                       &g_ActiveRelay, &g_ActiveStreamRelay, &g_ActiveMediaTrackRelay,
                       &server, tryNextFn]() {
                if (fbState->responded) return;

                if (fbState->currentAttempt >= fbState->attempts.size()) {
                    qWarning() << "[Auto] All transports exhausted";
                    if (!fbState->responded) {
                        fbState->responded = true;
                        fbState->respond(HttpResponse::error(502,
                            "All transport modes failed"));
                    }
                    return;
                }

                int idx = fbState->currentAttempt++;
                QString mode = fbState->attempts[idx];
                bool iceTcp = mode.endsWith(QStringLiteral("-tcp"));
                qInfo() << "[Auto] Attempt" << idx + 1 << "/"
                        << fbState->attempts.size() << ":" << mode
                        << "iceTcp=" << iceTcp;

                fbState->currentMode = mode;

                // ── respond callback ─────────────────────────────────────
                // Forward immediately for ALL transports.
                // The frontend needs the signaling info (WebSocket URL) from this response
                // to connect and complete the ICE handshake. Deferring would deadlock.
                auto attemptRespond = [fbState, mode](HttpResponse resp) {
                    if (fbState->responded) return;
                    if (resp.statusCode >= 400) {
                        qInfo() << "[Auto]" << mode
                                << "failed (HTTP" << resp.statusCode << ") — retrying";
                        return; // sessionFailed will trigger tryNext
                    }
                    qInfo() << "[Auto]" << mode << "succeeded — forwarding response";
                    fbState->responded = true;
                    fbState->respond(std::move(resp));
                };

                // Resolve internal transport string for StreamSession constructor
                QString internalTransport;
                if (mode.startsWith(QStringLiteral("webrtc-media")))
                    internalTransport = QStringLiteral("webrtc-media");
                else if (mode.startsWith(QStringLiteral("webrtc-dc")))
                    internalTransport = QStringLiteral("webrtc");
                else
                    internalTransport = mode; // "wss"

                auto* session = new StreamSession(
                    fbState->host, fbState->appId,
                    computerManager.http(),
                    std::move(attemptRespond),
                    signalingPort, serverHost,
                    appSettings.videoCodec(),
                    appSettings.gamingMode(),
                    effectiveUpnpEnabled,
                    internalTransport, stunServer,
                    appSettings.streamHeight(),
                    appSettings.streamFps(),
                    appSettings.streamBitrate()
                );
                session->setHttpsPort(server.activeHttpsPort());
                session->setStreamRelayPort(signalingPort + 1);
                session->setTransportMode(mode);
                session->setEnableIceTcp(iceTcp);
                session->setAutoMode(true);  // Disable internal WS fallback → use auto chain

                // ── Relay lifecycle tracking with auto-fallback integration ──
                // Common cleanup for sessionEnded: quit Sunshine, clean global ptr,
                // then if we haven't responded yet, try the next transport.
                auto onSessionEnded = [fbState, tryNextFn](const QString& relayType) {
                    qInfo() << "[Auto]" << relayType << "sessionEnded — responded="
                            << fbState->responded;
                    if (!fbState->responded) {
                        // Use QueuedConnection to avoid re-entrancy from libdatachannel callbacks
                        QMetaObject::invokeMethod(qApp, [fbState, tryNextFn]() {
                            if (!fbState->responded && *tryNextFn) {
                                qInfo() << "[Auto] Trying next transport after session ended";
                                (*tryNextFn)();
                            }
                        }, Qt::QueuedConnection);
                    }
                };

                // WSS mode: StreamRelay tracking
                QObject::connect(session, &StreamSession::streamRelayCreated,
                    [&g_ActiveStreamRelay, &computerManager, fbState, onSessionEnded](StreamRelay* r) {
                        g_ActiveStreamRelay = r;
                        // WSS: forward deferred response immediately (already done in
                        // attemptRespond above, but ensure it here too).
                        QObject::connect(r, &StreamRelay::sessionEnded,
                            [r, &g_ActiveStreamRelay, &computerManager, fbState, onSessionEnded]() {
                                auto* identity = IdentityManager::get();
                                auto* quitReply = computerManager.http()->quitAppAsync(
                                    fbState->host->activeAddress,
                                    fbState->host->activeHttpsPort,
                                    identity->getCertificate(),
                                    identity->getPrivateKey());
                                QObject::connect(quitReply, &QNetworkReply::finished,
                                    quitReply, &QNetworkReply::deleteLater);
                                r->stop();
                                r->deleteLater();
                                if (g_ActiveStreamRelay == r)
                                    g_ActiveStreamRelay = nullptr;
                                onSessionEnded(QStringLiteral("StreamRelay"));
                            });
                    });

                // WebRTC DataChannel mode: DataChannelRelay tracking
                QObject::connect(session, &StreamSession::relayCreated,
                    [&g_ActiveRelay, &computerManager, fbState, onSessionEnded](DataChannelRelay* r) {
                        g_ActiveRelay = r;
                        // Response already forwarded in attemptRespond (no more deferred response).
                        // Log ICE connection for diagnostics only.
                        QObject::connect(r, &DataChannelRelay::dataChannelsOpen,
                            r, [fbState]() {
                                qInfo() << "[Auto]" << fbState->currentMode
                                        << "ICE connected (response already sent)";
                            });
                        QObject::connect(r, &DataChannelRelay::sessionEnded,
                            [r, &g_ActiveRelay, &computerManager, fbState, onSessionEnded]() {
                                auto* identity = IdentityManager::get();
                                auto* quitReply = computerManager.http()->quitAppAsync(
                                    fbState->host->activeAddress,
                                    fbState->host->activeHttpsPort,
                                    identity->getCertificate(),
                                    identity->getPrivateKey());
                                QObject::connect(quitReply, &QNetworkReply::finished,
                                    quitReply, &QNetworkReply::deleteLater);
                                r->stop();
                                r->deleteLater();
                                if (g_ActiveRelay == r)
                                    g_ActiveRelay = nullptr;
                                onSessionEnded(QStringLiteral("DataChannelRelay"));
                            });
                    });

                // WebRTC Media Track mode: MediaTrackRelay tracking
                QObject::connect(session, &StreamSession::mediaTrackRelayCreated,
                    [&g_ActiveMediaTrackRelay, &computerManager, fbState, onSessionEnded](MediaTrackRelay* r) {
                        g_ActiveMediaTrackRelay = r;
                        // Response already forwarded in attemptRespond (no more deferred response).
                        // Log ICE connection for diagnostics only.
                        QObject::connect(r, &MediaTrackRelay::dataChannelsOpen,
                            r, [fbState]() {
                                qInfo() << "[Auto]" << fbState->currentMode
                                        << "ICE connected (response already sent)";
                            });
                        QObject::connect(r, &MediaTrackRelay::sessionEnded,
                            [r, &g_ActiveMediaTrackRelay, &computerManager, fbState, onSessionEnded]() {
                                auto* identity = IdentityManager::get();
                                auto* quitReply = computerManager.http()->quitAppAsync(
                                    fbState->host->activeAddress,
                                    fbState->host->activeHttpsPort,
                                    identity->getCertificate(),
                                    identity->getPrivateKey());
                                QObject::connect(quitReply, &QNetworkReply::finished,
                                    quitReply, &QNetworkReply::deleteLater);
                                r->stop();
                                r->deleteLater();
                                if (g_ActiveMediaTrackRelay == r)
                                    g_ActiveMediaTrackRelay = nullptr;
                                onSessionEnded(QStringLiteral("MediaTrackRelay"));
                            });
                    });

                // sessionFailed: launchApp error, RTSP failure, etc.
                QObject::connect(session, &StreamSession::sessionFailed,
                    session, [fbState, tryNextFn](const QString& error) {
                        if (fbState->responded) return;
                        qInfo() << "[Auto] sessionFailed:" << error
                                << "— trying next transport";
                        QMetaObject::invokeMethod(qApp, [tryNextFn]() {
                            if (*tryNextFn) {
                                (*tryNextFn)();
                            }
                        }, Qt::QueuedConnection);
                    });

                session->start();
            };

            // Store in shared_ptr so orphan-relay sessionEnded handlers
            // always see a valid function, never a dangling reference.
            *tryNextFn = tryNext;

            // Start the chain with the first transport
            tryNext();
        }
    });

    // Phase 5: Quit running app
    server.router()->postAsync("/api/hosts/:id/quit",
        [&computerManager, &g_ActiveRelay, &g_ActiveStreamRelay, &g_ActiveMediaTrackRelay](const HttpRequest& req, ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        qInfo() << "[quit] ENTER — uuid=" << uuid << "relay=" << g_ActiveRelay.data()
                << "relay valid=" << (!g_ActiveRelay.isNull());

        if (uuid.isEmpty()) {
            qWarning() << "[quit] Empty uuid, returning 400";
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }

        NvComputer* host = computerManager.getHost(uuid);
        if (!host) {
            qWarning() << "[quit] Host not found for uuid=" << uuid;
            respond(HttpResponse::error(404, "Host not found"));
            return;
        }
        qInfo() << "[quit] Host found:" << host->name << host->activeAddress.address()
                << ":" << host->activeHttpsPort;

        // Stop the transport relay first (closes PeerConnection or WS)
        bool relayStopped = false;

        if (g_ActiveRelay) {
            qInfo() << "[quit] WebRTC relay exists, stopping relay=" << g_ActiveRelay.data();
            DataChannelRelay* relay = g_ActiveRelay;
            g_ActiveRelay = nullptr;
            // Explicitly stop MoonlightShim before relay cleanup so LiStopConnection
            // runs on the main thread, not deferred to the relay destructor.
            if (relay->moonlightShim())
                relay->moonlightShim()->stopConnection();
            relay->stop();
            relay->deleteLater();
            relayStopped = true;
        }

        if (g_ActiveMediaTrackRelay) {
            qInfo() << "[quit] MediaTrackRelay exists, stopping relay=" << g_ActiveMediaTrackRelay.data();
            MediaTrackRelay* relay = g_ActiveMediaTrackRelay;
            g_ActiveMediaTrackRelay = nullptr;
            // Explicitly stop MoonlightShim before relay cleanup.
            if (relay->moonlightShim())
                relay->moonlightShim()->stopConnection();
            relay->stop();
            relay->deleteLater();
            relayStopped = true;
        }

        if (g_ActiveStreamRelay) {
            qInfo() << "[quit] StreamRelay exists, stopping relay=" << g_ActiveStreamRelay.data();
            StreamRelay* relay = g_ActiveStreamRelay;
            g_ActiveStreamRelay = nullptr;
            relay->stop();
            relay->deleteLater();
            relayStopped = true;
        }

        if (!relayStopped) {
            qInfo() << "[quit] No active relay (already stopped or never started)";
        }

        qInfo() << "[quit] Sending quitAppAsync to Sunshine ...";
        auto* identity = IdentityManager::get();
        QNetworkReply* reply = computerManager.http()->quitAppAsync(
            host->activeAddress, host->activeHttpsPort,
            identity->getCertificate(), identity->getPrivateKey());
        qInfo() << "[quit] quitAppAsync reply=" << reply;

        // Wait for quit to complete, then respond
        QObject::connect(reply, &QNetworkReply::finished, [reply, respond]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                qWarning() << "[quit] Sunshine quit failed: error=" << reply->error()
                           << "errorString=" << reply->errorString();
                qInfo() << "[quit] EXIT — returning 502";
                respond(HttpResponse::error(502, "Quit failed: " + reply->errorString()));
            } else {
                QByteArray body = reply->readAll();
                qInfo() << "[quit] Sunshine quit OK, body size=" << body.size()
                        << "body=" << body.left(200);
                QJsonObject result;
                result["status"] = "quit";
                qInfo() << "[quit] EXIT — returning 200 OK";
                respond(HttpResponse::json(result));
            }
        });
    });

    if (!server.start(httpsPort))
        return 1;

    // Persist active ports (may differ from preferred due to fallback)
    {
        quint16 activeHttps = server.activeHttpsPort();
        if (activeHttps > 0 && appSettings.httpsPort(0) != activeHttps)
            appSettings.setHttpsPort(activeHttps);

        quint16 activeHttp = server.httpPort();
        if (appSettings.httpPort(0) != activeHttp)
            appSettings.setHttpPort(activeHttp);
    }

    // Sync UPnP port mapping port with the actual server port
    internetAccess.setPorts(server.httpPort(), server.activeHttpsPort());

    // Auto-start Internet Access if it was enabled before last shutdown.
    // This handles DNS registration + public IP detection at boot without
    // waiting for the user to toggle the checkbox in the UI.
    if (appSettings.internetAccessEnabled()) {
        qInfo() << "[main] internet_access_enabled is true — auto-starting...";
        internetAccess.start();
        QJsonObject st = internetAccess.statusJson();
        qInfo() << "[main] auto-start completed — active:" << internetAccess.isActive()
                << "domain:" << st.value("domain").toString()
                << "lastError:" << st.value("last_error").toString();
    }

    // Configure HttpServer to proxy WebSocket upgrades to the signaling server.
    // Both HTTPS and WebSocket signaling share the same port (443 by default).
    server.setSignalingPort(signalingPort);
    // Legacy WSS StreamRelay uses the next port for its local WS server.
    server.setStreamRelayPort(signalingPort + 1);

    // — Internet Access via PowerDNS —

    // API route: get Internet Access status
    server.router()->get("/api/internet/status", [&](const HttpRequest&) {
        return HttpResponse::json(internetAccess.statusJson());
    });

    // API route: enable/configure Internet Access
    server.router()->post("/api/internet/enable", [&](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();

        if (body.contains("unique_id"))
            appSettings.setUniqueId(body["unique_id"].toString());
        if (body.contains("pdns_token"))
            appSettings.setPdnsToken(body["pdns_token"].toString());
        if (body.contains("auto_ip_detection"))
            appSettings.setAutoIpDetection(body["auto_ip_detection"].toBool());
        if (body.contains("transport_mode"))
            appSettings.setTransportMode(body["transport_mode"].toString());
        if (body.contains("public_ip"))
            appSettings.setPublicIp(body["public_ip"].toString());

        if (body.contains("internet_access_enabled"))
            appSettings.setInternetAccessEnabled(body["internet_access_enabled"].toBool());
        if (body.contains("upnp_enabled"))
            appSettings.setUpnpEnabled(body["upnp_enabled"].toBool());

        bool enabled = body.value("internet_access_enabled").toBool(
            appSettings.internetAccessEnabled());

        if (enabled) {
            qInfo() << "[main] POST /api/internet/enable — calling internetAccess.start()...";
            internetAccess.start();
            QJsonObject obj = internetAccess.statusJson();
            qInfo() << "[main] internetAccess.start() completed — active:" << internetAccess.isActive()
                    << "lastError:" << obj.value("last_error").toString();
            obj["status"] = "enabled";
            return HttpResponse::json(obj);
        } else {
            internetAccess.stop();
            QJsonObject obj;
            obj["status"] = "disabled";
            obj["internet_access_enabled"] = false;
            return HttpResponse::json(obj);
        }
    });

    // API route: disable Internet Access
    server.router()->post("/api/internet/disable", [&](const HttpRequest&) {
        internetAccess.stop();
        appSettings.setInternetAccessEnabled(false);

        QJsonObject obj;
        obj["status"] = "disabled";
        return HttpResponse::json(obj);
    });

    // API route: force refresh (re-check IP, DNS, certificate)
    server.router()->post("/api/internet/refresh", [&](const HttpRequest&) {
        internetAccess.forceRefresh();
        return HttpResponse::json(internetAccess.statusJson());
    });

    // API route: renew TLS certificate
    server.router()->post("/api/internet/renew-cert", [&](const HttpRequest&) {
        internetAccess.renewCertificate();
        QJsonObject obj;
        obj["status"] = "renewing";
        return HttpResponse::json(obj);
    });

    // — Admin settings (localhost only, server config) —

    server.router()->get("/api/admin/settings", [&server, &appSettings](const HttpRequest&) {
        QJsonObject obj;
        obj["http_port"] = static_cast<int>(server.httpPort());
        obj["https_port"] = static_cast<int>(server.activeHttpsPort());
        return HttpResponse::json(obj);
    });

    server.router()->post("/api/admin/settings", [&server, &appSettings](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();

        if (body.contains("https_port")) {
            quint16 newPort = static_cast<quint16>(body["https_port"].toInt(443));
            quint16 oldPort = server.activeHttpsPort();

            appSettings.setHttpsPort(newPort);

            QJsonObject obj;
            obj["https_port"] = static_cast<int>(newPort);

            if (newPort == oldPort || oldPort == 0) {
                obj["status"] = "saved";
            } else {
                obj["status"] = "saved";
                obj["port_changed"] = true;

                qInfo() << "[admin] Scheduled deferred HTTPS port change from"
                        << oldPort << "to" << newPort;

                QTimer::singleShot(0, [&server, newPort, oldPort]() {
                    qInfo() << "[admin] Deferred: changing HTTPS port to" << newPort;
                    if (!server.changeHttpsPort(newPort)) {
                        qWarning() << "[admin] Port change failed, restoring" << oldPort;
                        server.changeHttpsPort(oldPort);
                    }
                });
            }

            return HttpResponse::json(obj);
        }

        return HttpResponse::error(400, "No supported settings provided");
    });

    // — Streaming settings —

    server.router()->get("/api/settings/streaming", [&appSettings](const HttpRequest&) {
        QJsonObject obj;

        // Normalise "auto" → "hevc" (the old "auto" default is replaced by explicit HEVC).
        // Existing settings.json entries with "video_codec":"auto" are migrated on read.
        VideoCodec codec = appSettings.videoCodec();
        QString codecStr = AppSettings::videoCodecToString(codec);
        if (codecStr == "auto")
            codecStr = "hevc";
        obj["video_codec"] = codecStr;

        obj["gaming_mode"] = appSettings.gamingMode();
        obj["show_performance_stats"] = appSettings.showPerformanceStats();
        obj["upnp_enabled"] = appSettings.upnpEnabled();
        obj["transport"] = appSettings.transport();
        obj["stun_server"] = appSettings.stunServer();
        obj["internet_access_enabled"] = appSettings.internetAccessEnabled();
        obj["transport_mode"] = appSettings.transportMode();
        obj["auto_ip_detection"] = appSettings.autoIpDetection();
        obj["stream_bitrate"] = appSettings.streamBitrate();
        obj["stream_height"] = appSettings.streamHeight();
        obj["stream_fps"] = appSettings.streamFps();
        return HttpResponse::json(obj);
    });

    server.router()->post("/api/settings/streaming", [&appSettings](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();

        QJsonObject obj;
        bool hadChange = false;

        if (body.contains("video_codec")) {
            VideoCodec codec = AppSettings::videoCodecFromString(body["video_codec"].toString());
            appSettings.setVideoCodec(codec);
            obj["video_codec"] = AppSettings::videoCodecToString(codec);
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("gaming_mode")) {
            bool enabled = body["gaming_mode"].toBool();
            appSettings.setGamingMode(enabled);
            obj["gaming_mode"] = enabled;
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("show_performance_stats")) {
            bool enabled = body["show_performance_stats"].toBool();
            appSettings.setShowPerformanceStats(enabled);
            obj["show_performance_stats"] = enabled;
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("stream_bitrate")) {
            int kbps = body["stream_bitrate"].toInt(20000);
            appSettings.setStreamBitrate(kbps);
            obj["stream_bitrate"] = appSettings.streamBitrate();
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("stream_height")) {
            int height = body["stream_height"].toInt(1080);
            appSettings.setStreamHeight(height);
            obj["stream_height"] = appSettings.streamHeight();
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("stream_fps")) {
            int fps = body["stream_fps"].toInt(60);
            appSettings.setStreamFps(fps);
            obj["stream_fps"] = appSettings.streamFps();
            obj["status"] = "saved";
            hadChange = true;
        }

        if (body.contains("upnp_enabled")) {
            bool enabled = body["upnp_enabled"].toBool();
            appSettings.setUpnpEnabled(enabled);
            obj["upnp_enabled"] = enabled;
            obj["status"] = "saved";
            hadChange = true;
        }

        if (!hadChange)
            return HttpResponse::error(400, "No supported settings provided");

        return HttpResponse::json(obj);
    });

    // Phase N: System tray icon
    TrayManager trayManager(&server);
    trayManager.init();

    Logger::info("Server ready. Open https://localhost" + (httpsPort != 443 ? ":" + QString::number(server.activeHttpsPort()) : QString()) + " in your browser.");

    return app.exec();
}