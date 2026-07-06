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

#include "server/routes/HostRoutes.h"

#include "server/HttpServer.h"
#include "server/RestRouter.h"
#include "backend/ComputerManager.h"

#include <QJsonObject>

void registerHostRoutes(HttpServer& server, ComputerManager& computerManager)
{
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
        if (address.isEmpty()) return HttpResponse::error(400, "Missing 'address' field");

        auto [status, result] = computerManager.handleAddManualHost(address);
        return HttpResponse::json(result, status);
    });

    server.router()->del("/api/hosts/:id", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleDeleteHost(uuid);
        return HttpResponse::json(result, status);
    });

    server.router()->post("/api/hosts/:id/wol", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleWakeHost(uuid);
        return HttpResponse::json(result, status);
    });

    // Phase 3: Pairing routes
    server.router()->get("/api/hosts/:id/pair", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");
        auto [status, result] = computerManager.handleStartPairing(uuid);
        return HttpResponse::json(result, status);
    });

    server.router()->postAsync("/api/hosts/:id/pair", [&computerManager](const HttpRequest& req,
                                                                         ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) {
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }
        computerManager.handleSubmitPin(uuid, std::move(respond));
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
    server.router()->getAsync(
        "/api/hosts/:id/appasset",
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
}
