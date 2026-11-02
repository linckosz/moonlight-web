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

namespace mw::native::audio {

/// The app whose sound the stream carries in "Focused app" mode
/// (AppAudioCapture.h). Pure, so the rule is tested anywhere.
struct FocusTarget
{
    uint32_t pid = 0; ///< 0: none — the stream is silent.

    bool operator==(const FocusTarget& o) const { return pid == o.pid; }
    bool operator!=(const FocusTarget& o) const { return !(*this == o); }
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
///  - focus moving to an app on the streamed screen switches to it — the
///    viewer hears what they see;
///  - focus on another screen, on the shell, or on nothing changes nothing, so
///    Discord on a second monitor or a click on the taskbar never reaches the
///    stream;
///  - a target whose process ended (closed, crashed) is dropped: silence,
///    never the whole mix, until an app on the streamed screen takes focus.
inline FocusTarget nextFocusTarget(FocusTarget current, bool currentAlive, const FocusSample& front)
{
    if (current.pid != 0 && !currentAlive) current = FocusTarget{};
    if (front.pid == 0 || front.ignored || !front.onStreamedScreen) return current;
    return FocusTarget{front.pid};
}

} // namespace mw::native::audio
