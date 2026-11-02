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

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

namespace mw::native::audio {

class AudioPacer;

/// The sound of the app in front only — Settings → "Stream audio from: App in
/// front" (AudioSource.h). One per-process loopback client (Windows 10 build
/// 20348+, i.e. Windows 11) in INCLUDE-tree mode, aimed at the process that
/// owns the foreground window, so the app's own child processes (a browser's
/// audio service, a game's helper) are heard with it and nothing else is.
///
/// Which app that is follows FocusTarget.h's rule: focus moving to an app on
/// the streamed screen switches to it; focus on another screen, the taskbar or
/// the desktop changes nothing; an app that closes leaves silence.
///
/// Two threads:
///  - a scanner (ordinary priority) that every 500 ms reads the foreground
///    window, applies the rule, and activates a client for a new target — the
///    slow part, kept off the audio thread;
///  - the caller's audio thread, which pump()s the client into the AudioPacer.
///
/// Never a fallback to the whole mix: with no target, or a target that cannot
/// be captured, the stream is silent (the pacer pads it). A Windows without
/// per-process loopback says so in the log once.
class AppAudioCapture
{
public:
    AppAudioCapture();
    ~AppAudioCapture();
    AppAudioCapture(const AppAudioCapture&) = delete;
    AppAudioCapture& operator=(const AppAudioCapture&) = delete;

    /// Audio thread: move what the client captured into the pacer.
    void pump(AudioPacer& pacer, int64_t nowUs);

    struct Client;

private:
    void scannerLoop() noexcept;
    void scanOnce();

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
    uint32_t m_RetryPid = 0;
    int64_t m_RetryAfterUs = 0;
    bool m_ReportedUnsupported = false;
};

} // namespace mw::native::audio
