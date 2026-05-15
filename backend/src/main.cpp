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
#include "streaming/DataChannelRelay.h"
#include "network/NportClient.h"
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
    Logger::info("[main] Settings: http_port=" + QString::number(httpPort)
                 + ", https_port=" + QString::number(httpsPort)
                 + ", video_codec=" + AppSettings::videoCodecToString(preferredCodec));

    // Phase 5b: WebRTC DataChannel relay + signaling tracking
    quint16 signalingPort = parser.value("ws-port").toUShort();
    QPointer<DataChannelRelay> g_ActiveRelay;
    NportClient nportClient;

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
    server.router()->postAsync("/api/hosts/:id/start",
        [&computerManager, signalingPort, &g_ActiveRelay, &server, &appSettings, &nportClient](const HttpRequest& req, ResponseCallback respond) {
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
        // This works for both LAN and remote access:
        //   LAN:    Host: localhost:443  → wss://localhost/ws
        //   Remote: Host: moonlightweb-XXXX.nport.link → wss://moonlightweb-XXXX.nport.link/ws
        // The browser already knows how to reach the server — use the same host.
        QString serverHost = req.headers.value("host");
        int colon = serverHost.indexOf(':');
        if (colon >= 0)
            serverHost = serverHost.left(colon);

        auto* session = new StreamSession(
            host, appId,
            computerManager.http(),
            std::move(respond),
            signalingPort,
            serverHost,
            appSettings.videoCodec(),
            appSettings.gamingMode()
        );

        // Inform the session about the effective HTTPS port (for wsUrl()
        // construction when the WebSocket shares the same HTTPS port).
        session->setHttpsPort(server.activeHttpsPort());

        // Track the DataChannelRelay for quit/cleanup
        QObject::connect(session, &StreamSession::relayCreated,
            [&g_ActiveRelay, &nportClient](DataChannelRelay* relay) {
                qInfo() << "[main] relayCreated, relay=" << relay;
                g_ActiveRelay = relay;

                // Resume nport refresh once signaling is done (data channels open)
                QObject::connect(relay, &DataChannelRelay::dataChannelsOpen,
                    &nportClient, &NportClient::resumeRefresh);

                // Stop + clean relay whenever the session ends (DataChannel
                // disconnect, stream error, etc.), NOT just null the pointer.
                QObject::connect(relay, &DataChannelRelay::sessionEnded,
                    [relay, &g_ActiveRelay]() {
                        qInfo() << "[main] sessionEnded fired, relay=" << relay
                                << "g_ActiveRelay=" << g_ActiveRelay.data();
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

        // Pause nport refresh during signaling to avoid breaking WS connection
        nportClient.pauseRefresh();

        // Resume refresh if the session fails before data channels open
        QObject::connect(session, &StreamSession::sessionFailed,
            &nportClient, &NportClient::resumeRefresh);

        session->start();
    });

    // Phase 5: Quit running app
    server.router()->postAsync("/api/hosts/:id/quit",
        [&computerManager, &g_ActiveRelay](const HttpRequest& req, ResponseCallback respond) {
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

        // Stop the DataChannelRelay first (closes PeerConnection + DataChannels)
        if (g_ActiveRelay) {
            qInfo() << "[quit] Relay exists, stopping relay=" << g_ActiveRelay.data();

            DataChannelRelay* relay = g_ActiveRelay;
            g_ActiveRelay = nullptr;
            qInfo() << "[quit] Calling relay->stop() ...";
            relay->stop();
            qInfo() << "[quit] relay->stop() returned OK, scheduling deleteLater";
            relay->deleteLater();
        } else {
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
    // Both HTTPS and WebSocket signaling now share the same port (443 by default),
    // so the nport tunnel exposes the full UI to remote users.
    server.setSignalingPort(signalingPort);

    // ── nport tunnel ─────────────────────────────────────────────────────────────

    // Load or auto-generate the nport subdomain at startup.
    // Stored as 8 hex chars ("92b8d122"); "moonlightweb-" prefix is
    // prepended by NportClient when launching nport.
    QString nportSubdomain = appSettings.nportSubdomain();
    if (nportSubdomain.isEmpty()) {
        // Generate 8 random hex characters
        QString hex(8, QChar('0'));
        for (int i = 0; i < 8; ++i) {
            hex[i] = QStringLiteral("0123456789abcdef")
                     .at(QRandomGenerator::global()->bounded(16));
        }
        nportSubdomain = hex;
        appSettings.setNportSubdomain(nportSubdomain);
        qInfo() << "[main] Auto-generated nport subdomain:" << nportSubdomain;
    }

    // Connect nport signals for tunnel lifecycle
    QObject::connect(&nportClient, &NportClient::tunnelReady,
        [&](const QString& url) {
            qInfo() << "[main] nport tunnel ready:" << url;
        });
    QObject::connect(&nportClient, &NportClient::tunnelError,
        [&](const QString& err) {
            qWarning() << "[main] nport tunnel error:" << err;
        });

    // Always set the persisted subdomain (auto-generated or previously saved)
    nportClient.setSubdomain(nportSubdomain);
    nportClient.setTargetPort(server.httpPort());
    qInfo() << "[main] nport subdomain:" << nportSubdomain;

    // API route: get tunnel status
    server.router()->get("/api/tunnel/status", [&](const HttpRequest&) {
        QJsonObject obj;
        obj["active"] = nportClient.isActive();
        obj["public_url"] = nportClient.isActive()
            ? nportClient.publicUrl()
            : QString();
        obj["subdomain"] = nportClient.subdomain();
        obj["available"] = nportClient.isAvailable();

        // Last error (if any) — empty string if no error
        QString err = nportClient.lastError();
        if (!err.isEmpty())
            obj["error"] = err;

        return HttpResponse::json(obj);
    });

    // API route: configure/enable the nport tunnel
    server.router()->post("/api/tunnel/configure", [&](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();

        QString subdomain = body["subdomain"].toString();
        if (subdomain.isEmpty()) {
            // Generate a subdomain based on hostname if none provided
            subdomain = nportClient.subdomain();
            if (subdomain.isEmpty()) {
                // Generate a unique subdomain (hostname + random suffix)
                const QString chars = QStringLiteral("abcdefghijklmnopqrstuvwxyz0123456789");
                auto ts = QDateTime::currentMSecsSinceEpoch();
                quint64 seed = (static_cast<quint64>(ts) << 16)
                             ^ QRandomGenerator::global()->generate64();
                QString shortId(8, QChar('a'));
                for (int i = 0; i < 8; ++i) {
                    shortId[i] = chars.at(static_cast<int>(seed % 36));
                    seed /= 36;
                }
                subdomain = shortId;
            }
        }

        appSettings.setNportSubdomain(subdomain);
        qInfo() << "[main] nport subdomain saved:" << subdomain;

        // (Re)start the tunnel
        nportClient.stop();
        nportClient.setSubdomain(subdomain);
        nportClient.setTargetPort(server.httpPort());

        if (nportClient.isAvailable()) {
            nportClient.start();
        } else {
            qWarning() << "[main] nport not available (binary not found) —"
                       << "tunnel not started";
        }

        QJsonObject obj;
        obj["status"] = "configured";
        obj["subdomain"] = subdomain;
        return HttpResponse::json(obj);
    });

    // API route: disable/stop the nport tunnel (keeps subdomain for re-enable)
    server.router()->post("/api/tunnel/disable", [&](const HttpRequest&) {
        nportClient.stop();

        QJsonObject obj;
        obj["status"] = "disabled";
        return HttpResponse::json(obj);
    });

    // ── Admin settings (localhost only, server config) ────────────────────────

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

            // Save to persistent settings
            appSettings.setHttpsPort(newPort);

            QJsonObject obj;
            obj["https_port"] = static_cast<int>(newPort);

            if (newPort == oldPort || oldPort == 0) {
                // Same port or server not running — nothing to restart
                obj["status"] = "saved";
            } else {
                // Port changed: respond first, then restart the server.
                // This ensures the HTTP response is flushed to the client
                // before stop() closes all sockets.
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

    // ── Streaming settings ────────────────────────────────────────────────────

    server.router()->get("/api/settings/streaming", [&appSettings](const HttpRequest&) {
        QJsonObject obj;
        obj["video_codec"] = AppSettings::videoCodecToString(appSettings.videoCodec());
        obj["gaming_mode"] = appSettings.gamingMode();
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
