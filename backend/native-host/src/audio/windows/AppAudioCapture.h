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

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace mw::native::audio {

class AudioPacer;

/// The host's sound WITHOUT the apps AudioAppFilter names: one per-process
/// loopback client (Windows 10 build 20348+, i.e. Windows 11) for each app that
/// has an audio session and is not excluded, mixed back into one stereo stream.
///
/// Why not one "everything except" client: Windows offers exactly that
/// (PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE), but for ONE process
/// tree per client, and clients cannot be subtracted from one another. Several
/// exclusions therefore mean the inverse: include each app that may be heard.
///
/// Two threads:
///  - a scanner (ordinary priority) that, every 750 ms or at once when woken,
///    lists the processes that own an audio session, decides which process
///    trees to capture, and activates/initialises clients for the new ones —
///    the slow part, kept off the audio thread;
///  - the caller's audio thread, which pump()s: drains every client into its
///    own FIFO and pushes the mix into the AudioPacer.
///
/// Process trees: an INCLUDE client takes the target's child processes too. So
/// a session owner that has an excluded app among its descendants (Explorer,
/// which launched Discord from the Start menu, say) is NOT captured — its own
/// sound is lost rather than the excluded app's leaked — and an owner whose
/// ancestor is already captured is skipped, or its sound would play twice.
///
/// Never a silent fallback to the whole mix: with exclusions set, an app that
/// cannot be captured is missing from the stream, never the reverse. A machine
/// where per-process loopback does not exist at all streams silence and says
/// so in the log; clearing the exclusions brings the whole mix back.
class AppAudioCapture
{
public:
    AppAudioCapture();
    ~AppAudioCapture();
    AppAudioCapture(const AppAudioCapture&) = delete;
    AppAudioCapture& operator=(const AppAudioCapture&) = delete;

    /// Audio thread: move what every client captured into the pacer, mixed.
    void pump(AudioPacer& pacer, int64_t nowUs);

    /// Ask the scanner for a pass now (the exclusion list changed).
    void wake();

    /// Clients currently captured (for the log).
    int clientCount() const;

    struct Client;

private:
    void scannerLoop() noexcept;
    void scanOnce();

    std::thread m_Scanner;
    mutable std::mutex m_Mutex;
    std::condition_variable m_Cv;
    bool m_Stop = false;
    bool m_Wake = true;

    /// The clients the audio thread mixes. The scanner builds a new list and
    /// swaps it in under m_Mutex; the audio thread takes a copy of the pointer.
    std::shared_ptr<const std::vector<std::shared_ptr<Client>>> m_Clients;

    /// Audio thread only: the mix buffer, kept to avoid an allocation a tick.
    std::vector<float> m_Mix;
    int64_t m_LastReportUs = 0;
};

} // namespace mw::native::audio
