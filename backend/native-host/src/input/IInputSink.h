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

#include "mw/native/InputEvent.h"
#include "mw/native/NativeHost.h"

#include <string>

namespace mw::native::input {

/// Injects browser input into the local OS.
///
/// ── No thread, no queue ─────────────────────────────────────────────────────
///
/// inject() applies the event on the calling thread and returns. An
/// implementation MUST be safe to call from any thread and MUST NOT block.
/// SendInput, uinput's write() and CGEventPost all satisfy this. If some future
/// backend cannot, it owns its own serialisation — it does not get to push a
/// queue back onto the caller, because a queue would hand the user a burst of
/// stale events the moment it drained.
///
/// **Which thread that is, today**: still the worker's Qt thread. The relay
/// marshals every input message off libdatachannel's callback with a
/// QueuedConnection, because the handler also drives clipboard, policy and
/// stats objects that live there. That hop is one event-loop turn of pure
/// latency for the native path, and removing it means splitting the keyboard
/// and mouse cases out of the shared handler rather than relaxing anything
/// here. This interface is already safe for it; the relay is not yet.
class IInputSink
{
public:
    virtual ~IInputSink() = default;

    /// Prepare for injection. Called once, before any inject().
    virtual bool start(std::string& error) = 0;

    /// Release everything still held, then shut down. Releasing matters: a
    /// session that ends mid-keypress would otherwise leave the host with a key
    /// stuck down and no one left to lift it.
    virtual void stop() = 0;

    /// Apply one event. Never blocks; unsupported events are ignored.
    virtual void inject(const InputEvent& event) = 0;

    /// The captured display's rectangle on the virtual desktop has moved — a
    /// resolution change, a monitor rearranged behind our back. Absolute pointer
    /// positions are aimed at that rectangle, so a sink that maps them needs the
    /// new one; a sink that only sends relative motion ignores this.
    ///
    /// Plain integers rather than a capture type on purpose: the input layer has
    /// no business including the capture headers to hear about a rectangle.
    ///
    /// The caller serialises this against inject(), so an implementation may
    /// simply store it.
    virtual void setDisplayRect(int left, int top, int right, int bottom)
    {
        (void)left;
        (void)top;
        (void)right;
        (void)bottom;
    }

    /// The bounds of the whole desktop the captured display sits on — the union
    /// of every active monitor, not just ours.
    ///
    /// Only a sink whose absolute pointer is aimed at the DESKTOP rather than at
    /// one screen needs this, which on the three platforms means uinput alone:
    /// a kernel device reports a fraction of its own axis and the compositor
    /// stretches it over everything, so the display's rectangle by itself says
    /// nothing about where a position lands. Windows asks the OS for the same
    /// bounds at each injection (SM_CXVIRTUALSCREEN) and macOS works in global
    /// coordinates throughout, so both ignore this.
    ///
    /// Same contract as setDisplayRect: plain integers, serialised against
    /// inject(), may simply be stored. Never set means "unknown" — a sink must
    /// still work, treating its own display as the whole desktop, which is what
    /// a single-monitor host actually is.
    virtual void setDesktopRect(int left, int top, int right, int bottom)
    {
        (void)left;
        (void)top;
        (void)right;
        (void)bottom;
    }

    /// Whether presses aimed at an elevated (administrator) window may go
    /// through — SessionConfig::allowElevatedInput. A sink on an OS with no
    /// such distinction ignores it. Set before start(), never changed after.
    virtual void setAllowElevated(bool allow) { (void)allow; }

    /// Where to report the gate opening or closing — see InputGate. Called
    /// from inject(), on its thread, on change only. A sink that never gates
    /// never calls it.
    virtual void setGateCallback(InputGateCallback callback) { (void)callback; }

    /// Get the viewer out from behind a window they cannot reach past — see
    /// Session::releaseInputBlock, which this implements. Returns false when
    /// the gate is not closed, or where the platform has no way to do it.
    /// Does its work on a thread of its own: the caller is a viewer's control
    /// message, not something to block on the shell.
    virtual bool releaseBlock() { return false; }
};

} // namespace mw::native::input
