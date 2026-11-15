/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

// Whether somebody is using a stream: a key, the mouse, a finger, a pad.
//
// A booth whose streams have PINs (booth_stream_pin, issue #68) lets a stream
// nobody used for a while be taken over without its PIN. "Used" is read here,
// on the messages the host acts on, after the input policy let them through:
// a view-only guest's presses never count, and neither does what a page sends
// on its own (heartbeats, keyframe requests, link reports, the held-input
// state it repeats while something is down).
//
// One stream per worker process, so one flag per process: every transport
// that receives input notes it, the worker reads and clears it on its own
// beat and tells the server (an `inputSeen` event). A relaxed store on the
// input path, nothing more.

#include <QString>

#include <atomic>

namespace InputActivity {

/// How often the worker tells the server, at most: the idle time it serves is
/// counted in minutes.
constexpr int kReportMs = 30 * 1000;

/// Whether a message of this type is a person acting on the host. A list, so
/// that a type added later is not activity until it is named here.
inline bool isActivity(const QString& type)
{
    static const char* const kTypes[] = {
        // keyboard
        "keydown", "keyup", "textinput", "secureattention", "clipboardpaste",
        // mouse, and touch, which the page sends as the mouse
        "mousemove", "mousedown", "mouseup", "mousewheel", "mousehwheel",
        // a pad's state, sent when it changes (and repeated while held away from rest)
        "gamepad", "gamepadconnect"};
    for (const char* t : kTypes)
        if (type == QLatin1String(t)) return true;
    return false;
}

namespace detail {
inline std::atomic<bool> g_Seen{false};
}

/// Somebody acted on the host since the last take().
inline void note()
{
    detail::g_Seen.store(true, std::memory_order_relaxed);
}

/// Whether somebody did, and from now on not.
inline bool take()
{
    return detail::g_Seen.exchange(false, std::memory_order_relaxed);
}

/// The type of a message that was let through: noted when it is activity.
inline void noteIfActivity(const QString& type)
{
    if (isActivity(type)) note();
}

} // namespace InputActivity
