#include <QCoreApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QStandardPaths>
#include "server/HttpServer.h"
#include "server/RestRouter.h"
#include "common/Logger.h"
#include "backend/ComputerManager.h"
#include "backend/IdentityManager.h"
#include "streaming/Session.h"
#include "streaming/StreamRelay.h"
#include "network/DdnsClient.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName("Moonlight-Web");
    QCoreApplication::setApplicationVersion("0.1.0");
    QCoreApplication::setOrganizationName("Moonlight-Web");

    // Parse command line
    QCommandLineParser parser;
    parser.setApplicationDescription("Moonlight-Web streaming server");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption portOption("port", "HTTP server port", "port", "48000");
    parser.addOption(portOption);

    QCommandLineOption logOption("log", "Log file path", "path");
    parser.addOption(logOption);

    QCommandLineOption wsPortOption("ws-port", "WebSocket relay port", "port", "48001");
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

    // Phase 5b: WebSocket relay tracking
    quint16 wsPort = parser.value("ws-port").toUShort();
    QPointer<StreamRelay> g_ActiveRelay;

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
        [&computerManager, wsPort, &g_ActiveRelay, &server](const HttpRequest& req, ResponseCallback respond) {
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

        // Extract server host from the request Host header
        QString serverHost = req.headers.value("host");
        int colon = serverHost.indexOf(':');
        if (colon >= 0)
            serverHost = serverHost.left(colon);

        auto* session = new StreamSession(
            host, appId,
            computerManager.http(),
            std::move(respond),
            wsPort,
            server.sslConfiguration(),
            serverHost
        );

        // Track the relay for quit/cleanup
        QObject::connect(session, &StreamSession::relayCreated,
            [&g_ActiveRelay](StreamRelay* relay) {
                qInfo() << "[main] relayCreated, relay=" << relay;
                g_ActiveRelay = relay;
                // Stop + clean relay whenever the session ends (WS disconnect,
                // stream error, etc.), NOT just null the pointer. This prevents
                // a race where WS close arrives before the HTTP /quit request,
                // leaving the relay's WS server bound to the port.
                QObject::connect(relay, &StreamRelay::sessionEnded,
                    [relay, &g_ActiveRelay]() {
                        qInfo() << "[main] sessionEnded fired, relay=" << relay
                                << "g_ActiveRelay=" << g_ActiveRelay.data();
                        relay->stop();
                        relay->deleteLater();
                        // Only clear g_ActiveRelay if it still points to us.
                        // The HTTP /quit handler may have already cleared it
                        // before calling relay->stop(), which can trigger
                        // sessionEnded synchronously.
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

        // Stop the stream relay first
        if (g_ActiveRelay) {
            qInfo() << "[quit] Relay exists, stopping relay=" << g_ActiveRelay.data();

            // Save relay pointer locally BEFORE clearing g_ActiveRelay.
            // stop() can trigger sessionEnded synchronously (via WS close),
            // which fires the sessionEnded lambda that sets g_ActiveRelay
            // to nullptr via QPointer. If we don't save the pointer first,
            // the subsequent g_ActiveRelay->deleteLater() below would
            // dereference a null QPointer -> crash.
            StreamRelay* relay = g_ActiveRelay;
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

    // Read preferred HTTPS port from settings.json (persisted across restarts)
    quint16 httpsPort = 443;
    {
        QString settingsDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(settingsDir);
        QString settingsPath = settingsDir + "/settings.json";
        QFile file(settingsPath);
        if (file.open(QIODevice::ReadOnly)) {
            QJsonObject settings = QJsonDocument::fromJson(file.readAll()).object();
            QJsonObject::iterator portIt = settings.find("https_port");
            if (portIt != settings.end())
                httpsPort = static_cast<quint16>(portIt->toInt());
            file.close();
        }
    }

    if (!server.start(httpsPort))
        return 1;

    // Persist the active HTTPS port (may differ from preferred port due to fallback)
    {
        QString settingsDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QString settingsPath = settingsDir + "/settings.json";
        QFile file(settingsPath);
        QJsonObject settings;
        if (file.open(QIODevice::ReadOnly)) {
            settings = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
        }

        int activePort = static_cast<int>(server.activeHttpsPort());
        if (settings.value("https_port").toInt() != activePort) {
            settings["https_port"] = activePort;
            if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                file.write(QJsonDocument(settings).toJson(QJsonDocument::Indented));
                file.close();
            }
        }
    }

    // Phase N: DuckDNS dynamic DNS
    bool ddnsConsentPending = false;
    bool ddnsConsentResult = false;

    DdnsClient ddns([&server]() {
        // Cert reload callback — called after Let's Encrypt cert obtained
        qInfo() << "[main] Let's Encrypt cert obtained, reloading TLS...";
        if (!server.reloadTls()) {
            qWarning() << "[main] TLS reload failed";
        }
    });

    QObject::connect(&ddns, &DdnsClient::consentRequired, [&]() {
        qInfo() << "[main] DuckDNS consent required";
        ddnsConsentPending = true;
    });

    QObject::connect(&ddns, &DdnsClient::registered,
        [](const QString& subdomain, const QString& ip, quint16 httpsPort) {
        qInfo() << "[main] DuckDNS registered:" << subdomain << ":" << httpsPort << "->" << ip;
    });

    QObject::connect(&ddns, &DdnsClient::certObtained, []() {
        qInfo() << "[main] Let's Encrypt certificate obtained and loaded";
    });

    QObject::connect(&ddns, &DdnsClient::errorOccurred, [](const QString& msg) {
        qWarning() << "[main] DuckDNS error:" << msg;
    });

    // API route: get DuckDNS consent status
    server.router()->get("/api/ddns/consent", [&](const HttpRequest&) {
        QJsonObject obj;
        obj["asked"] = !ddnsConsentPending && (ddns.isActive() || !ddns.subdomain().isEmpty());
        obj["pending"] = ddnsConsentPending;
        obj["active"] = ddns.isActive();
        if (ddns.isActive()) {
            obj["subdomain"] = ddns.subdomain();
        }
        return HttpResponse::json(obj);
    });

    // API route: submit DuckDNS consent decision
    server.router()->post("/api/ddns/consent", [&](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        if (!body.contains("granted")) {
            return HttpResponse::error(400, "Missing 'granted' field");
        }

        bool granted = body["granted"].toBool();
        ddnsConsentPending = false;
        ddnsConsentResult = granted;
        ddns.setConsent(granted);

        QJsonObject obj;
        obj["status"] = granted ? "accepted" : "declined";
        return HttpResponse::json(obj);
    });

    // API route: configure DuckDNS token (must be done before start)
    server.router()->post("/api/ddns/configure", [&](const HttpRequest& req) {
        QJsonDocument doc = QJsonDocument::fromJson(req.body);
        QJsonObject body = doc.object();
        if (!body.contains("token")) {
            return HttpResponse::error(400, "Missing 'token' field");
        }

        QString token = body["token"].toString();
        // Save token to settings (DdnsClient.loadSettings will pick it up on next start)
        QString settingsDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir().mkpath(settingsDir);
        QString settingsPath = settingsDir + "/settings.json";
        QFile file(settingsPath);
        QJsonObject settings;
        if (file.open(QIODevice::ReadOnly)) {
            settings = QJsonDocument::fromJson(file.readAll()).object();
            file.close();
        }
        settings["ddns_token"] = token;
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(QJsonDocument(settings).toJson(QJsonDocument::Indented));
            file.close();
        }

        qInfo() << "[main] DuckDNS token configured";

        // Apply token and re-trigger DuckDNS registration
        ddns.configure(token);

        QJsonObject obj;
        obj["status"] = "configured";
        return HttpResponse::json(obj);
    });

    // Start DuckDNS workflow (will emit consentRequired if first launch)
    ddns.setHttpsPort(server.activeHttpsPort());
    ddns.start();

    Logger::info("Server ready. Open http://localhost:" + QString::number(port) + " in your browser.");

    return app.exec();
}
