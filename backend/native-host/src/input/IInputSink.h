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
};

} // namespace mw::native::input
