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

#include "server/routes/ErrorRoutes.h"
#include "server/ErrorReports.h"
#include "server/HttpServer.h"
#include "server/RestRouter.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// A page keeps 40 lines of at most 1 KiB (ErrorTrail.js), and sends them as JSON.
constexpr qsizetype kMaxBody = 48 * 1024;
constexpr qsizetype kMaxLines = 200;

} // namespace

HttpResponse takeErrorLines(const HttpRequest& req, ErrorReports& reports, const QString& who)
{
    if (req.body.size() > kMaxBody) return HttpResponse::error(413, "Too large");
    const QJsonObject body = QJsonDocument::fromJson(req.body).object();
    if (body.value(QStringLiteral("lines")).toArray().size() > kMaxLines)
        return HttpResponse::error(413, "Too many lines");
    // The header is read for the browser's family and major version only
    // (ErrorReports::browserFamily); it is never kept or sent.
    reports.addPage(who, req.headers.value(QStringLiteral("user-agent")), body);
    QJsonObject obj;
    obj[QStringLiteral("status")] = QStringLiteral("ok");
    return HttpResponse::json(obj);
}

void registerErrorRoutes(HttpServer& server, ErrorReports& reports,
                         std::function<int(const HttpRequest&)> guestSlot)
{
    RestRouter* router = server.router();

    router->post(QStringLiteral("/api/errors/client"), [&reports](const HttpRequest& req) {
        return takeErrorLines(req, reports,
                              req.isHostMachine ? QStringLiteral("host") : QStringLiteral("owner"));
    });

    // Under /api/share/player/ like the guest's other routes: a guest has no
    // session, only the cookie of their slot, checked here.
    router->post(QStringLiteral("/api/share/player/errors"),
                 [&reports, guestSlot](const HttpRequest& req) {
                     if (!guestSlot || guestSlot(req) < 0)
                         return HttpResponse::error(403, "Forbidden");
                     return takeErrorLines(req, reports, QStringLiteral("guest"));
                 });
}
