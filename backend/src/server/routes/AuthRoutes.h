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

class HttpServer;
class AuthManager;
class GeoIpService;
class AppSettings;

/// Register auth / admin-PIN / certificate-token / pairing-key routes on the
/// server's router. @p settings is needed for the MW-BIND-v1 host identity key
/// handed to a browser when it pairs.
void registerAuthRoutes(HttpServer& server, AuthManager& authManager, GeoIpService& geoIpService,
                        AppSettings& settings);
