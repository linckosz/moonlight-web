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

/**
 * @brief Registers the app to start at user login (GUI session).
 *
 * Windows uses a logon Scheduled Task created by the Inno Setup installer. macOS
 * and Linux have no wizard-driven installer, so the in-app setup wizard installs
 * the platform login item itself: a per-user LaunchAgent on macOS
 * (~/Library/LaunchAgents/com.moonlightweb.agent.plist, relaunch on crash only)
 * and an XDG autostart entry on Linux (~/.config/autostart/moonlightweb.desktop).
 *
 * This is distinct from the root LaunchDaemon (packaging/launchd) used for a
 * headless server binding :80/:443, which sets MW_SERVICE=1 and shows no tray.
 */
namespace Autostart {

/// Install the per-user login item. Returns true on success. Writing the file is
/// enough for the next login; the already-running instance is left untouched (no
/// immediate second launch). macOS + Linux — no-op on Windows.
bool installLoginItem();

/// Whether the per-user login item is already present.
bool isLoginItemInstalled();

} // namespace Autostart
