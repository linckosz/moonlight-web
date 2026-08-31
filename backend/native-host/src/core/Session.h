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

#include "mw/native/NativeHost.h"

#include <memory>
#include <string>

namespace mw::native {

/// The four callbacks a session delivers on, bundled so the platform seam takes
/// one parameter instead of four and gaining a fifth later touches one line.
struct SessionCallbacks
{
    VideoCallback onVideo;
    AudioCallback onAudio;
    RumbleCallback onRumble;
    SessionEndedCallback onEnded;
};

/// What the Selector decided, in plain values a platform backend can act on.
///
/// This exists because the decision and the execution live in different places,
/// and the decision is the hard part. Handing the backend only a SessionConfig
/// meant it had to work the GPU out again for itself — and its simpler answer
/// ("the GPU that drives the display") is wrong in precisely the case the
/// Selector exists to handle: a display whose own GPU cannot encode.
///
/// Observed on a real machine: an RX 7600 driving the only monitor while an
/// RTX 5060 Ti held the only usable encoder. The Selector correctly chose NVENC
/// with a cross-GPU copy; the backend re-derived AMD and opened NVENC on it,
/// which fails with "no encode device".
///
/// Values are copied, not pointers into Capabilities — that is a local of the
/// caller and does not outlive createSession().
struct ResolvedTarget
{
    int displayId = -1;
    /// The output's index WITHIN its own adapter, which is what DXGI wants and
    /// is not the global display id on a multi-GPU machine.
    unsigned outputIndex = 0;

    /// The adapter that scans the display out — where capture must happen.
    uint64_t captureAdapterHandle = 0;
    /// The adapter that will encode. Equal to the capture adapter in the normal
    /// case; different only when the display's own GPU cannot encode.
    uint64_t encodeAdapterHandle = 0;

    std::string encodeGpuName;
    EncoderApi encoder = EncoderApi::None;
    Codec codec = Codec::H264;

    /// True when the two adapters differ, so the frame has to cross between
    /// them. Costly, rare, and always worth a log line.
    bool crossGpuCopy = false;

    bool hdr = false;
    bool yuv444 = false;
};

namespace detail {

/// Build the platform's session object. Implemented once per OS backend
/// (and by platform/Unimplemented.cpp when none is built).
///
/// Receives an already-validated config AND the Selector's resolution: the
/// display exists, a codec was agreed with the client, and the GPU to encode on
/// has been chosen. Everything a backend still has to do is genuinely platform
/// work — it must not re-decide any of the above.
std::unique_ptr<Session> createPlatformSession(const SessionConfig& config,
                                               const ResolvedTarget& target,
                                               const SessionCallbacks& callbacks,
                                               std::string& error);

} // namespace detail
} // namespace mw::native
