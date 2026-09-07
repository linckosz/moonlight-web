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

namespace mw::native::audio {

/// Silence the host's speakers for the length of a session while the loopback
/// capture keeps hearing the mix — what Sunshine calls `localAudioPlayMode=0`.
///
/// WASAPI loopback taps the audio engine's output, BEFORE the endpoint. What
/// the engine does to that output is what decides whether a mute reaches the
/// tap. Measured on this project's bench (D5 probe, 07/09/2026):
///
///   * an endpoint whose driver mutes IN HARDWARE (`ENDPOINT_HARDWARE_SUPPORT_
///     MUTE`, an HDMI output for instance) is muted by the driver after the
///     tap — loopback RMS unchanged with the speakers silent;
///   * the master VOLUME is applied by the engine on the same machine — set to
///     zero, the loopback goes quiet too. Never usable;
///   * an exclusive-mode stream cannot even be opened while the game holds a
///     shared one (`AUDCLNT_E_DEVICE_IN_USE`). Never usable either.
///
/// Hence two strategies, tried in this order, and a documented "cannot":
///
///   1. HardwareMute — the default output mutes in hardware: `SetMute(TRUE)`
///      on it, restored at release. No dependency, no visible change but the
///      speaker icon.
///   2. VirtualSink — no hardware mute, but a playback device that drives no
///      speaker exists (Steam Streaming Speakers, VB-Cable, VoiceMeeter…): it
///      becomes the default output for the session and the loopback opens on
///      it, so the game's sound goes there and nowhere else. The previous
///      default is put back at release. Uses the same unpublished
///      `IPolicyConfig` interface Sunshine and every output switcher use.
///   3. None — the host keeps hearing itself, and the log says why.
///
/// engage() must run BEFORE the loopback opens: strategy 2 moves the default
/// device the loopback is opened on. release() after the loopback is closed.
class HostMute
{
public:
    enum class Strategy
    {
        None,
        HardwareMute,
        VirtualSink,
    };

    HostMute() = default;
    ~HostMute();
    HostMute(const HostMute&) = delete;
    HostMute& operator=(const HostMute&) = delete;

    /// Silence the speakers if there is a way to. `how` receives one sentence
    /// for the log in every case, the failed one included.
    Strategy engage(std::string& how);

    /// Undo what engage() did, if anything. Idempotent. Leaves a state the
    /// user changed meanwhile alone: a mute they lifted themselves during the
    /// session is not put back, and a device they picked stays picked.
    void release();

    Strategy strategy() const { return m_Strategy; }

    /// Which strategy engage() WOULD pick on this machine, without touching
    /// anything. For the probe, the tests and the diagnostics.
    static Strategy available(std::string& why);

private:
    Strategy m_Strategy = Strategy::None;
    /// HardwareMute: the endpoint id we muted, to find it again at release.
    /// VirtualSink: the id of the default output we displaced.
    std::wstring m_RestoreId;
    /// VirtualSink: the device we made default, so release() can tell "still
    /// ours" from "the user picked something else meanwhile".
    std::wstring m_SinkId;
    bool m_CoOwned = false;
};

const char* toString(HostMute::Strategy s);

} // namespace mw::native::audio
