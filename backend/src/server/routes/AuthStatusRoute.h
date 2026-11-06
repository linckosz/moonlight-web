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

#include <QString>

#include <functional>

class AuthManager;
class RestRouter;

/// What GET /api/auth/status takes from the server around it, as functions:
/// the tests stand in for the server and its location lookup.
struct AuthStatusDeps
{
    /// The raw session cookie a request carries (HttpServer::sessionTokenFromRequest).
    std::function<QString(const HttpRequest&)> sessionToken;
    /// Where a session is, looked up again and stored when its address has
    /// changed (GeoIpService); may be empty.
    std::function<void(const QString& token, const QString& address)> locate;
    /// What this install calls itself (AppSettings::displayName), said to a
    /// caller on this LAN.
    std::function<QString()> displayName;
};

/// Register GET /api/auth/status: what this browser may do here, asked by the
/// page before and after a PIN. Registered by registerAuthRoutes.
void registerAuthStatusRoute(RestRouter& router, AuthManager& authManager, AuthStatusDeps deps);
