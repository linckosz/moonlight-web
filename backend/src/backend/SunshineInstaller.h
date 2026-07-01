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

/**
 * @brief Detects and (on macOS) installs the Sunshine streaming server.
 *
 * Windows uses the Inno Setup installer to fetch and silently install Sunshine,
 * so on Windows this only detects it. macOS has no native installer (the app is
 * shipped as a bare .dmg), so the in-app setup wizard drives the install here:
 * download the official Sunshine-macOS-<arch>.dmg, copy Sunshine.app into
 * /Applications, then set its credentials via the `--creds` CLI. TCC permissions
 * (Screen Recording / Accessibility) still require a manual user grant at
 * Sunshine's first launch — no API can bypass that.
 */
namespace SunshineInstaller {

struct DetectResult
{
    bool installed = false;
    QString exePath; ///< Full path to the sunshine binary when installed.
    QString version; ///< Best-effort version string ("" if unknown).
};

/// Locate an existing Sunshine install (platform-specific well-known paths + PATH).
DetectResult detect();

/// macOS only: download + install the official Sunshine DMG, then apply the given
/// credentials. Returns an empty string on success, or a human-readable error.
/// No-op returning an error on other platforms.
QString installMacOS(const QString& user, const QString& pass);

/// Set Sunshine's web-UI credentials via `sunshine --creds <user> <pass>`.
/// Best-effort: returns true when the CLI exited 0.
bool setCredentials(const QString& user, const QString& pass);

/// Start Sunshine so it begins serving (and, on macOS, prompts the user for the
/// Screen Recording / Accessibility permissions it needs). Best-effort.
bool launch();

} // namespace SunshineInstaller
