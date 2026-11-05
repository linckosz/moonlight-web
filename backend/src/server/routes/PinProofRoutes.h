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

#include <QByteArray>

class RestRouter;
class AuthManager;
class GeoIpService;

/// MW-TLS-v1 and MW-PIN-v1 (docs/design/tls-key-proof.md) on @p router:
/// GET /api/auth/tls-proof, POST /api/auth/pin/start and /api/auth/pin/finish.
///
/// @p hostKeyPem is the host key as it stands when the routes are registered:
/// they sign with it and pair under it, and read no file of their own on a call.
/// A file of its own, so that the tests drive these very routes on a router of
/// their own.
void registerPinProofRoutes(RestRouter& router, AuthManager& authManager,
                            GeoIpService& geoIpService, const QByteArray& hostKeyPem);
