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
 * The environment an elevated helper inherits, and what it must not believe.
 *
 * The installer's elevated tasks (update, virtual display, virtual HID, the
 * stream worker) run our exe with the user's admin token but the user's OWN
 * environment: HKCU\Environment, which any unelevated process of that user can
 * write. A variable that tells Qt or OpenSSL where to load code from would then
 * load the user's DLL into an elevated process — a UAC bypass for whoever can
 * set a variable and start the task, which is every program the user runs.
 *
 * Only what loads code (or decides which certificates TLS trusts) goes. The
 * rest — the session, the desktop, the profile paths, the debug switches the
 * capture and the audio read — is left exactly as it was.
 */
namespace mw::inheritedenv {

/// Whether this variable may steer an elevated process into loading code or
/// trusting certificates (Qt plugin and QML paths, OpenSSL's configuration,
/// modules and CA files, the Vulkan loader's driver and layer lists).
/// Case-insensitive, like Windows environment names.
bool isCodeLoading(const QString& name);

/// Whether this command line starts one of the modes that run elevated:
/// --update-apply, --vdisplay-apply, --vhid-apply, --stream-worker.
bool isElevatedHelper(int argc, char** argv);

/// Whether this process runs elevated, asked of its own token. Windows only:
/// false elsewhere, and false when the token cannot be read.
bool startedElevated();

/// Whether this start gets scrub(): one of the elevated modes, by its name, or
/// any start that runs elevated, whatever it was asked to do. Both, because
/// each covers what the other may miss: a mode not in the list yet, and a token
/// that could not be read.
inline bool mustScrub(bool elevatedMode, bool elevatedToken)
{
    return elevatedMode || elevatedToken;
}

/// Drop every isCodeLoading() variable from this process and take the current
/// directory out of the DLL search order. Windows only (a no-op elsewhere:
/// pkexec and launchd hand their helpers a clean environment). Call first in
/// main(), before QApplication reads any of them.
void scrub();

} // namespace mw::inheritedenv
