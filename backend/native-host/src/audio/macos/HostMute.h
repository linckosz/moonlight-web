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

#include <cstdint>
#include <string>
#include <vector>

namespace mw::native::audio {

/// Silence this Mac's speakers for the length of a session while the capture
/// keeps hearing the mix — the macOS half of what Sunshine calls
/// `localAudioPlayMode=0`. Same contract as the Windows class of the same
/// name; the machinery underneath is simpler, and measurably so.
///
/// WASAPI loopback taps the audio ENGINE, so on Windows the master volume
/// reaches the capture and only a driver-level mute escapes it. macOS taps
/// somewhere else: ScreenCaptureKit's audio is a second output of the same
/// stream (§20.8), fed by the applications, not by the output device. Measured
/// on the bench (08/09/2026, a 440 Hz tone playing throughout, RMS of the tap
/// over 2.5 s per state):
///
///     baseline 0.2997 · endpoint muted 0.3027 · volume 0.5 0.3028 ·
///     volume 0 0.3029 · restored 0.3030
///
/// Identical to within the noise: **neither the mute nor the volume of the
/// output device is on the capture's path**. Two consequences, both of which
/// shape the code below:
///
///   * the volume is usable here, where on Windows it never was;
///   * there is no need for the "route to a device that drives no speaker"
///     strategy Windows falls back on — nothing about the output device can
///     take the sound away from the stream, so muting the one the user
///     actually listens to is enough.
///
/// Hence two strategies and a documented "cannot":
///
///   1. EndpointMute — `kAudioDevicePropertyMute` on the default output, on
///      its master element or, failing that, on its stereo pair. What the user
///      sees is the muted speaker icon, and their volume level is untouched.
///   2. VolumeZero — an output with no settable mute (some HDMI and AirPlay
///      endpoints): its volume is taken to zero and put back at release.
///   3. None — the Mac keeps hearing itself, and the log says why.
///
/// engage() before the capture opens and release() after it closes, purely to
/// mirror Windows: on macOS the order genuinely does not matter.
class HostMute
{
public:
    enum class Strategy
    {
        None,
        EndpointMute,
        VolumeZero,
    };

    HostMute() = default;
    ~HostMute();
    HostMute(const HostMute&) = delete;
    HostMute& operator=(const HostMute&) = delete;

    /// Silence the speakers if there is a way to. `how` receives one sentence
    /// for the log in every case, the failed one included.
    Strategy engage(std::string& how);

    /// Undo what engage() did, if anything. Idempotent. Leaves a state the
    /// user changed meanwhile alone: a mute they lifted during the session is
    /// not put back, and a volume they moved is not overwritten.
    void release();

    Strategy strategy() const { return m_Strategy; }

    /// Which strategy engage() WOULD pick on this machine, without touching
    /// anything. For the probe, the tests and the diagnostics.
    static Strategy available(std::string& why);

private:
    /// One element of the output device and the value it held before we wrote
    /// to it — 0/1 for a mute, a scalar in [0, 1] for a volume.
    struct Saved
    {
        uint32_t element;
        float previous;
    };

    Strategy m_Strategy = Strategy::None;
    /// The device we wrote to. Kept rather than resolved again at release: the
    /// user may have changed their default output mid-session, and the one to
    /// put back is the one we silenced.
    uint32_t m_Device = 0;
    /// Empty when there was nothing to do (already silent) — release() then
    /// has nothing to undo either.
    std::vector<Saved> m_Saved;
};

const char* toString(HostMute::Strategy s);

} // namespace mw::native::audio
