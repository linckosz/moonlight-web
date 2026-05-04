#include <QCoreApplication>
#include <QCommandLineParser>
#include <QJsonDocument>
#include "server/HttpServer.h"
#include "server/RestRouter.h"
#include "common/Logger.h"
#include "backend/ComputerManager.h"
#include "backend/IdentityManager.h"

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

    // Register API routes
    server.router()->get("/api/health", [](const HttpRequest&) {
        QJsonObject obj;
        obj["status"] = "ok";
        obj["version"] = QCoreApplication::applicationVersion();
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

        computerManager.handleAddManualHost(address);

        QJsonObject obj;
        obj["status"] = "adding";
        return HttpResponse::json(obj);
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

    if (!server.start())
        return 1;

    Logger::info("Server ready. Open http://localhost:" + QString::number(port) + " in your browser.");

    return app.exec();
}
