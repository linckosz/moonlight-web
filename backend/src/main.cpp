#include <QCoreApplication>
#include <QCommandLineParser>
#include "server/HttpServer.h"
#include "server/RestRouter.h"
#include "common/Logger.h"

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

    // Register API routes (placeholder for future phases)
    server.router()->get("/api/health", [](const HttpRequest&) {
        QJsonObject obj;
        obj["status"] = "ok";
        obj["version"] = QCoreApplication::applicationVersion();
        return HttpResponse::json(obj);
    });

    if (!server.start())
        return 1;

    Logger::info("Server ready. Open http://localhost:" + QString::number(port) + " in your browser.");

    return app.exec();
}
