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

#include <QString>

class AppSettings;
class ComputerManager;

/**
 * @brief First-run provisioning consumed from a file written by the installer.
 *
 * The Windows installer collects user choices (authorize Internet Access, pair
 * the local Sunshine with given credentials) but cannot run the crypto-heavy
 * pairing / Internet-Access logic itself. Instead it drops a `provisioning.json`
 * next to the executable; on first startup the server reads it, applies the
 * actions once, then renames the file to `provisioning.consumed.json` (stripping
 * the password) so it is never replayed.
 *
 * Expected JSON shape (all keys optional):
 * {
 *   "internet_access_authorized": true,
 *   "sunshine": { "auto_pair": true, "username": "admin", "password": "admin" }
 * }
 */
namespace Provisioning {

/// Apply provisioning from `<exeDir>/provisioning.json` if present. No-op when
/// the file is absent. Sets `internet_access_enabled` in settings (the caller's
/// existing auto-start path then brings Internet Access up) and pairs the local
/// Sunshine via the GameStream protocol + Sunshine REST `/api/pin`.
void applyOnce(const QString& exeDir, AppSettings& settings, ComputerManager& computers);

} // namespace Provisioning
