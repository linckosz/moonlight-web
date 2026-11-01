/*
 * MoonlightWeb — native capture & encoding engine.
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

#include <cstdint>
#include <string>
#include <vector>

namespace mw::native {

/// The applications whose sound is kept OUT of the stream, by executable file
/// name ("chrome.exe", "discord.exe"), matched case-insensitively.
///
/// Process-wide and thread-safe: the stream worker sets it (at start, then
/// whenever the owner ticks a box), and the audio capture thread reads it.
/// An empty list means "everything the host plays", which is exactly the
/// whole-mix loopback the engine has always used.
///
/// Only the Windows capture honours it today (per-process loopback); the other
/// platforms keep streaming the whole mix.
class AudioAppFilter
{
public:
    /// Replace the list. Names are trimmed, lower-cased and de-duplicated; a
    /// path is reduced to its file name. Bumps generation() only when the
    /// resulting set actually differs.
    static void set(const std::vector<std::string>& exeNames);

    /// The current list, normalised as set() stored it.
    static std::vector<std::string> get();

    /// Changes each time the list changes; 0 before the first set(). Cheap
    /// enough to poll from an audio thread.
    static uint64_t generation();

    /// The normalised form of one name: file name only, lower case, trimmed.
    /// Public so the capture and the tests use the very same rule.
    static std::string normalise(const std::string& exeName);
};

} // namespace mw::native
