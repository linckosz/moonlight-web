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

#include "backend/NvApp.h"

#include <QString>
#include <QVector>

#include <functional>
#include <optional>

class AppLinkStore;
class RestRouter;

/// What the link routes take from the server around them.
struct AppLinkDeps
{
    /// The apps of the host @p hostUuid as this server last listed them, or
    /// nothing when this server has no such host.
    std::function<std::optional<QVector<NvApp>>(const QString& hostUuid)> appsOf;

    /// Whether the host @p hostUuid is driven by a multi-user backend, where
    /// each seat has its own apps. No link is made there: the list this server
    /// kept last is the seat last refreshed, chosen by an id the client gives
    /// itself, so it may be another user's. Unset counts as yes.
    std::function<bool(const QString& hostUuid)> multiUser;
};

/// The routes of the links straight to an app (issue #63, server/AppLinks.h).
///
///   POST /api/app-links                  {host_uuid, app_id} → {alias}
///   GET  /api/app-links/:alias           → where it points, and in what state
///   POST /api/app-links/:alias/confirm   {app_id} → the link follows its candidate
///
/// Behind the session gate like every API route, so a guest, who has no
/// session here, never reaches them; nor does the local channel or a TV on
/// plain HTTP, whose lists of routes do not name them. What they answer is no
/// more than the host's list of apps already tells the same session. An alias
/// this server does not know and one it knows for a host that is gone get the
/// same 404, so asking says nothing about which aliases exist.
void registerAppLinkRoutes(RestRouter& router, AppLinkStore& store, AppLinkDeps deps);
