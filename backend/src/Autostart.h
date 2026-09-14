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
 * One per-user login item per platform, and the same one whether it was
 * created by an installer or toggled from inside the app:
 *
 *  - Windows: the logon Scheduled Task named after the edition (`MoonlightWeb`
 *    / `MoonlightWebDev`), the very task the Inno Setup installer registers
 *    when its "Start at logon" box is ticked. Driven here through the Task
 *    Scheduler COM API, so no `schtasks` process and no elevation: a task a
 *    user created (or an installer created for that user) is that user's to
 *    change.
 *  - macOS: a per-user LaunchAgent (~/Library/LaunchAgents/com.moonlightweb.agent.plist,
 *    relaunch on crash only), the one the .pkg postinstall writes as well.
 *  - Linux: an XDG autostart entry (~/.config/autostart/moonlightweb.desktop).
 *
 * This is distinct from the service supervisors (root LaunchDaemon, systemd
 * unit, NSSM service) used for a headless server: those set MW_SERVICE=1 and
 * show no tray. Under one of them the login item is meaningless (the server
 * is already up before anyone logs in), so isSupported() answers false and
 * nothing offers the option — that is also what keeps a service install
 * from ever mixing the two mechanisms.
 */
namespace Autostart {

/// Whether a login item makes sense here at all: a desktop session (not a
/// service supervisor) on a platform with a mechanism behind it. When false,
/// no menu entry and no checkbox is shown, and the other calls are no-ops.
bool isSupported();

/// Install the per-user login item. Returns true on success. Writing it is
/// enough for the next login; the already-running instance is left untouched
/// (no immediate second launch).
bool installLoginItem();

/// Remove the per-user login item. Returns true on success, and also when
/// there was nothing to remove.
bool removeLoginItem();

/// Whether the per-user login item is present. Probed from the OS every time,
/// never cached: the user can remove the task or the file by hand, and a box
/// that stays ticked afterwards would be lying.
bool isLoginItemInstalled();

} // namespace Autostart
