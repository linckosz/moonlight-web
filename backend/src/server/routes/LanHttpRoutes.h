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
class LanHttpPairing;
class RestRouter;

/// What the TV's routes take from the server around them, as functions: the
/// tests stand in for the server and its clock.
struct LanHttpDeps
{
    /// Seconds since the epoch.
    std::function<qint64()> now;
    /// A failure fed to the connection guard (HttpServer::reportAuthFailure).
    std::function<void(const QString& address)> reportAuthFailure;
    /// The raw HTTPS session cookie of an approver's request
    /// (HttpServer::sessionTokenFromRequest). Never asked on the TV's path.
    std::function<QString(const HttpRequest&)> sessionToken;
    /// This machine's public link while its line is up, empty otherwise: the
    /// approval page the QR code opens is under it, as MW-PAIR-v1's. May be
    /// empty.
    std::function<QString()> rendezvousUrl;
    /// The HTTPS port, for the approval page's address on this LAN when there
    /// is no public link. 0: none.
    std::function<quint16()> httpsPort;
};

/// Register the routes of a TV on plain HTTP (issue #42) and of the
/// administrator who lets it in:
///   GET  /api/lan-http/status          the TV: signed in or not (refreshes
///                                      its cookie)
///   POST /api/lan-http/pair            the TV asks to be let in: id, code,
///                                      claim, and the approval page's address
///   GET  /api/lan-http/pair/:id        the TV asks where its request stands,
///                                      with its claim (X-MW-Claim); answered
///                                      "approved" once, with its cookie
///   GET  /api/lan-http/approve/:id     the approval page: the TV's address
///   POST /api/lan-http/approve/:id     the administrator approves, typing the
///                                      code the TV shows
///   POST /api/lan-http/refuse/:id      the administrator refuses
/// The TV's three answer only on that path (HttpRequest::lanHttp); the
/// administrator's three never on it, and only with the right that mints a PIN
/// (HttpRequest::isLocal). Nothing a TV is told names the administrator.
void registerLanHttpRoutes(RestRouter& router, AuthManager& authManager, LanHttpPairing& book,
                           LanHttpDeps deps);
