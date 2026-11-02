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

#include <atomic>
#include <mutex>
#include <string>

namespace mw::native {

/// Which sound the stream carries (Settings → "Stream audio from").
///
///  - AllApps: the whole mix the host plays, as the engine always did. Default.
///  - FocusedApp: only the app in front on the streamed screen
///    (AppAudioCapture.h) — the game being played, so that Discord or a
///    browser stay on the host's speakers and out of the stream.
///
/// Set once by the stream worker before the session starts, read by the audio
/// capture when it opens. Only the Windows capture honours FocusedApp today
/// (per-process loopback, Windows 11); the other platforms stream the whole mix.
enum class AudioSource : int
{
    AllApps = 0,
    FocusedApp = 1,
};

namespace detail {
inline std::atomic<int>& audioSourceSlot()
{
    static std::atomic<int> slot{static_cast<int>(AudioSource::AllApps)};
    return slot;
}
} // namespace detail

inline void setAudioSource(AudioSource source)
{
    detail::audioSourceSlot().store(static_cast<int>(source), std::memory_order_release);
}

inline AudioSource audioSource()
{
    return static_cast<AudioSource>(detail::audioSourceSlot().load(std::memory_order_acquire));
}

/// The settings value ("all", "focused") as the mode; anything else is AllApps,
/// so an unknown or missing value never changes what the stream carries.
inline AudioSource audioSourceFromString(const std::string& value)
{
    return value == "focused" ? AudioSource::FocusedApp : AudioSource::AllApps;
}

namespace detail {
struct AudioFocusState
{
    std::mutex mutex;
    std::string streamedDisplay;
    std::string focusedApp;
};
inline AudioFocusState& audioFocusState()
{
    static AudioFocusState state;
    return state;
}
} // namespace detail

/// The GDI name of the display this stream shows ("\\.\DISPLAY2"), set by the
/// capture whenever it locates its output. "Focused app" follows focus only to
/// windows on it, so apps on another screen never reach the stream. Empty:
/// not known, and focus is followed on every screen.
inline void setStreamedDisplay(const std::string& gdiName)
{
    auto& s = detail::audioFocusState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.streamedDisplay = gdiName;
}

inline std::string streamedDisplay()
{
    auto& s = detail::audioFocusState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.streamedDisplay;
}

/// The executable whose sound "Focused app" streams right now ("huntgame.exe"),
/// for the Settings page to show; empty when none (another mode, no stream,
/// or the app closed). Written by the capture, read by whoever reports it.
inline void setFocusedAppName(const std::string& exe)
{
    auto& s = detail::audioFocusState();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.focusedApp = exe;
}

inline std::string focusedAppName()
{
    auto& s = detail::audioFocusState();
    std::lock_guard<std::mutex> lock(s.mutex);
    return s.focusedApp;
}

} // namespace mw::native
