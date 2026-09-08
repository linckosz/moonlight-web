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

#include "PipeWireDefaults.h"

#include <memory>
#include <string>

namespace mw::native::audio {

/// Silence this machine's speakers for the length of a session while the
/// capture keeps hearing the mix — the Linux third of what GameStream calls
/// `localAudioPlayMode=0`. Same contract as the Windows and macOS classes of
/// the same name.
///
/// ── Where the tap sits, measured rather than assumed ────────────────────────
///
/// The tap is the MONITOR of the default sink (PipeWireCapture.h). The obvious
/// fear — and what this project's own plan wrote down as a certainty — is that
/// a monitor hears exactly what the sink plays, so muting the sink would mute
/// the capture with it. That is PulseAudio's semantics. **PipeWire's default is
/// the opposite**, and the difference is one node property.
///
/// Measured on the Linux bench (08/09/2026, WirePlumber 0.4.8 on PipeWire
/// 0.3.48, a 440 Hz tone at 0.25 playing throughout, RMS of the monitor over
/// 2 s per state):
///
///                                real ALSA output   null sink
///     baseline                        0.1755          0.1755
///     sink muted                      0.1755          0.0000
///     volume 0 %                      0.1755          0.0000
///     restored                        0.1755          0.1755
///     monitor.channel-volumes        (absent)          true
///
/// The property is the whole story. `monitor.channel-volumes` decides whether
/// the adapter applies the node's volume and mute to its monitor ports; it
/// defaults to FALSE, so on every real output — ALSA, HDMI, USB, Bluetooth —
/// the monitor is taken upstream of both and a mute never reaches the capture.
/// The only sinks that set it true are the virtual ones the PulseAudio compat
/// layer creates (module-null-sink and friends), which keep Pulse's semantics
/// on purpose.
///
/// Hence two strategies, chosen by reading that property rather than by
/// guessing at the hardware, and a documented "cannot":
///
///   1. SinkMute — the default sink's monitor does not carry its volume: set
///      `mute` on the node and put it back at release. Nothing moves in the
///      user's graph, their volume level is untouched, and what they see is the
///      muted speaker icon. This is the case on every ordinary desktop.
///   2. NullSink — the monitor DOES carry the volume, so muting would take the
///      capture down with it (measured above). A sink that plays nowhere is
///      created and made the default output for the session; the session
///      manager moves the playing streams onto it, the speakers fall silent,
///      and the tap — which follows the default output — lands on its monitor.
///      The previous default is put back at release, and the sink dies with our
///      connection, so a worker that is killed does not strand the machine on a
///      silent output.
///   3. None — the host keeps hearing itself, and the log says why.
///
/// engage() must run BEFORE the tap opens: strategy 2 moves the default sink
/// the tap attaches to. release() after the tap is closed, or the session
/// manager may walk the tap back onto the real output while it still runs.
///
/// ── What this depends on ────────────────────────────────────────────────────
///
/// A session manager that publishes the "default" metadata object — which is
/// how the default output is named at all. WirePlumber does, on every current
/// desktop. The bench's Ubuntu 22.04 was still running the retired
/// pipewire-media-session 0.4.1, which does not (`pactl set-default-sink`
/// there exits 1 and there is no metadata to read): that machine answers None
/// with those words, rather than muting a sink it cannot prove is the one the
/// user listens to.
class HostMute
{
public:
    enum class Strategy
    {
        None,
        SinkMute,
        NullSink,
    };

    HostMute();
    ~HostMute();
    HostMute(const HostMute&) = delete;
    HostMute& operator=(const HostMute&) = delete;

    /// Silence the speakers if there is a way to. `how` receives one sentence
    /// for the log in every case, the failed one included.
    Strategy engage(std::string& how);

    /// Undo what engage() did, if anything. Idempotent. Leaves a state the user
    /// changed meanwhile alone: a mute they lifted during the session is not
    /// put back, and an output they picked themselves stays picked.
    void release();

    Strategy strategy() const;

    /// Which strategy engage() WOULD pick on this machine, without touching
    /// anything. For the probe, the tests and the diagnostics.
    static Strategy available(std::string& why);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

const char* toString(HostMute::Strategy s);

} // namespace mw::native::audio
