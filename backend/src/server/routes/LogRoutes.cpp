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
#include "common/RunFlags.h"
#include "server/AppSettings.h"
#include "server/HttpServer.h"
#include "server/LogArchive.h"
#include "server/RestRouter.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QSysInfo>

namespace {

// A page sends at most a few hundred lines every few seconds; these bound what
// a misbehaving one (or anyone with a session) can push through per request.
// The file's own rotation bounds the rest.
constexpr qsizetype kMaxBody = 512 * 1024;
constexpr qsizetype kMaxLines = 2000;
constexpr qsizetype kMaxLineChars = 8 * 1024;

QString logDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
           QStringLiteral("/logs");
}

Logger::Level levelOf(const QString& l)
{
    if (l == QLatin1String("error")) return Logger::Error;
    if (l == QLatin1String("warn")) return Logger::Warning;
    if (l == QLatin1String("debug")) return Logger::Debug;
    return Logger::Info;
}

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

HttpResponse writeClientLog(const HttpRequest& req, const QString& who)
{
    if (!mw::run::debug()) return HttpResponse::error(404, "Debug mode is off");
    if (req.body.size() > kMaxBody) return HttpResponse::error(413, "Batch too large");
    const QJsonObject body = QJsonDocument::fromJson(req.body).object();
    const QJsonArray lines = body.value(QStringLiteral("lines")).toArray();
    if (lines.size() > kMaxLines) return HttpResponse::error(413, "Too many lines");

    Logger* log = Logger::client();
    if (!log->hasLogFile()) {
        QDir().mkpath(logDir());
        // Every level the page sent: the page already chose what to print.
        log->setMinLevel(Logger::Debug);
        log->setLogFile(logDir() + QStringLiteral("/moonlightweb-client.log"));
    }

    // Who, in every line: the page's own id (two tabs are two pages), the kind
    // of user, and where from.
    QString id = body.value(QStringLiteral("id")).toString().left(16);
    id.remove(QRegularExpression(QStringLiteral("[^A-Za-z0-9]")));
    const QString tag = QStringLiteral("[%1 %2@%3]").arg(id, who, req.clientAddress);

    if (body.value(QStringLiteral("seq")).toInt() == 0) {
        log->log(Logger::Info,
                 tag + QStringLiteral(" page %1 — %2")
                           .arg(body.value(QStringLiteral("page")).toString().left(200),
                                body.value(QStringLiteral("ua")).toString().left(300)));
    }
    const int dropped = body.value(QStringLiteral("dropped")).toInt();
    if (dropped > 0)
        log->log(Logger::Warning,
                 tag + QStringLiteral(" %1 earlier lines dropped (the page's buffer was full)")
                           .arg(dropped));

    for (const QJsonValue& v : lines) {
        const QJsonObject line = v.toObject();
        // The page's own clock: the server's stamp says when the batch came in,
        // this one when the line was written.
        const QString when =
            QDateTime::fromMSecsSinceEpoch(qint64(line.value(QStringLiteral("t")).toDouble()))
                .toString(QStringLiteral("hh:mm:ss.zzz"));
        QString text = line.value(QStringLiteral("m")).toString();
        if (text.size() > kMaxLineChars) text = text.left(kMaxLineChars) + QStringLiteral(" […]");
        text.replace(QLatin1Char('\n'), QStringLiteral("\n    "));
        log->log(levelOf(line.value(QStringLiteral("l")).toString()),
                 tag + QLatin1Char(' ') + when + QLatin1Char(' ') + text);
    }

    QJsonObject obj;
    obj[QStringLiteral("status")] = QStringLiteral("ok");
    return HttpResponse::json(obj);
}

void registerLogRoutes(HttpServer& server, LogArchive& archive,
                       std::function<LogScrubber::Names()> names)
{
    RestRouter* router = server.router();

    // Any session: the owner's pages, here or remote. A guest's go through
    // /api/share/player/log, which checks their own cookie instead.
    router->post(QStringLiteral("/api/logs/client"), [](const HttpRequest& req) {
        return writeClientLog(req,
                              req.isHostMachine ? QStringLiteral("host") : QStringLiteral("owner"));
    });

    router->get(QStringLiteral("/api/logs/archive"), [&archive](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Admin only");
        return HttpResponse::json(statusJson(archive.status()));
    });

    router->post(QStringLiteral("/api/logs/archive"), [&archive, names](const HttpRequest& req) {
        if (!req.isLocal) return HttpResponse::error(403, "Admin only");
        if (!archive.start(logDir(), Logger::instance()->logFilePath(), aboutText(), archiveName(),
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
