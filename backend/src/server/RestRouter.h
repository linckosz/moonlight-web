/*
 * Moonlight-Web — browser-based Sunshine/GameStream client.
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

#include <QObject>
#include <QMap>
#include <QStringList>
#include <functional>
#include "common/Types.h"

using SyncRouteHandler = std::function<HttpResponse(const HttpRequest&)>;
using AsyncRouteHandler = std::function<void(const HttpRequest&, ResponseCallback)>;

class RestRouter : public QObject
{
    Q_OBJECT

public:
    explicit RestRouter(QObject* parent = nullptr);

    // Sync handlers — wrapped internally to call respond() synchronously
    void get(const QString& path, SyncRouteHandler handler);
    void post(const QString& path, SyncRouteHandler handler);
    void del(const QString& path, SyncRouteHandler handler);

    // Async handlers — for routes that need deferred responses
    void getAsync(const QString& path, AsyncRouteHandler handler);
    void postAsync(const QString& path, AsyncRouteHandler handler);

    // Dispatch — calls respond() synchronously for sync routes, asynchronously otherwise
    void dispatchAsync(const HttpRequest& request, ResponseCallback respond) const;
    bool hasRoute(const QString& method, const QString& path) const;

private:
    struct ParamRoute {
        QString method;
        QStringList segments;
        AsyncRouteHandler handler;
    };

    QString routeKey(const QString& method, const QString& path) const;
    bool matchParamRoute(const QStringList& pathSegments, const ParamRoute& route,
                         QMap<QString, QString>& outParams) const;

    QMap<QString, AsyncRouteHandler> m_Routes;
    QList<ParamRoute> m_ParamRoutes;
};
