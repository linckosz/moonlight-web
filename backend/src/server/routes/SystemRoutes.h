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

#include <QJsonObject>

#include <functional>

class HttpServer;
class AppSettings;
class AuthManager;
class InternetAccessManager;
class ComputerManager;
struct HttpRequest;

/// Register Internet-access, first-run setup, admin-settings and streaming-settings routes.
/// @param spendHostKey Spends the key a link of this machine carried, for
///        POST /api/auth/host-key (HostKeyVault::spend): true once, for a key
///        made here and still waiting.
/// @param onInternetAccessToggled Invoked with the new state whenever the
///        Internet-access switch is thrown here. The rendezvous line follows the
///        same switch — it is an outbound connection that announces this machine
///        is online, so it belongs on the consent side of the boundary, not
///        outside it. A callback rather than a direct dependency keeps these
///        routes ignorant of the rendezvous client entirely.
/// @param rendezvousStatus Supplies {url, online} for /api/internet/status. Also
///        a callback for the reason above — and because the address an instance
///        answers at is now the ONLY way its owner can learn it: nothing is
///        published in DNS any more, so an unsurfaced URL means a reachable
///        machine nobody can reach.
/// @param viewerOnLan Whether the browser asking reaches this machine over its
///        own network, as a stream would plan it (main.cpp streamClientKind):
///        GET /api/settings/streaming says so as `viewer_lan`, and the page's
///        automatic bitrate takes the LAN rule (frontend/js/util/AutoBitrate.js).
void registerSystemRoutes(HttpServer& server, AppSettings& appSettings, AuthManager& authManager,
                          InternetAccessManager& internetAccess, ComputerManager& computerManager,
                          std::function<bool(const QString& key)> spendHostKey = {},
                          std::function<void(bool)> onInternetAccessToggled = {},
                          std::function<QJsonObject()> rendezvousStatus = {},
                          std::function<bool(const HttpRequest&)> viewerOnLan = {});
