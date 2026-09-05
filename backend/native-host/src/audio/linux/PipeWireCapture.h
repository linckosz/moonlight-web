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

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace mw::native::audio {

/// What the host is playing, from PipeWire: a capture stream on the monitor
/// of the DEFAULT output, delivered as 48 kHz stereo float on PipeWire's own
/// thread. The 5 ms cadence is not here — PacedOpusSink owns it — because
/// PipeWire calls when the graph runs, not when the wire is due.
///
/// ── Why PipeWire and not PulseAudio ─────────────────────────────────────────
///
/// libpulse is LGPL and off this module's allow-list (LICENSE.md); libpipewire
/// is MIT and on it. PipeWire is also the audio server of every current
/// desktop (Fedora since 34, Ubuntu since 22.10, Debian since 12, Arch,
/// SteamOS, Bazzite), and PulseAudio applications keep working on it through
/// pipewire-pulse — the reverse is not true: a PipeWire capture stream on a
/// machine where PulseAudio still owns the sound card finds a graph with no
/// output in it. Ubuntu 22.04 is that machine. There the session streams
/// silent and the log says so, in words the user can act on.
///
/// ── What the stream asks for ────────────────────────────────────────────────
///
/// `stream.capture.sink = true` — capture the sink's monitor, not a
/// microphone — with no target named, so the session manager attaches it to
/// the default output and MOVES it when the user changes that default
/// mid-session (the WASAPI loopback's "follows the default endpoint", done by
/// the server). Interleaved F32, 48 kHz, two channels: PipeWire's adapter
/// converts from whatever the sink runs at, so a 44.1 kHz or 5.1 output costs
/// a resampler in the graph and nothing here. `node.latency = 240/48000` asks
/// the graph for 5 ms quanta; whether it gets them depends on the other
/// clients, and the pacer's queue absorbs a 21 ms default quantum either way.
///
/// ── Failure, and staying honest about it ────────────────────────────────────
///
/// No daemon → start() fails and the session has no audio. A daemon with no
/// output to attach to → the stream reports an error, the capture retries
/// every two seconds (a sink can appear: HDMI plugged, pipewire-pulse
/// started), and nothing is pushed meanwhile — the sink ticks silence. No
/// microphone is ever opened: the only source this asks for is a monitor.
class PipeWireCapture
{
public:
    /// Stereo interleaved float32, `frames` samples per channel. Called on
    /// PipeWire's data thread; must not block for long.
    using SampleCallback = std::function<void(const float* interleaved, size_t frames)>;

    explicit PipeWireCapture(SampleCallback onSamples);
    ~PipeWireCapture();
    PipeWireCapture(const PipeWireCapture&) = delete;
    PipeWireCapture& operator=(const PipeWireCapture&) = delete;

    /// Connect to the daemon and ask for the default output's monitor. Fails
    /// — with the reason — only when there is no daemon to talk to; a graph
    /// without an output is reported and retried by the thread.
    bool start(std::string& error);

    /// Stop and tear down. Idempotent. No callback is in flight once this
    /// returns.
    void stop();

    /// Samples per channel delivered since start().
    int64_t framesCaptured() const;

    /// The library's runtime version, e.g. "0.3.48", for the session log.
    static std::string libraryVersion();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mw::native::audio
