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

#include <QJsonObject>
#include <QStringList>

/**
 * The apps playing sound on this PC, for the settings page that keeps some of
 * them out of the stream (AudioAppFilter, per-app capture).
 *
 * One entry per executable, merged across its processes and every playback
 * device: what the Windows volume mixer shows, with the same icons. Apps
 * already excluded are listed even when they are not running, so a choice can
 * always be undone.
 */
namespace AudioSources {

/// {
///   "supported": bool,         // per-app capture exists here (Windows 11)
///   "reason": string,          // why not, when it does not
///   "sources": [ { "exe": "chrome.exe", "name": "Google Chrome",
///                  "icon": "data:image/png;base64,…" | "",
///                  "running": bool, "playing": bool, "excluded": bool } ]
/// }
/// Takes a few tens of milliseconds (COM, one icon per new executable, cached).
QJsonObject list(const QStringList& excluded);

/// Whether this Windows has per-process loopback (build 20348+).
bool perAppCaptureSupported();

} // namespace AudioSources
