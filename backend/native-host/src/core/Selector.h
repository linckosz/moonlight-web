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

#include "mw/native/Capabilities.h"
#include "mw/native/SessionConfig.h"

#include <string>

namespace mw::native {

/// Everything the "Detect → Optimize" half of the engine decides, before a
/// single platform object is created.
struct Selection
{
    const DisplayInfo* display = nullptr;
    const GpuInfo* gpu = nullptr;

    Codec codec = Codec::H264;
    EncoderApi encoder = EncoderApi::None;

    int width = 0;
    int height = 0;
    int fps = 0;

    /// 10-bit, and therefore actually HDR. False when HDR was asked for but the
    /// display is in SDR or the encoder has no 10-bit path — the session then
    /// runs SDR rather than failing.
    bool hdr = false;

    /// 4:4:4 granted: asked for AND the chosen codec has it on this encoder.
    /// The request steers the codec choice (see select()), so this is false
    /// only when no codec the client and the GPU share can carry 4:4:4 — the
    /// session then runs 4:2:0 rather than failing.
    bool yuv444 = false;

    /// The chosen GPU is not the one driving the display, so each frame costs a
    /// cross-GPU copy (§6). Only ever true when the display's own GPU has no
    /// encoder at all.
    bool crossGpuCopy = false;
};

/// Resolve a SessionConfig against what the machine has.
///
/// Pure: no OS calls, no allocation beyond the error string, no state. That is
/// what lets the whole "which GPU, which encoder, which codec" policy — the
/// part users would otherwise have to configure — be unit-tested on any machine
/// including CI, with no GPU present.
///
/// Returns false and fills `error` when the display does not exist, or when the
/// client and the machine share no codec at all.
bool select(const Capabilities& caps, const SessionConfig& config, Selection& out,
            std::string& error);

} // namespace mw::native
