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
    /// Taken while full screen. Such a target is sticky: a windowed app taking
    /// focus (Discord, a browser) does not replace it.
    bool fullscreen = false;

    bool operator==(const FocusTarget& o) const
    {
        return pid == o.pid && fullscreen == o.fullscreen;
    }
    bool operator!=(const FocusTarget& o) const { return !(*this == o); }
};

/// What the scanner saw in front this pass.
struct FocusSample
{
    uint32_t pid = 0;        ///< The foreground window's process; 0: no window.
    bool fullscreen = false; ///< Its window covers its whole monitor.
    /// The shell, the lock screen, this host itself: never a target, and no
    /// reason to drop the current one either.
    bool ignored = false;
};

/// The next target, from the current one (`currentAlive`: its process still
/// runs) and what is in front now:
///  - a full-screen app in front always becomes the target;
///  - a windowed app does too, unless the target was taken full screen — so
///    alt-tabbing out of a game to Discord keeps the game's sound;
///  - the shell, the lock screen or nothing in front changes nothing;
///  - a target whose process ended is dropped (silence, never the whole mix)
///    until something takes focus.
inline FocusTarget nextFocusTarget(FocusTarget current, bool currentAlive, const FocusSample& front)
{
    if (current.pid != 0 && !currentAlive) current = FocusTarget{};
    if (front.pid == 0 || front.ignored) return current;
    if (front.pid == current.pid) {
        // The same app, gone full screen: it becomes sticky. Back to a window,
        // it stays sticky — it is still the game it was.
        if (front.fullscreen) current.fullscreen = true;
        return current;
    }
    if (front.fullscreen) return FocusTarget{front.pid, true};
    if (current.pid == 0 || !current.fullscreen) return FocusTarget{front.pid, false};
    return current;
}

} // namespace mw::native::audio
