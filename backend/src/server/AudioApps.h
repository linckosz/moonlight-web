/*
 * MoonlightWeb
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

#include <QJsonArray>

/**
 * The apps with sound on this PC, for Settings → "Stream audio from", where one
 * can be picked: what the Windows volume mixer lists, minus Windows' own
 * surfaces and this server.
 */
namespace AudioApps {

/// [ { "exe": "chrome.exe", "name": "Google Chrome" } ], sorted by name. One
/// entry per executable, merged across its processes and the playback devices.
/// Empty away from Windows. Takes a few milliseconds.
QJsonArray list();

} // namespace AudioApps
