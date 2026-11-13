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
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace mw::native::input {

/// Whether GNOME reverses the wheel of our uinput mouse, so UinputInput can
/// send it the other way and the page still scrolls the way the client asked.
///
/// ── Why the host has to know ────────────────────────────────────────────────
///
/// The browser hands over a wheel delta already in the client's own direction,
/// natural scrolling included, and a Windows or macOS host injects it as it is.
/// GNOME does not stop there: its "Natural Scrolling" switch for mice
/// (org.gnome.desktop.peripherals.mouse natural-scroll) is applied by the
/// compositor to every mouse it sees, and our uinput device is one. With it
/// on, all four directions came out reversed (UM790Pro, Ubuntu GNOME Wayland,
/// 12/11/2026); with it off they followed the client. The client cannot see a
/// host setting, so only the host can undo it.
///
/// ── How it is read ──────────────────────────────────────────────────────────
///
/// By running /usr/bin/gsettings, the desktop's own reader, rather than
/// parsing dconf's database here or loading GIO into this process. The child
/// holds nothing of ours: the app clears its ambient and inheritable
/// capabilities at main() (common/LinuxCapabilities.h), so an exec of a binary
/// without file capabilities starts with none, and the child gets /dev/null and
/// a pipe, nothing else. The path is fixed, never looked up in PATH.
/// Never as root nor set-uid: there the exec would give the child every
/// capability back, and the modules gsettings loads are the environment's
/// choice. The wheel is then left as it is.
///
/// Read once when the session starts, then again, at most every two seconds,
/// when the wheel is used: a switch flipped mid-stream is followed from the
/// next turn of the wheel. The read runs on a thread of its own; the wheel
/// never waits for it and uses the last answer.
///
/// Only on GNOME (XDG_CURRENT_DESKTOP), and only while gsettings answers
/// "true" or "false": any other desktop, a missing gsettings, a missing schema
/// or an answer that is neither leaves the wheel exactly as it is today.
/// KDE Plasma keeps its own per-device setting and is not read here.
class GnomeNaturalScroll
{
public:
    GnomeNaturalScroll() = default;
    ~GnomeNaturalScroll() { stop(); }

    GnomeNaturalScroll(const GnomeNaturalScroll&) = delete;
    GnomeNaturalScroll& operator=(const GnomeNaturalScroll&) = delete;

    /// Start following the setting, if this is a GNOME desktop with gsettings.
    /// Never blocks on the first read and never fails: off GNOME it says so
    /// once and stays off.
    void start();
    void stop();

    /// Whether to send the wheel reversed. Cheap, never blocks: the last answer,
    /// and a nudge to read it again when it is more than two seconds old.
    bool reversed();

private:
    using Clock = std::chrono::steady_clock;

    void run();
    void readOnce();

    std::atomic<bool> m_Reversed{false};
    std::thread m_Thread;
    std::mutex m_Mutex;
    std::condition_variable m_Wake;
    bool m_Stop = false;           ///< under m_Mutex
    bool m_Wanted = false;         ///< under m_Mutex: a read was asked for
    Clock::time_point m_LastAsk{}; ///< under m_Mutex
    int m_Logged = -1;             ///< the reader thread's: last value logged, -1 none
    bool m_FailLogged = false;
};

} // namespace mw::native::input
