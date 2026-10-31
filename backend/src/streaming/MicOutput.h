/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 MoonlightWeb contributors
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

#include <functional>
#include <memory>
#include <string>

// Where the viewer's microphone comes out on this machine: a device the host's
// own apps can record from. One backend per OS, chosen at build time; none of
// them includes Qt, so each compiles on its own (MicUplink logs for them).
//
//   Windows — WASAPI playback into the render side of a virtual cable (VB-CABLE's
//             "CABLE Input", Steam's "Steam Streaming Microphone"): its capture
//             side is the microphone the apps see. Windows has no virtual
//             microphone of its own, and MoonlightWeb ships no audio driver.
//   Linux   — PipeWire: a virtual source, "MoonlightWeb Microphone", made once
//             and left in the graph (object.linger), and a playback stream into
//             it per session — so an app keeps the device across streams.
//   Others  — none yet: unavailable() says so.
namespace mw::mic {

class JitterRing;

/// What the backends report through: one line, already worded.
using LogFn = std::function<void(const std::string&)>;

class IMicOutput
{
public:
    virtual ~IMicOutput() = default;

    /// Opens the device and starts pulling 48 kHz mono from `ring`, which must
    /// outlive stop(). False with `why` when there is nowhere to play it.
    virtual bool start(JitterRing& ring, std::string& why) = 0;

    /// Stops pulling and closes the device. Idempotent.
    virtual void stop() = 0;

    /// The device it plays into, for the log.
    virtual std::string deviceName() const = 0;
};

/// @param device the admin's choice (settings.json `mic_device`): part of a
///        device's name; empty or "auto" picks the first virtual cable found.
std::unique_ptr<IMicOutput> makeOutput(const std::string& device, LogFn log);

/// Empty when this machine has somewhere to play a microphone into; otherwise
/// why not, in a sentence a viewer can act on. Cheap: no device is opened.
std::string unavailable(const std::string& device);

} // namespace mw::mic
