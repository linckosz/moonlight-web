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

#include <optional>

/**
 * @brief The `--vdisplay-apply` mode: the part of adding a virtual display
 * that needs rights the server does not have.
 *
 * Started by the elevated scheduled task (stage "all"), by the SYSTEM service
 * as one child per stage (the desktop halves in the console session, the
 * node half as SYSTEM), or by a hand-elevated instance. It reads
 * request.json from the staging directory, does exactly what that says,
 * writes result.json and prints the same JSON line on stdout. The installer
 * and its uninstaller name their verb instead (`--vdisplay-verb`): no file is
 * read or written then, the answer is the exit code and stdout. Never a
 * window, never a message box: the task runs hidden on a desktop that may
 * have no monitor at all.
 *
 * Stage "snapshot" (any desktop process, Activate only, first): save the
 *   active display layout — paths and modes — to topology.bin.
 * Stage "driver" (elevated):
 *   install    — re-hash the three driver files against the constants
 *                compiled in here, write the driver's settings XML if there
 *                is none, trust the catalog's signer for the duration, create
 *                the Root\MttVDD node named "MoonlightWeb Virtual Display"
 *                and install the package on it (SetupAPI + newdev), untrust
 *                again, leave the node disabled.
 *   uninstall  — remove our node; delete the package from the driver store
 *                only if no other node uses it.
 *   activate   — enable our node.
 *   deactivate — disable it.
 * Stage "mode" (any desktop process):
 *   activate   — wait for the virtual display to appear, switch off what
 *                Windows switched on with it, set 1080p at 120 Hz SDR, make
 *                it primary.
 *   deactivate — before the node goes: the saved layout back (or, without
 *                one, the previous primary back in its role).
 */
namespace VirtualDisplayApply {

/// @param stage "all" | "snapshot" | "driver" | "mode"
/// @param dir   staging directory; empty = VirtualDisplay::stagingDir()
/// @param verb  the installer's verb, `install` or `uninstall`: the request is
///              built from it (VirtualDisplay::requestForVerb), with no file
///              read or written and no directory taken; any other word, an
///              empty one included, is refused. None = the request file of
///              the staging directory.
/// @return process exit code: 0 ok, 1 failed, 3010 ok but reboot required
int run(const QString& stage, const QString& dir,
        const std::optional<QString>& verb = std::nullopt);

/// The `--vdisplay-guard` mode (Windows): left behind by a server whose
/// stream turned the other displays off (Request::exclusive), it waits on
/// that server's process. When the server dies with them still off, it plays
/// the desktop half of a deactivation — the layout from before, or Windows'
/// extended one — without elevation; the node goes off at the server's next
/// start (resetAtStartup). It leaves as soon as they are back on, by the
/// stream's end or anything else (VirtualDisplay::guardStep).
/// @param server "<pid>:<creation time>" of the server process, the time as
///               the 100 ns ticks GetProcessTimes gives: a pid reused by
///               another process is not mistaken for the server.
/// @return process exit code: 0 nothing to do or restored, 1 failed
int guard(const QString& server);

} // namespace VirtualDisplayApply
