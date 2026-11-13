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

#include "server/routes/AppLinkRoutes.h"

#include "server/AppLinks.h"
#include "server/RestRouter.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>

#include <memory>
#include <utility>

namespace {

/// The one answer for an alias that leads nowhere this session can see: not
/// one this server holds, or one for a host that is gone. The same words for
/// both, so the answer cannot be used to find out which aliases exist.
HttpResponse noSuchLink()
{
    return HttpResponse::json(QJsonObject{{QStringLiteral("error"), QStringLiteral("not_found")}},
                              404);
}

HttpResponse refused(int status, const QString& error)
{
    return HttpResponse::json(QJsonObject{{QStringLiteral("error"), error}}, status);
}

/// The app id a body gives, or 0 when it gives none that could be one. A JSON
/// number only: "12" as a string is not an id.
int appIdFrom(const QJsonObject& body)
{
    const QJsonValue v = body.value(QStringLiteral("app_id"));
    if (!v.isDouble()) return 0;
    const double d = v.toDouble();
    if (d < 1 || d > 2147483647.0 || d != static_cast<double>(static_cast<qint64>(d))) return 0;
    return static_cast<int>(d);
}

const NvApp* appWithId(const QVector<NvApp>& apps, int id)
{
    for (const NvApp& app : apps)
        if (app.id() == id) return &app;
    return nullptr;
}

/// What the page is told about a link.
QJsonObject describe(const AppLink& link)
{
    QJsonObject obj;
    obj[QStringLiteral("host_uuid")] = link.hostUuid;
    obj[QStringLiteral("app_id")] = link.appId;
    obj[QStringLiteral("name")] = link.name;
    obj[QStringLiteral("state")] = AppLinkStore::stateName(link.state);
    if (link.state == AppLink::State::ToConfirm)
        obj[QStringLiteral("candidate")] =
            QJsonObject{{QStringLiteral("app_id"), link.candidateId},
                        {QStringLiteral("name"), link.candidateName}};
    return obj;
}

} // namespace

void registerAppLinkRoutes(RestRouter& router, AppLinkStore& store, AppLinkDeps deps)
{
    const auto shared = std::make_shared<AppLinkDeps>(std::move(deps));

    // A link to an app the host lists right now. The same app asked twice
    // gets the same alias: one link per app, however many times the icon is
    // pressed.
    router.post(QStringLiteral("/api/app-links"), [&store, shared](const HttpRequest& req) {
        QJsonParseError parseError;
        const QJsonDocument doc = QJsonDocument::fromJson(req.body, &parseError);
        if (parseError.error != QJsonParseError::NoError || !doc.isObject())
            return refused(400, QStringLiteral("invalid_body"));
        const QJsonObject body = doc.object();

        const QString hostUuid = body.value(QStringLiteral("host_uuid")).toString();
        const int appId = appIdFrom(body);
        if (hostUuid.isEmpty() || hostUuid.size() > 128 || appId <= 0)
            return refused(400, QStringLiteral("invalid_body"));

        const std::optional<QVector<NvApp>> apps =
            shared->appsOf ? shared->appsOf(hostUuid) : std::nullopt;
        if (!apps) return noSuchLink();
        // A seat's apps are not the host's: see AppLinkDeps::multiUser.
        if (!shared->multiUser || shared->multiUser(hostUuid))
            return refused(409, QStringLiteral("not_on_multi_seat"));
        const NvApp* app = appWithId(*apps, appId);
        if (!app) return noSuchLink();

        const AppLinkStore::Created made = store.create(hostUuid, *app);
        switch (made.result) {
        case AppLinkStore::CreateResult::Created:
        case AppLinkStore::CreateResult::Existing:
            return HttpResponse::json(QJsonObject{{QStringLiteral("alias"), made.link.alias}});
        case AppLinkStore::CreateResult::Full:
            return refused(409, QStringLiteral("app_links_full"));
        case AppLinkStore::CreateResult::Unwritable: break;
        }
        return refused(503, QStringLiteral("app_links_unwritable"));
    });

    // Where a link points. The same right as the host's list of apps, which
    // already shows this session every name and id this can say.
    router.get(QStringLiteral("/api/app-links/:alias"), [&store, shared](const HttpRequest& req) {
        const std::optional<AppLink> link =
            store.find(req.pathParams.value(QStringLiteral("alias")));
        if (!link) return noSuchLink();
        if (!shared->appsOf || !shared->appsOf(link->hostUuid)) return noSuchLink();
        return HttpResponse::json(describe(*link));
    });

    // The person opening a link "to confirm" said yes: its candidate is the
    // app it meant. The candidate is named in the body, so a yes given to one
    // question is never taken for another the list has asked since.
    router.post(QStringLiteral("/api/app-links/:alias/confirm"),
                [&store, shared](const HttpRequest& req) {
                    const QString alias = req.pathParams.value(QStringLiteral("alias"));
                    const std::optional<AppLink> link = store.find(alias);
                    if (!link) return noSuchLink();
                    const std::optional<QVector<NvApp>> apps =
                        shared->appsOf ? shared->appsOf(link->hostUuid) : std::nullopt;
                    if (!apps) return noSuchLink();

                    const QJsonDocument doc = QJsonDocument::fromJson(req.body);
                    const int candidateId = doc.isObject() ? appIdFrom(doc.object()) : 0;
                    if (candidateId <= 0) return refused(400, QStringLiteral("invalid_body"));

                    switch (store.confirm(alias, candidateId, *apps)) {
                    case AppLinkStore::ConfirmResult::Followed:
                        if (const std::optional<AppLink> now = store.find(alias))
                            return HttpResponse::json(describe(*now));
                        return noSuchLink();
                    case AppLinkStore::ConfirmResult::NotPending:
                        return refused(409, QStringLiteral("not_pending"));
                    case AppLinkStore::ConfirmResult::Unknown: return noSuchLink();
                    case AppLinkStore::ConfirmResult::Unwritable: break;
                    }
                    return refused(503, QStringLiteral("app_links_unwritable"));
                });
}
