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

#pragma once

#include "common/Types.h"
#include "server/RestRouter.h"

#include <QJsonDocument>
#include <QJsonObject>

#include <utility>

/// Setting or clearing a host's backend and changing its seats, the
/// administrator's alone (HttpRequest::isLocal). A template so the tests can
/// drive these very routes without a ComputerManager.
template <typename Manager> void registerHostBackendWrites(RestRouter& router, Manager& manager)
{
    static const auto refused = []() {
        return HttpResponse::error(
            403, "A host's backend and seats can only be changed by its administrator");
    };

    router.postAsync(
        "/api/hosts/:id/backend", [&manager](const HttpRequest& req, ResponseCallback respond) {
            if (!req.isLocal) {
                respond(refused());
                return;
            }

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
            manager.handleSetBackend(uuid, type, apiUrl, body["apiToken"].toString(),
                                     body["pairUser"].toString(), body["pairPassword"].toString(),
                                     std::move(respond));
        });

    router.del("/api/hosts/:id/backend", [&manager](const HttpRequest& req) {
        if (!req.isLocal) return refused();

        QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");

        auto [status, result] = manager.handleClearBackend(uuid);
        return HttpResponse::json(result, status);
    });

    router.postAsync("/api/hosts/:id/seats",
                     [&manager](const HttpRequest& req, ResponseCallback respond) {
                         if (!req.isLocal) {
                             respond(refused());
                             return;
                         }
                         QString uuid = req.pathParams.value("id");
                         if (uuid.isEmpty()) {
                             respond(HttpResponse::error(400, "Missing host ID"));
                             return;
                         }
                         // The body is the provider's own provisioning payload, passed
                         // through untouched: MultiSeat's SeatRequest grows upstream and
                         // mirroring it here would silently drop new fields.
                         manager.handleProvisionSeat(
                             uuid, QJsonDocument::fromJson(req.body).object(), std::move(respond));
                     });

    router.delAsync("/api/hosts/:id/seats/:seatId",
                    [&manager](const HttpRequest& req, ResponseCallback respond) {
                        if (!req.isLocal) {
                            respond(refused());
                            return;
                        }
                        QString uuid = req.pathParams.value("id");
                        QString seatId = req.pathParams.value("seatId");
                        if (uuid.isEmpty() || seatId.isEmpty()) {
                            respond(HttpResponse::error(400, "Missing host or seat ID"));
                            return;
                        }
                        manager.handleTeardownSeat(uuid, seatId, std::move(respond));
                    });

    router.delAsync("/api/hosts/:id/seats/:seatId/owner",
                    [&manager](const HttpRequest& req, ResponseCallback respond) {
                        if (!req.isLocal) {
                            respond(refused());
                            return;
                        }
                        QString uuid = req.pathParams.value("id");
                        QString seatId = req.pathParams.value("seatId");
                        if (uuid.isEmpty() || seatId.isEmpty()) {
                            respond(HttpResponse::error(400, "Missing host or seat ID"));
                            return;
                        }
                        manager.handleReleaseSeatOwner(uuid, seatId, std::move(respond));
                    });
}
