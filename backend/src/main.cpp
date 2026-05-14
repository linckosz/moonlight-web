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
#include "network/ZrokClient.h"
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

    // Start HTTP server
    quint16 port = parser.value("port").toUShort();
    HttpServer server(port);

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

    // Read persistent settings (https_port, video_codec, zrok_token, ...)
    AppSettings appSettings;
    quint16 httpsPort = appSettings.httpsPort(443);
    VideoCodec preferredCodec = appSettings.videoCodec();
    Logger::info("[main] Settings: https_port=" + QString::number(httpsPort)
                 + ", video_codec=" + AppSettings::videoCodecToString(preferredCodec));

    // Phase 5b: WebRTC DataChannel relay + signaling tracking
    quint16 signalingPort = parser.value("ws-port").toUShort();
    QPointer<DataChannelRelay> g_ActiveRelay;
    ZrokClient zrokClient;

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
        [&computerManager, signalingPort, &g_ActiveRelay, &server, &appSettings, &zrokClient](const HttpRequest& req, ResponseCallback respond) {
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

        // Determine the signaling WS URL for the browser.
        // If zrok is active, the browser connects via the public zrok URL.
        // Otherwise, use the local serverHost.
        QString serverHost;
        QString explicitWsUrl;
        if (zrokClient.isActive()) {
            // zrok provides "moonlightweb-xxxx.share.zrok.io" (no scheme).
            // The browser connects via wss://<host> (port 443, default for WSS).
            QString zrokHost = zrokClient.publicUrl();
            explicitWsUrl = "wss://" + zrokHost;
            serverHost = zrokHost;
            qInfo() << "[main] Using zrok signaling URL:" << explicitWsUrl;
        } else {
            // Local LAN: use the Host header from the browser's request.
            serverHost = req.headers.value("host");
            int colon = serverHost.indexOf(':');
            if (colon >= 0)
                serverHost = serverHost.left(colon);
        }

        auto* session = new StreamSession(
            host, appId,
            computerManager.http(),
            std::move(respond),
            signalingPort,
            serverHost,
            appSettings.videoCodec(),
            appSettings.gamingMode()
        );

        // Apply zrok override URL if active
        if (!explicitWsUrl.isEmpty())
            session->setExplicitWsUrl(explicitWsUrl);

        // Track the DataChannelRelay for quit/cleanup
        QObject::connect(session, &StreamSession::relayCreated,
            [&g_ActiveRelay](DataChannelRelay* relay) {
                qInfo() << "[main] relayCreated, relay=" << relay;
                g_ActiveRelay = relay;
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

    // Persist the active HTTPS port (may differ from preferred port due to fallback)
    {
        quint16 activePort = server.activeHttpsPort();
        if (appSettings.httpsPort(0) != activePort)
            appSettings.setHttpsPort(activePort);
    }

    // ── zrok tunnel (replaces DuckDNS for remote signaling WS access) ─────

    // Load persisted zrok config (token + reserved name)
    QString zrokToken = appSettings.zrokToken();
    QString zrokReservedName = appSettings.zrokReservedName();
    if (!zrokToken.isEmpty()) {
        zrokClient.setToken(zrokToken);
        zrokClient.setTargetPort(signalingPort);

        if (!zrokReservedName.isEmpty()) {
            zrokClient.setReservedName(zrokReservedName);
        }

        qInfo() << "[main] zrok token configured, reservedName="
                << (zrokReservedName.isEmpty() ? "<not reserved>" : zrokReservedName);

        zrokClient.start();
    }

    // API route: get zrok tunnel status
    server.router()->get("/api/zrok/status", [&](const HttpRequest&) {
        QJsonObject obj;
        obj["active"] = zrokClient.isActive();
        obj["public_url"] = zrokClient.isActive()
            ? "wss://" + zrokClient.publicUrl()
            : QString();
        obj["reserved_name"] = zrokClient.reservedName();
        obj["token_configured"] = !appSettings.zrokToken().isEmpty();
        return HttpResponse::json(obj);
    });

    // API route: configure zrok token (triggers tunnel start)
    server.router()->post("/api/zrok/configure", [&](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        if (!body.contains("token")) {
            return HttpResponse::error(400, "Missing 'token' field");
        }

        QString token = body["token"].toString();
        appSettings.setZrokToken(token);

        qInfo() << "[main] zrok token saved";

        // (Re)start the zrok client with the new token
        zrokClient.stop();
        zrokClient.setToken(token);

        // Generate a reserved name if not already set
        if (zrokClient.reservedName().isEmpty()) {
            // 8-char alphanumeric ID (a-z, 0-9), generated once and persisted.
            // Uses timestamp + random suffix for high-probability uniqueness.
            const QString chars = QStringLiteral("abcdefghijklmnopqrstuvwxyz0123456789");
            auto ts = QDateTime::currentMSecsSinceEpoch();
            // Mix timestamp bits and random bits into a 48-bit seed
            quint64 seed = (static_cast<quint64>(ts) << 16) ^ QRandomGenerator::global()->generate64();
            QString shortId(8, QChar('a'));
            for (int i = 0; i < 8; ++i) {
                shortId[i] = chars.at(static_cast<int>(seed % 36));
                seed /= 36;
            }
            QString name = "moonlightweb-" + shortId;
            zrokClient.setReservedName(name);
            appSettings.setZrokReservedName(name);
            qInfo() << "[main] Generated zrok reserved name:" << name;
        }

        zrokClient.setTargetPort(signalingPort);
        zrokClient.start();

        QJsonObject obj;
        obj["status"] = "configured";
        obj["reserved_name"] = zrokClient.reservedName();
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
