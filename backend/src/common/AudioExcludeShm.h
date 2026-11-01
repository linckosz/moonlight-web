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

#include <QStringList>
#include <QtGlobal>

/**
 * The apps kept out of the stream's sound, handed from the server to every
 * running stream worker without touching their pipes.
 *
 * A worker may run as the user, elevated, or as SYSTEM on the console desktop
 * (StreamWorkerHost::Launch) — the last one cannot read the user's
 * settings.json. A small named section in the session's Local namespace,
 * created by the server and readable by its workers, reaches all of them alike
 * and lets a box ticked mid-stream apply within half a second, with no new
 * command on any of the worker protocols.
 *
 * Layout: a sequence counter (odd while being written), a byte count, and the
 * names as UTF-8, one per line. Windows only; elsewhere publish() is a no-op
 * and read() reports nothing, since only the Windows capture filters by app.
 */
namespace AudioExcludeShm {

/// Server: (create and) write the list. Keeps the section open for the life
/// of the process.
void publish(const QStringList& exeNames);

/// Worker: the list as last published. False when there is no section to
/// read (no server published one, or not Windows); `*sequence` changes each
/// time the list does.
bool read(QStringList* exeNames, quint32* sequence);

} // namespace AudioExcludeShm
