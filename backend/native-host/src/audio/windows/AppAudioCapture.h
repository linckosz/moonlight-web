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

#include "../FocusTarget.h"
#include "mw/native/AudioSource.h"

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mw::native::audio {

class AudioPacer;

/// The sound of one app only — Settings → "Stream audio from" (AudioSource.h).
/// One per-process loopback client (Windows 10 build 20348+, i.e. Windows 11)
/// in INCLUDE-tree mode, so the app's own child processes (a browser's audio
/// service, a game's helper) are heard with it and nothing else is.
///
/// Automatic aims it at the process owning the foreground window, following
/// FocusTarget.h's rule: focus moving to an app on the streamed screen switches
/// to it; focus on another screen, the taskbar or the desktop changes nothing;
/// an app that closes leaves silence. A picked app is found by its executable,
/// and its sound comes back when it restarts.
///
/// Two threads:
///  - a scanner (ordinary priority) that reads the foreground window every
///    100 ms, or looks for the picked app every second, and activates a client
///    for a new target — the slow part, kept off the audio thread;
///  - the caller's audio thread, which pump()s the client into the AudioPacer.
///
/// Never a fallback to the whole mix: with no target, or a target that cannot
/// be captured, the stream is silent (the pacer pads it). A Windows without
/// per-process loopback says so in the log once.
class AppAudioCapture
{
public:
    explicit AppAudioCapture(AppAudioOptions options);
    ~AppAudioCapture();
    AppAudioCapture(const AppAudioCapture&) = delete;
    AppAudioCapture& operator=(const AppAudioCapture&) = delete;

    /// Audio thread: move what the client captured into the pacer.
    void pump(AudioPacer& pacer, int64_t nowUs);

    struct Client;

private:
    void scannerLoop() noexcept;
    void scanOnce();
    FocusSample sampleForeground(const std::string& selfExe);
    void publishApp(const std::string& exe);

    const AppAudioOptions m_Options;
    std::thread m_Scanner;
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    bool m_Stop = false;

    /// The client the audio thread drains. The scanner swaps it under m_Mutex;
    /// the audio thread takes a copy of the pointer.
    std::shared_ptr<Client> m_Client;

    // Scanner thread only.
    FocusTarget m_Target;
    void* m_TargetProcess = nullptr; ///< HANDLE, held so a reused PID is not mistaken for it.
    // The foreground window last read, resolved once rather than on every poll.
    void* m_FrontWindow = nullptr; ///< HWND
    uint32_t m_FrontOwner = 0;
    uint32_t m_FrontPid = 0;
    std::string m_FrontExe;
    std::string m_PublishedApp;
    uint32_t m_RetryPid = 0;
    int64_t m_RetryAfterUs = 0;
    bool m_ReportedUnsupported = false;
};

} // namespace mw::native::audio
