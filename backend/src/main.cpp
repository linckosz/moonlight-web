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

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QCoreApplication::setApplicationName("Moonlight-Web");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Moonlight-Web");

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
        // Works for both LAN (localhost:443) and remote access via deSEC domain.
        QString serverHost = req.headers.value("host");
        int colon = serverHost.indexOf(':');
        if (colon >= 0)
            serverHost = serverHost.left(colon);

        // Resolve transport preference: new transport_mode takes priority
        // over the legacy transport setting.
        QString transportPref = appSettings.transportMode();
        if (transportPref.isEmpty() || transportPref == "auto") {
            transportPref = appSettings.transport();
        }
        if (transportPref == "webrtc-media-udp" || transportPref == "webrtc-media-tcp")
            transportPref = "webrtc-media";
        else if (transportPref == "webrtc-dc-udp" || transportPref == "webrtc-dc-tcp")
            transportPref = "webrtc";
        else if (transportPref == "wss")
            ;
        else if (transportPref != "webrtc" && transportPref != "webrtc-media" && transportPref != "wss")
            transportPref = appSettings.transport();

        qInfo() << "[Session] Transport preference:" << transportPref;

        auto* session = new StreamSession(
            host, appId,
            computerManager.http(),
            std::move(respond),
            signalingPort,
            serverHost,
            appSettings.videoCodec(),
            appSettings.gamingMode(),
            effectiveUpnpEnabled,
            transportPref,
            stunServer,
            appSettings.streamHeight(),
            appSettings.streamFps(),
            appSettings.streamBitrate()
        );

        // Inform the session about the effective HTTPS port
        session->setHttpsPort(server.activeHttpsPort());

        // Set the port for the legacy WSS StreamRelay
        session->setStreamRelayPort(signalingPort + 1);

        // Track StreamRelay for quit/cleanup (WSS mode)
        QObject::connect(session, &StreamSession::streamRelayCreated,
            [&g_ActiveStreamRelay, &computerManager, host](StreamRelay* relay) {
                qInfo() << "[main] streamRelayCreated, relay=" << relay;
                g_ActiveStreamRelay = relay;

                QObject::connect(relay, &StreamRelay::sessionEnded,
                    [relay, &g_ActiveStreamRelay, &computerManager, host]() {
                        qInfo() << "[main] StreamRelay sessionEnded, relay=" << relay;

                        // Best-effort quit to Sunshine (session ended unexpectedly)
                        auto* identity = IdentityManager::get();
                        auto* quitReply = computerManager.http()->quitAppAsync(
                            host->activeAddress, host->activeHttpsPort,
                            identity->getCertificate(), identity->getPrivateKey());
                        QObject::connect(quitReply, &QNetworkReply::finished, quitReply, &QNetworkReply::deleteLater);

                        relay->stop();
                        relay->deleteLater();
                        if (g_ActiveStreamRelay == relay) {
                            g_ActiveStreamRelay = nullptr;
                        }
                    });
            });

        // Track the DataChannelRelay for quit/cleanup (WebRTC mode)
        QObject::connect(session, &StreamSession::relayCreated,
            [&g_ActiveRelay, &computerManager, host](DataChannelRelay* relay) {
                qInfo() << "[main] relayCreated, relay=" << relay;
                g_ActiveRelay = relay;

                QObject::connect(relay, &DataChannelRelay::sessionEnded,
                    [relay, &g_ActiveRelay, &computerManager, host]() {
                        qInfo() << "[main] sessionEnded fired, relay=" << relay
                                << "g_ActiveRelay=" << g_ActiveRelay.data();

                        // Best-effort quit to Sunshine (session ended unexpectedly)
                        auto* identity = IdentityManager::get();
                        auto* quitReply = computerManager.http()->quitAppAsync(
                            host->activeAddress, host->activeHttpsPort,
                            identity->getCertificate(), identity->getPrivateKey());
                        QObject::connect(quitReply, &QNetworkReply::finished, quitReply, &QNetworkReply::deleteLater);

                        relay->stop();
                        relay->deleteLater();
                        if (g_ActiveRelay == relay) {
                            qInfo() << "[main] sessionEnded — clearing g_ActiveRelay (was us)";
                            g_ActiveRelay = nullptr;
                        } else {
                            qInfo() << "[main] sessionEnded — g_ActiveRelay already cleared by quit handler";
                        }
                    });
            });

        // Track the MediaTrackRelay for quit/cleanup (WebRTC media mode)
        QObject::connect(session, &StreamSession::mediaTrackRelayCreated,
            [&g_ActiveMediaTrackRelay, &computerManager, host](MediaTrackRelay* relay) {
                qInfo() << "[main] mediaTrackRelayCreated, relay=" << relay;
                g_ActiveMediaTrackRelay = relay;

                QObject::connect(relay, &MediaTrackRelay::sessionEnded,
                    [relay, &g_ActiveMediaTrackRelay, &computerManager, host]() {
                        qInfo() << "[main] MediaTrackRelay sessionEnded, relay=" << relay;

                        // Best-effort quit to Sunshine (session ended unexpectedly)
                        auto* identity = IdentityManager::get();
                        auto* quitReply = computerManager.http()->quitAppAsync(
                            host->activeAddress, host->activeHttpsPort,
                            identity->getCertificate(), identity->getPrivateKey());
                        QObject::connect(quitReply, &QNetworkReply::finished, quitReply, &QNetworkReply::deleteLater);

                        relay->stop();
                        relay->deleteLater();
                        if (g_ActiveMediaTrackRelay == relay) {
                            g_ActiveMediaTrackRelay = nullptr;
                        }
                    });
            });

        session->start();
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
        if (appSettings.httpsPort(0) != activeHttps)
            appSettings.setHttpsPort(activeHttps);

        quint16 activeHttp = server.httpPort();
        if (appSettings.httpPort(0) != activeHttp)
            appSettings.setHttpPort(activeHttp);
    }

    // Configure HttpServer to proxy WebSocket upgrades to the signaling server.
    // Both HTTPS and WebSocket signaling share the same port (443 by default).
    server.setSignalingPort(signalingPort);
    // Legacy WSS StreamRelay uses the next port for its local WS server.
    server.setStreamRelayPort(signalingPort + 1);

    // — Internet Access via deSEC —

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
        if (body.contains("desec_token"))
            appSettings.setDesecToken(body["desec_token"].toString());
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