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
#include "server/HostWritePacing.h"
#include "server/RestRouter.h"

#include <QJsonObject>

#include <functional>
#include <utility>

/// Who is asking, as the pairing of a host remembers it: a session id, never
/// its token.
using HostWriteCaller = std::function<QString(const HttpRequest&)>;

namespace HostWriteAnswers {

/// The page shows these in the viewer's language from `code`; `message` is
/// for everything else that reads the API.
inline HttpResponse paced(const char* code, const char* message, qint64 waitMs)
{
    HttpResponse r = HttpResponse::json(
        {{"status", "error"}, {"code", QLatin1String(code)}, {"message", QLatin1String(message)}},
        429);
    r.headers.insert(QStringLiteral("Retry-After"),
                     QString::number(qMax<qint64>(1, (waitMs + 999) / 1000)));
    return r;
}

inline HttpResponse tooManyAttempts(qint64 waitMs)
{
    return paced("too_many_attempts", "Too many attempts. Try again in a moment.", waitMs);
}

inline HttpResponse restartTooSoon(qint64 waitMs)
{
    return paced("restart_too_soon", "This host was restarted less than a minute ago.", waitMs);
}

/// One answer for every way adding a host can fail, to anyone but the
/// administrator: what a port says back is not handed out.
inline HttpResponse hostAddFailed()
{
    return HttpResponse::json({{"status", "error"},
                               {"code", "host_add_failed"},
                               {"message", "No host could be added at this address."}},
                              400);
}

} // namespace HostWriteAnswers

/// The writes on a host that every paired device may make, paced by
/// HostWritePacing, and its pairing, bound to the session that started it.
/// A template so the tests can drive these very routes.
template <typename Manager>
void registerHostWrites(RestRouter& router, Manager& manager, HostWritePacing& pacing,
                        HostWriteCaller caller)
{
    // A paced scan still answers as one that was made: the page has nothing to
    // tell apart.
    router.post("/api/hosts/scan", [&manager, &pacing](const HttpRequest& req) {
        if (pacing.scanAllowed(req.isLocal)) manager.handleScanRequest(req.isLocal);
        return HttpResponse::json({{"status", "scanning"}});
    });

    // Restart the streaming service a host runs. Answers 501 wherever
    // MoonlightWeb holds no control path for it, which is what the host list's
    // `restartSupported` flag mirrors so the menu entry never appears in vain.
    router.postAsync("/api/hosts/:id/restart", [&manager, &pacing](const HttpRequest& req,
                                                                   ResponseCallback respond) {
        const QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) {
            respond(HttpResponse::error(400, "Missing host ID"));
            return;
        }
        const qint64 wait = pacing.restartWaitMs(uuid, req.isLocal);
        if (wait > 0) {
            respond(HostWriteAnswers::restartTooSoon(wait));
            return;
        }
        // Opened before anything stops, so a second request while the first one
        // runs is already held back.
        const auto previous = pacing.restartBegin(uuid);
        manager.handleRestartHost(
            uuid, [&pacing, uuid, previous, respond = std::move(respond)](HttpResponse r) {
                // Turned down before anything was done: no window.
                if (r.statusCode == 404 || r.statusCode == 501) pacing.restartUndo(uuid, previous);
                respond(std::move(r));
            });
    });

    // A second press within kWakeMs gets the first one's answer, and no packet.
    router.post("/api/hosts/:id/wol", [&manager, &pacing](const HttpRequest& req) {
        const QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");
        if (const auto again = pacing.wakeReplay(uuid)) return HttpResponse::json(*again);

        auto [status, result] = manager.handleWakeHost(uuid);
        if (status == 200) pacing.wakeSent(uuid, result);
        return HttpResponse::json(result, status);
    });

    // Starting a pairing is a POST even though it reads like a query, because it
    // changes state on the Sunshine host. A GET is fetched by things that never
    // meant to act: a link preview bot, a browser prefetch, an antivirus that
    // follows URLs — none of which a cross-site check can distinguish from the
    // user, since they arrive as plain navigations. POST is out of their reach.
    router.post("/api/hosts/:id/pair/start", [&manager, caller](const HttpRequest& req) {
        const QString uuid = req.pathParams.value("id");
        if (uuid.isEmpty()) return HttpResponse::error(400, "Missing host ID");
        auto [status, result] = manager.handleStartPairing(uuid, caller(req), req.isLocal);
        return HttpResponse::json(result, status);
    });

    router.postAsync("/api/hosts/:id/pair",
                     [&manager, caller](const HttpRequest& req, ResponseCallback respond) {
                         const QString uuid = req.pathParams.value("id");
                         if (uuid.isEmpty()) {
                             respond(HttpResponse::error(400, "Missing host ID"));
                             return;
                         }
                         manager.handleSubmitPin(uuid, caller(req), std::move(respond));
                     });
}
