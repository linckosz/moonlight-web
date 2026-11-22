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

#include <string>

// Why a Mac's display is away, in words its owner can act on.
//
// ScreenCaptureKit says nothing a person can use when the display it was
// asked for is dark: it loses it ("Failed to find any displays or windows to
// capture", -3815) and, asked to start again, never calls its completion back
// — which the session reports as "ScreenCaptureKit did not start within 5 s".
// That is what a viewer read on 22/11/2026, streaming a Mac whose locked
// screen saver took the panel back every 5 to 25 s after the session woke it.
// The same bench showed what does NOT break the capture: the Mac locked with
// its panel kept on streamed on, lock screen and all. So the cause named first
// is the dark panel or the screen saver; the lock is said with it, because
// that is what the owner has to undo as well.
//
// What is read, and only read (MacSession.mm): whether the panel is asleep
// (CGDisplayIsAsleep), whether the screen saver runs (its engine's bundle id),
// whether the session is locked (the Quartz session dictionary). Nothing here
// or there touches the lock, the screen saver, the password or the privacy
// settings, and nothing types or clicks on the Mac's behalf: a stream never
// undoes what its owner set.
//
// The full reason is for the Mac's owner and the local log. A guest of a
// shared stream is not told that the Mac is locked — that is telling a
// stranger its owner is away: the server takes kCode off the head of the
// error into the answer's "code" (backend/src/streaming/DisplayAwayAnswer.h)
// and, on the way out to a guest, puts a neutral sentence in place of the
// reason.
//
// Pure strings, no platform: tested on every OS, like FrameFit.h.

namespace mw::native::platform::away {

/// What the Mac's display looks like when its capture failed. Each field is
/// false when it could not be read.
struct AwayState
{
    bool asleep = false;      ///< the panel is dark (display sleep)
    bool screenSaver = false; ///< the screen saver is running
    bool locked = false;      ///< the login session is locked
};

/// The stable tag at the head of every error this header words, "tag: text":
/// for the server (the guest's answer) and, later, the page's translations.
/// DisplayAwayAnswer.h spells it again; the backend tests hold the two equal.
inline constexpr const char* kCode = "mac_display_away";

/// Whether anything is known: false means the capture failed for a reason
/// this header cannot name, and the error stays ScreenCaptureKit's own.
inline bool known(const AwayState& s)
{
    return s.asleep || s.screenSaver || s.locked;
}

/// The reason in a few words, for the log: "screen off, screen saver on, Mac
/// locked". Empty when nothing is known.
inline std::string logReason(const AwayState& s)
{
    std::string out;
    auto add = [&out](const char* part) {
        if (!out.empty()) out += ", ";
        out += part;
    };
    if (s.asleep) add("screen off");
    if (s.screenSaver) add("screen saver on");
    if (s.locked) add("Mac locked");
    return out;
}

/// The owner's sentence: what is wrong, then what to do. Empty when nothing
/// is known.
inline std::string ownerReason(const AwayState& s)
{
    std::string what;
    if (s.asleep && s.screenSaver)
        what = "the Mac's screen is off and its screen saver is on";
    else if (s.asleep)
        what = "the Mac's screen is off";
    else if (s.screenSaver)
        what = "the Mac's screen saver is on";
    if (s.locked) what += what.empty() ? "the Mac is locked" : ", and the Mac is locked";
    if (what.empty()) return {};

    const bool wake = s.asleep || s.screenSaver;
    const char* todo = wake && s.locked ? "wake and unlock it" : wake ? "wake it" : "unlock it";
    return what + " — " + todo + ", then start the stream again";
}

/// The error a session that could not start reports: kCode, the owner's
/// reason, ScreenCaptureKit's own words in brackets. Just the latter when
/// nothing is known.
inline std::string startError(const AwayState& s, const std::string& captureError)
{
    const std::string reason = ownerReason(s);
    if (reason.empty()) return captureError;
    std::string out = std::string(kCode) + ": " + reason;
    if (!captureError.empty()) out += " (" + captureError + ")";
    return out;
}

} // namespace mw::native::platform::away
