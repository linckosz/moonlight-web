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

// Who signed a PE file, and whether its Authenticode verifies: the two reads
// the elevated update helper makes (UpdateApply, step 4). Windows only.
namespace Authenticode {

/// The subject of the certificate that signed `path`, from its embedded
/// signature; empty when it has none (or cannot be read).
QString signerSubject(const QString& path);

/// WinVerifyTrust on `path`, machine stores only: 0 (ERROR_SUCCESS) when the
/// signature verifies, the trust error otherwise.
long verify(const QString& path);

} // namespace Authenticode
