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

#include "../../core/DesktopGate.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace mw::native::platform {

/// Watches this session's lock state for a DesktopGate: the one thing that
/// opens it (core/DesktopGate.h says why the input desktop is not enough).
///
/// A thread of its own with an invisible top-level window, registered for this
/// session's notifications, then the first read (security review 13/11/2026,
/// C1): a notice that comes in between waits in the thread's queue, and has
/// the session read again when it is taken. A message-only window does not
/// always get these notifications (D3); the reads every kReadEveryMs are the
/// net whatever they do.
///
/// A read is WTSSessionInfoEx of this process's own session
/// (ProcessIdToSessionId, never the console's, D2): unlocked only when its
/// SessionFlags say so AND it is the active session of its console (D1).
/// Anything else, a failure included, counts as locked (C3).
class SessionLockWatch
{
public:
    struct Callbacks
    {
        /// The gate closed since the last word: what the viewer holds is let
        /// go of. On the watcher's thread.
        std::function<void()> onClosed;
        /// It opened: the next picture is a fresh one, sent as a key frame.
        /// On the watcher's thread.
        std::function<void()> onOpened;
    };

    /// How often the session is read again, whatever was announced or not
    /// (C2 asks once a second at least).
    static constexpr int kReadEveryMs = 250;

    SessionLockWatch(std::shared_ptr<DesktopGate> gate, Callbacks callbacks);
    ~SessionLockWatch();

    SessionLockWatch(const SessionLockWatch&) = delete;
    SessionLockWatch& operator=(const SessionLockWatch&) = delete;

    /// The thread, its window, the registration, then the first read, applied
    /// to the gate before this returns. False, with @p error, when there is no
    /// thread or no window: the gate stays closed.
    bool start(std::string& error);

    /// Have the session read again now, from any thread: a picture or an event
    /// found the input desktop not the user's and closed the gate.
    void kick();

    /// Idempotent. The callbacks are not called once it returns.
    void stop();

private:
    void run();
    void onNotice(unsigned long code, unsigned long session);
    /// Reads the session and the input desktop, gives both to the gate, and
    /// says what changed.
    void evaluate();
    /// The log line and the callbacks for whatever the gate did since the last
    /// call, by whoever it was.
    void settle();

    /// The window procedure, in the .cpp, where <windows.h> is.
    friend struct LockWatchWindow;

    std::shared_ptr<DesktopGate> m_Gate;
    Callbacks m_Callbacks;
    std::thread m_Thread;

    /// Set by the thread once its window is up and the first read applied.
    std::mutex m_StartMutex;
    std::condition_variable m_StartWake;
    bool m_StartDone = false;
    bool m_StartOk = false;
    std::string m_StartError;

    /// The window, for kick() and stop() from other threads. Null until the
    /// thread made it, and once it is going.
    std::mutex m_WindowMutex;
    void* m_Window = nullptr;

    // The thread's own.
    unsigned long m_Session = 0;
    uint64_t m_Seen = 0;
    bool m_SaidFirst = false;
    uint64_t m_Notices = 0;
};

} // namespace mw::native::platform
