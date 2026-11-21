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

#include "server/routes/HostBackendWrites.h"
#include "server/routes/HostManualAdd.h"
#include "server/routes/HostWrites.h"
#include "server/AuthManager.h"
#include "server/HostWritePacing.h"
#include "server/ClientUniqueId.h"
#include "server/HttpServer.h"
#include "server/LanHttp.h"
#include "server/RestRouter.h"
#include "backend/ComputerManager.h"
#include "backend/streambackend/NativeHostBackend.h"
#include "backend/streambackend/StreamBackendRegistry.h"

#include <QJsonArray>
#include <QJsonObject>

void registerHostRoutes(HttpServer& server, ComputerManager& computerManager)
{
    server.router()->get("/api/hosts", [&computerManager](const HttpRequest& req) {
        QJsonArray hosts = computerManager.getHostsJson();
        // A TV on plain HTTP reads this machine's own host and no other, not
        // even another's name (issue #42; LanHttp::hostsFor).
        if (req.lanHttp)
            hosts = LanHttp::hostsFor(hosts, NativeHostBackend::hostUuid(),
                                      NativeHostBackend::typeName(), LanHttp::kStreamsThisPc);
        for (qsizetype i = 0; i < hosts.size(); ++i)
            hosts[i] = hostForReader(hosts[i].toObject(), req.isLocal);
        QJsonObject obj;
        obj["hosts"] = hosts;
        return HttpResponse::json(obj);
    });

    // One for the server's life, like the router these routes live on.
    static HostWritePacing pacing;
    // The session behind the cookie; a caller without one (the provisioning
    // path never comes through here) is told apart by its address.
    AuthManager* auth = server.authManager();
    const HostWriteCaller caller = [auth](const HttpRequest& req) {
        const QString id =
            auth ? auth->sessionIdForToken(HttpServer::sessionTokenFromRequest(req)) : QString();
        return id.isEmpty() ? QStringLiteral("peer:") + req.clientAddress : id;
    };

    registerHostWrites(*server.router(), computerManager, pacing, caller);
    registerHostManualAdd(*server.router(), computerManager, pacing, caller);

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

    // Backend management (integration doc §5.3). Deliberately scoped to the
    // host, not to a session: which backend drives a host is an admin fact
    // about that host. A plain Sunshine host never carries one, so its card and
    // its flows stay exactly as they are.
    registerHostBackendWrites(*server.router(), computerManager);

    server.router()->get("/api/hosts/:id/backend", [&computerManager](const HttpRequest& req) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = computerManager.handleGetBackend(uuid);
        if (!req.isLocal) result.remove(QStringLiteral("apiUrl"));
        return HttpResponse::json(result, status);
    });

    // Seats. One set of routes for every backend, because they go through
    // IStreamBackend: a "seat" is whatever the provider says it is. Reading them
    // is any session's; changing them is in registerHostBackendWrites.
    server.router()->getAsync("/api/hosts/:id/seats",
                              [&computerManager](const HttpRequest& req, ResponseCallback respond) {
                                  QString uuid = req.pathParams.value("id");
                                  if (uuid.isEmpty()) {
                                      respond(HttpResponse::error(400, "Missing host ID"));
                                      return;
                                  }
                                  computerManager.handleListSeats(uuid, std::move(respond));
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
    server.router()->getAsync("/api/hosts/:id/apps", [&computerManager](const HttpRequest& req,
                                                                        ResponseCallback respond) {
        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) {
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }
        // Same field the browser already sends on /start, so one identity
        // follows a user through the whole flow.
        computerManager.handleGetAppList(
            uuid, sanitizeClientUniqueId(req.queryParams.value("client_uniqueid")),
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
