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

#include <QString>

namespace FirstRunLink {

/// Where a first launch on macOS or Linux opens the setup wizard: this
/// machine's plain HTTP port on loopback, so that the first page anyone sees
/// (an app catalogue's screenshot included) is the wizard, not the browser's
/// warning about the self-signed certificate. The wizard ends on HTTPS, where
/// that certificate is accepted once (SetupView.js).
///
/// No host key (#k=) rides on it: none is made for this link. On loopback the
/// wizard needs none (the local privilege, RequestGuard::loopbackOnly), and the
/// plain-HTTP origin is to keep nothing: whoever holds the port later holds it.
///
/// Empty when @p activeHttpPort is 0 (no plain listener up): the caller then
/// opens the HTTPS link it always did.
inline QString wizardUrl(quint16 activeHttpPort)
{
    if (activeHttpPort == 0) return {};
    if (activeHttpPort == 80) return QStringLiteral("http://localhost/setup");
    return QStringLiteral("http://localhost:%1/setup").arg(activeHttpPort);
}

} // namespace FirstRunLink
