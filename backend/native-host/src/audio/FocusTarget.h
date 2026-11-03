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
#include <unordered_map>
#include <vector>

namespace mw::native::audio {

/// The app whose sound the stream carries in "Automatic" mode
/// (AppAudioCapture.h). Pure, so the rule is tested anywhere.
struct FocusTarget
{
    uint32_t pid = 0;       ///< 0: none, the stream is silent.
    uint32_t candidate = 0; ///< Seen in front on the last poll, not yet the target.
};

/// What the scanner saw in front this pass.
struct FocusSample
{
    uint32_t pid = 0; ///< The foreground window's process; 0: no window.
    /// Its window is on the screen being streamed (or that screen is unknown).
    bool onStreamedScreen = true;
    /// The shell (taskbar, Start, desktop, lock screen) or this host itself:
    /// never a target, and no reason to drop the current one either.
    bool ignored = false;
};

/// The next target: the app in front, when it is on the streamed screen.
///  - an app takes over once seen on two polls in a row, so a window flashing
///    through the focus costs no capture client and no gap in the sound;
///  - focus on another screen, on the shell, or on nothing changes nothing and
///    clears the candidate, so Discord on a second monitor or a click on the
///    taskbar never reaches the stream;
///  - a target whose process ended (closed, crashed) is dropped at once:
///    silence, never the whole mix, until an app on the streamed screen takes
///    focus. `currentAlive` is true when that cannot be told.
inline FocusTarget nextFocusTarget(FocusTarget current, bool currentAlive, const FocusSample& front)
{
    if (current.pid != 0 && !currentAlive) current.pid = 0;
    if (front.pid == 0 || front.ignored || !front.onStreamedScreen || front.pid == current.pid) {
        current.candidate = 0;
        return current;
    }
    if (front.pid != current.candidate) {
        current.candidate = front.pid;
        return current;
    }
    current.pid = front.pid;
    current.candidate = 0;
    return current;
}

struct ProcessEntry
{
    uint32_t pid = 0;
    uint32_t parent = 0;
    std::string lowerExe;
};

/// The process to capture for an app the user picked by name: the first of its
/// processes not started by another of them. Capture takes the process tree, so
/// the root carries the helpers (a browser's audio service) that play the sound.
/// 0 when the app is not running.
inline uint32_t pickAppRoot(const std::vector<ProcessEntry>& processes, const std::string& lowerExe)
{
    std::unordered_map<uint32_t, const ProcessEntry*> byPid;
    byPid.reserve(processes.size());
    for (const ProcessEntry& p : processes)
        byPid.emplace(p.pid, &p);
    for (const ProcessEntry& p : processes) {
        if (p.lowerExe != lowerExe) continue;
        const auto parent = byPid.find(p.parent);
        if (parent == byPid.end() || parent->second->lowerExe != lowerExe || p.parent == p.pid)
            return p.pid;
    }
    return 0;
}

} // namespace mw::native::audio
