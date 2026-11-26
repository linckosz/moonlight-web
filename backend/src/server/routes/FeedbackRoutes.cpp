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

#include "server/routes/FeedbackRoutes.h"
#include "common/Logger.h"
#include "server/Feedback.h"
#include "server/HttpServer.h"
#include "server/RestRouter.h"

#include <QJsonDocument>
#include <QJsonObject>

namespace {

// The relay takes 8 KiB; the owner's page sends a fraction of that.
constexpr qsizetype kMaxBody = 8 * 1024;

HttpResponse jsonError(int status, const QString& code)
{
    return HttpResponse::json(QJsonObject{{QStringLiteral("error"), code}}, status);
}

} // namespace

HttpResponse feedbackStatus(const HttpRequest& req, const Feedback& feedback)
{
    if (!req.isLocal) return HttpResponse::error(403, "Admin only");
    QJsonObject o;
    o[QStringLiteral("available")] = feedback.available();
    o[QStringLiteral("max_message")] = Feedback::kMaxMessageUnits;
    o[QStringLiteral("attachment")] = feedback.attachment();
    return HttpResponse::json(o);
}

void takeFeedback(const HttpRequest& req, Feedback& feedback, ResponseCallback respond)
{
    if (!req.isLocal) return respond(HttpResponse::error(403, "Admin only"));
    if (req.body.size() > kMaxBody) return respond(jsonError(413, QStringLiteral("too_large")));
    const QJsonDocument doc = QJsonDocument::fromJson(req.body);
    if (!doc.isObject()) return respond(jsonError(400, QStringLiteral("invalid_json")));
    const QJsonObject body = doc.object();

    // The form's hidden field: only something filling every input fills it.
    // Answered like a success, and nothing leaves.
    if (!body.value(QStringLiteral("website")).toString().isEmpty()) {
        Logger::info(QStringLiteral("[Feedback] dropped: hidden field filled"));
        return respond(
            HttpResponse::json(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}}));
    }

    Feedback::Request r;
    r.message = body.value(QStringLiteral("message")).toString();
    r.email = body.value(QStringLiteral("email")).toString();
    r.attachVersion = body.value(QStringLiteral("include_version")).toBool(false);

    feedback.send(r, [respond](Feedback::Outcome o, const QString& detail) {
        switch (o) {
        case Feedback::Outcome::Sent:
            return respond(
                HttpResponse::json(QJsonObject{{QStringLiteral("status"), QStringLiteral("ok")}}));
        case Feedback::Outcome::Invalid: return respond(jsonError(400, detail));
        case Feedback::Outcome::Busy: return respond(jsonError(409, QStringLiteral("busy")));
        case Feedback::Outcome::RateLimited:
            return respond(jsonError(429, QStringLiteral("rate_limited")));
        case Feedback::Outcome::Unreachable:
            return respond(jsonError(502, QStringLiteral("unreachable")));
        case Feedback::Outcome::Unavailable: break;
        }
        respond(jsonError(503, QStringLiteral("unavailable")));
    });
}

void registerFeedbackRoutes(HttpServer& server, Feedback& feedback)
{
    RestRouter* router = server.router();
    router->get(QStringLiteral("/api/feedback"),
                [&feedback](const HttpRequest& req) { return feedbackStatus(req, feedback); });
    router->postAsync(QStringLiteral("/api/feedback"),
                      [&feedback](const HttpRequest& req, ResponseCallback respond) {
                          takeFeedback(req, feedback, std::move(respond));
                      });
}
