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

#include <functional>

class HttpServer;
class AppSettings;
class AuthManager;
class InternetAccessManager;
class ComputerManager;

/// Register Internet-access, first-run setup, admin-settings and streaming-settings routes.
/// @param onHostKeyRotated Invoked after a host-key redemption rotated the key
///        (single-use), so the entry points embedding it (Desktop shortcut)
///        are rewritten with the fresh key.
void registerSystemRoutes(HttpServer& server, AppSettings& appSettings, AuthManager& authManager,
                          InternetAccessManager& internetAccess, ComputerManager& computerManager,
                          std::function<void()> onHostKeyRotated = {});
