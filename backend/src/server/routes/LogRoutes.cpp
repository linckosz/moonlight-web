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

#include "server/routes/LogRoutes.h"
#include "common/Edition.h"
#include "common/Logger.h"
#include "common/PrivilegedPlace.h"
#include "common/RunFlags.h"
#include "server/AppSettings.h"
#include "server/ClientLogLines.h"
#include "server/HttpServer.h"
#include "server/LogArchive.h"
#include "server/RestRouter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSysInfo>

namespace {

// A page sends at most a few hundred lines every few seconds; these bound what
// a misbehaving one (or anyone with a session) can push through per request.
// The file's own rotation bounds the rest.
constexpr qsizetype kMaxBody = 512 * 1024;
constexpr qsizetype kMaxLines = 2000;

QString onOff(bool on, bool fromCli)
{
    if (!on) return QStringLiteral("off");
    return fromCli ? QStringLiteral("on (command line)") : QStringLiteral("on (admin page)");
}

// What wrote these logs: the first thing anyone reading them asks.
QByteArray aboutText()
{
    QString s;
    s += QStringLiteral("MoonlightWeb %1 (%2)\n")
             .arg(QCoreApplication::applicationVersion(), mw::edition::displayName());
    s += QStringLiteral("Archived: %1\n")
             .arg(QDateTime::currentDateTime().toString(Qt::ISODateWithMs));
    s += QStringLiteral("System: %1, %2 %3, %4\n")
             .arg(QSysInfo::prettyProductName(), QSysInfo::kernelType(), QSysInfo::kernelVersion(),
                  QSysInfo::currentCpuArchitecture());
    s += QStringLiteral("Qt: %1\n").arg(QString::fromLatin1(qVersion()));
    s += QStringLiteral("Debug mode: %1\n").arg(onOff(mw::run::debug(), mw::run::debugFromCli()));
    s += QStringLiteral("Verbose logs: %1\n")
             .arg(onOff(mw::run::verbose(), mw::run::verboseFromCli()));
    if (!AppSettings::fileOverride().isEmpty())
        s += QStringLiteral("Settings file: %1 (--config)\n")
                 .arg(QDir::toNativeSeparators(AppSettings::fileOverride()));
    s += QStringLiteral("\nEach log is the latest non-empty one of its kind; a log that rotates "
                        "comes with its previous files, %1 in all.\n\n")
             .arg(LogArchive::kGenerationsKept);
    s += LogScrubber::notice();
    return s.toUtf8();
}

// No machine name in it: a file's name shows wherever it is attached.
QString archiveName()
{
    return QStringLiteral("moonlightweb-logs-%1.zip")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
}

QJsonObject statusJson(const LogArchive::Status& s)
{
    QJsonObject obj;
    obj[QStringLiteral("state")] = s.state;
    obj[QStringLiteral("done_bytes")] = double(s.doneBytes);
    obj[QStringLiteral("total_bytes")] = double(s.totalBytes);
    obj[QStringLiteral("files")] = s.files;
    obj[QStringLiteral("size")] = double(s.size);
    obj[QStringLiteral("file_name")] = s.fileName;
    if (!s.error.isEmpty()) obj[QStringLiteral("error")] = s.error;
    return obj;
}

} // namespace

HttpResponse writeClientLog(const HttpRequest& req, const QString& who, const QString& sender,
                            bool outOfDebug)
{
    const bool debug = mw::run::debug();
    if (!clientLogHeard(debug, outOfDebug)) return HttpResponse::error(404, "Debug mode is off");
    if (req.body.size() > kMaxBody) return HttpResponse::error(413, "Batch too large");
    const QJsonObject body = QJsonDocument::fromJson(req.body).object();
    const QJsonArray lines = body.value(QStringLiteral("lines")).toArray();
    if (lines.size() > kMaxLines) return HttpResponse::error(413, "Too many lines");

    Logger* log = Logger::client();
    // Set once: a file that breaks later is reopened by the logger itself.
    if (log->logFilePath().isEmpty()) {
        // This process's log directory (common/PrivilegedPlace.h); none means
        // the lines are not kept.
        if (const QString dir = mw::place::logDir(); !dir.isEmpty())
            log->setLogFile(dir + QStringLiteral("/moonlightweb-client.log"));
    }
    // In debug mode, every level the page sent; out of it, warnings and errors
    // only, whatever the page sent. Set on every batch, so debug mode switched
    // during the run takes effect at the next one.
    const Logger::Level floor = debug ? Logger::Debug : Logger::Warning;
    log->setMinLevel(floor);

    // Share links and secrets in them are hidden (ClientLogLines.h).
    QList<ClientLogLine> kept;
    for (const ClientLogLine& line : clientLogLines(body, who, req.clientAddress))
        if (line.level >= floor) kept.append(line);
    static ClientLogBudget budget;
    for (const ClientLogLine& line :
         budget.admit(sender, kept, debug ? ClientLogBudget::kDebug : ClientLogBudget::kQuiet,
                      QDateTime::currentMSecsSinceEpoch()))
        log->log(line.level, line.text);

    QJsonObject obj;
    obj[QStringLiteral("status")] = QStringLiteral("ok");
    return HttpResponse::json(obj);
}

void registerLogRoutes(HttpServer& server, LogArchive& archive,
                       std::function<LogScrubber::Names()> names)
{
    RestRouter* router = server.router();

    // Any session: the owner's pages, here or remote. One without a session
    // never gets here (HttpServer's check before any route answers 401). A
    // guest's go through /api/share/player/log, which checks their own cookie
    // instead. Out of debug mode too, for their warnings and errors.
    router->post(QStringLiteral("/api/logs/client"), [](const HttpRequest& req) {
        return writeClientLog(
            req, req.isHostMachine ? QStringLiteral("host") : QStringLiteral("owner"),
            clientLogSender(HttpServer::sessionTokenFromRequest(req), req.clientAddress), true);
    });

    router->get(QStringLiteral("/api/logs/archive"), [&archive](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Admin only");
        return HttpResponse::json(statusJson(archive.status()));
    });

    router->post(QStringLiteral("/api/logs/archive"), [&archive, names](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Admin only");
        if (!archive.start(mw::place::readableLogDirs(), Logger::instance()->logFilePath(),
                           aboutText(), archiveName(),
                           LogScrubber(names ? names() : LogScrubber::Names())))
            return HttpResponse::error(409, "An archive is already being built");
        Logger::info(QStringLiteral("[Logs] Archive requested from %1").arg(req.clientAddress));
        return HttpResponse::json(statusJson(archive.status()));
    });

    router->get(QStringLiteral("/api/logs/archive/download"), [&archive](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Admin only");
        const LogArchive::Status s = archive.status();
        const QByteArray zip = archive.result();
        if (zip.isEmpty()) return HttpResponse::error(404, "No archive ready");
        HttpResponse resp;
        resp.contentType = QStringLiteral("application/zip");
        resp.headers[QStringLiteral("Content-Disposition")] =
            QStringLiteral("attachment; filename=\"%1\"").arg(s.fileName);
        resp.headers[QStringLiteral("Cache-Control")] = QStringLiteral("no-store");
        resp.body = zip;
        return resp;
    });
}
