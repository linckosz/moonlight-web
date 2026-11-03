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

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>

namespace mw::native {

/// Which sound the stream carries (Settings → "Stream audio from").
///
///  - AllApps: the whole mix the host plays, as the engine always did. Default.
///  - FocusedApp: "Automatic", the app in focus on the streamed screen
///    (AppAudioCapture.h), so Discord or a browser stay out of the stream.
///  - OneApp: one app the user picked, by executable name.
///
/// Only the Windows capture honours the last two (per-process loopback,
/// Windows 11); the other platforms stream the whole mix.
enum class AudioSource : int
{
    AllApps = 0,
    FocusedApp = 1,
    OneApp = 2,
};

struct AudioSourceChoice
{
    AudioSource source = AudioSource::AllApps;
    std::string app; ///< OneApp: the executable, lower case ("discord.exe").
};

/// The settings value ("all", "focused", "app:discord.exe") as the choice;
/// anything else is AllApps, so an unknown or missing value never changes what
/// the stream carries.
inline AudioSourceChoice parseAudioSource(const std::string& value)
{
    if (value == "focused") return {AudioSource::FocusedApp, {}};
    constexpr std::string_view kAppPrefix = "app:";
    if (value.size() > kAppPrefix.size() && value.compare(0, kAppPrefix.size(), kAppPrefix) == 0) {
        std::string exe = value.substr(kAppPrefix.size());
        std::transform(exe.begin(), exe.end(), exe.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return {AudioSource::OneApp, std::move(exe)};
    }
    return {};
}

/// Windows' own surfaces: the desktop, the taskbar, Alt+Tab, Start, the prompts,
/// the overlays drawn over a game. None of them is an app the user went to, so
/// taking the focus says nothing about whose sound they want. Explorer has a
/// second reason to be here: capture takes a process's whole tree, and
/// everything launched from the Start menu runs under it, Discord included.
inline bool isWindowsShellExe(std::string_view lowerExe)
{
    static constexpr std::array<std::string_view, 14> kShell = {
        "explorer.exe",
        "shellexperiencehost.exe",
        "startmenuexperiencehost.exe",
        "searchhost.exe",
        "searchapp.exe",
        "lockapp.exe",
        "textinputhost.exe",
        // A store app's frame, when the app inside could not be told apart.
        "applicationframehost.exe",
        "logonui.exe",
        "shellhost.exe",
        "consent.exe",
        "gamebar.exe",
        "nvidia overlay.exe",
        "dwm.exe",
    };
    return std::find(kShell.begin(), kShell.end(), lowerExe) != kShell.end();
}

/// The GDI name of the display a session shows ("\\.\DISPLAY2"). The session
/// refreshes it whenever it locates its output, and the capture's own thread
/// reads it. Empty: not known, and focus is followed on every screen.
class StreamedDisplay
{
public:
    void set(const std::string& gdiName)
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Name = gdiName;
    }
    std::string get() const
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        return m_Name;
    }

private:
    mutable std::mutex m_Mutex;
    std::string m_Name;
};

/// What per-app capture needs from its session.
struct AppAudioOptions
{
    AudioSourceChoice choice;
    /// Null: focus is followed on every screen.
    std::shared_ptr<StreamedDisplay> display;
    /// The executable now captured ("huntgame.exe"), empty when none. Called on
    /// the capture's own thread, each time it changes.
    std::function<void(const std::string&)> onAppChanged;
};

} // namespace mw::native
