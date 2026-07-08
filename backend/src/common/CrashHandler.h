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

// Process-wide crash handler.
//
// On Windows it installs an unhandled-exception filter that writes a compact
// minidump (MiniDumpNormal — call stacks + module list, no full heap, so it
// stays small and carries no bulk user data) to <crashDir> when the process
// crashes, then lets the default handler run so the supervisor still restarts
// the app (exit code stays non-zero). Registration is O(1) and free until an
// actual crash, so it is safe to leave enabled in production.
//
// On other platforms this is a no-op (dumps rely on the OS core-dump facility).
namespace CrashHandler {

// Install the handler and remember where dumps go. Also prunes old dumps,
// keeping only the most recent kMaxDumps. Safe to call once, early in main().
void install(const QString& crashDir);

} // namespace CrashHandler
