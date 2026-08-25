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
#include "backend/streambackend/StreamBackendRegistry.h"

#include <QJsonArray>
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

    // Rename a host. POST rather than PATCH because the router speaks
    // get/post/del, and because this changes state either way.
    //
    // A missing 'name' is refused, but an empty one is not: clearing the field is
    // how the user drops the alias and goes back to the host's own name.
    server.router()->post("/api/hosts/:id/name", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");

        QJsonObject body = QJsonDocument::fromJson(req.body).object();
        if (!body.contains("name")) return HttpResponse::error(400, "Missing 'name' field");

        auto [status, result] = computerManager.handleRenameHost(uuid, body["name"].toString());
        return HttpResponse::json(result, status);
    });

    // Restart the streaming service a host runs. Answers 501 wherever
    // MoonlightWeb holds no control path for it, which is what the host list's
    // `restartSupported` flag mirrors so the menu entry never appears in vain.
    server.router()->postAsync(
        "/api/hosts/:id/restart",
        [&computerManager](const HttpRequest& req, ResponseCallback respond) {
            QString uuid = req.pathParams.value("id");
            if (uuid.isEmpty()) {
                respond(HttpResponse::error(400, "Missing host ID"));
                return;
            }
            computerManager.handleRestartHost(uuid, std::move(respond));
        });

    server.router()->post("/api/hosts/:id/wol", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleWakeHost(uuid);
        return HttpResponse::json(result, status);
    });

    // Phase 3: Pairing routes.
    //
    // Starting a pairing is a POST even though it reads like a query, because it
    // changes state on the Sunshine host. A GET is fetched by things that never
    // meant to act: a link preview bot, a browser prefetch, an antivirus that
    // follows URLs — none of which a cross-site check can distinguish from the
    // user, since they arrive as plain navigations. POST is out of their reach.
    server.router()->post("/api/hosts/:id/pair/start", [&computerManager](const HttpRequest& req) {
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

    // Backend management (integration doc §5.3). Deliberately scoped to the
    // host, not to a session: which backend drives a host is an admin fact
    // about that host. A plain Sunshine host never carries one, so its card and
    // its flows stay exactly as they are.
    server.router()->postAsync(
        "/api/hosts/:id/backend",
        [&computerManager](const HttpRequest& req, ResponseCallback respond) {
            QString uuid = req.pathParams.value("id");
            if (uuid.isEmpty()) {
                respond(HttpResponse::error(400, "Missing host ID"));
                return;
            }

            QJsonObject body = QJsonDocument::fromJson(req.body).object();
            const QString type = body["type"].toString();
            const QString apiUrl = body["apiUrl"].toString();
            if (type.isEmpty()) {
                respond(HttpResponse::error(400, "Missing 'type' field"));
                return;
            }
            if (apiUrl.isEmpty()) {
                respond(HttpResponse::error(400, "Missing 'apiUrl' field"));
                return;
            }

            // Absent token = keep the stored one; the browser is never sent it.
            computerManager.handleSetBackend(uuid, type, apiUrl, body["apiToken"].toString(),
                                             body["pairUser"].toString(),
                                             body["pairPassword"].toString(), std::move(respond));
        });

    server.router()->get("/api/hosts/:id/backend", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleGetBackend(uuid);
        return HttpResponse::json(result, status);
    });

    server.router()->del("/api/hosts/:id/backend", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleClearBackend(uuid);
        return HttpResponse::json(result, status);
    });

    // Seat administration. One set of routes for every backend, because they go
    // through IStreamBackend: a "seat" is whatever the provider says it is.
    server.router()->getAsync("/api/hosts/:id/seats",
                              [&computerManager](const HttpRequest& req, ResponseCallback respond) {
                                  QString uuid = req.pathParams.value("id");
                                  if (uuid.isEmpty()) {
                                      respond(HttpResponse::error(400, "Missing host ID"));
                                      return;
                                  }
                                  computerManager.handleListSeats(uuid, std::move(respond));
                              });

    server.router()->postAsync(
        "/api/hosts/:id/seats",
        [&computerManager](const HttpRequest& req, ResponseCallback respond) {
            QString uuid = req.pathParams.value("id");
            if (uuid.isEmpty()) {
                respond(HttpResponse::error(400, "Missing host ID"));
                return;
            }
            // The body is the provider's own provisioning payload, passed
            // through untouched: MultiSeat's SeatRequest grows upstream and
            // mirroring it here would silently drop new fields.
            computerManager.handleProvisionSeat(uuid, QJsonDocument::fromJson(req.body).object(),
                                                std::move(respond));
        });

    server.router()->delAsync(
        "/api/hosts/:id/seats/:seatId",
        [&computerManager](const HttpRequest& req, ResponseCallback respond) {
            QString uuid = req.pathParams.value("id");
            QString seatId = req.pathParams.value("seatId");
            if (uuid.isEmpty() || seatId.isEmpty()) {
                respond(HttpResponse::error(400, "Missing host or seat ID"));
                return;
            }
            computerManager.handleTeardownSeat(uuid, seatId, std::move(respond));
        });

    server.router()->delAsync(
        "/api/hosts/:id/seats/:seatId/owner",
        [&computerManager](const HttpRequest& req, ResponseCallback respond) {
            QString uuid = req.pathParams.value("id");
            QString seatId = req.pathParams.value("seatId");
            if (uuid.isEmpty() || seatId.isEmpty()) {
                respond(HttpResponse::error(400, "Missing host or seat ID"));
                return;
            }
            computerManager.handleReleaseSeatOwner(uuid, seatId, std::move(respond));
        });

    server.router()->getAsync("/api/hosts/:id/profiles",
                              [&computerManager](const HttpRequest& req, ResponseCallback respond) {
                                  QString uuid = req.pathParams.value("id");
                                  if (uuid.isEmpty()) {
                                      respond(HttpResponse::error(400, "Missing host ID"));
                                      return;
                                  }
                                  computerManager.handleListProfiles(uuid, std::move(respond));
                              });

    server.router()->getAsync("/api/hosts/:id/lobbies",
                              [&computerManager](const HttpRequest& req, ResponseCallback respond) {
                                  QString uuid = req.pathParams.value("id");
                                  if (uuid.isEmpty()) {
                                      respond(HttpResponse::error(400, "Missing host ID"));
                                      return;
                                  }
                                  computerManager.handleListLobbies(uuid, std::move(respond));
                              });

    // What the settings dialog offers, so the UI never hardcodes a backend list.
    server.router()->get("/api/backends", [](const HttpRequest&) {
        QJsonArray types;
        for (const QString& t : StreamBackendRegistry::instance().knownTypes()) {
            types.append(t);
        }
        return HttpResponse::json(QJsonObject{{"types", types}});
    });

    // Phase 4: App list (async — fetches from Sunshine via HTTPS)
    server.router()->getAsync("/api/hosts/:id/apps",
                              [&computerManager](const HttpRequest& req, ResponseCallback respond) {
                                  QString uuid = req.pathParams.value("id");
                                  if (uuid.isEmpty()) {
                                      respond(HttpResponse::error(400, "Missing host ID"));
                                      return;
                                  }
                                  // Same field the browser already sends on /start, so one identity
                                  // follows a user through the whole flow.
                                  computerManager.handleGetAppList(
                                      uuid, req.queryParams.value("client_uniqueid"),
                                      std::move(respond));
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

            computerManager.handleGetBoxArt(uuid, appId,
                                            req.headers.value(QStringLiteral("if-none-match")),
                                            std::move(respond));
        });
}
