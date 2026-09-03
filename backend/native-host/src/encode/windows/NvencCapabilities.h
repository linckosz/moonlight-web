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

#include <cstdint>
#include <vector>

namespace mw::native::encode {

/// What NVENC can really do on one adapter.
struct NvencCaps
{
    bool usable = false;
    std::vector<Codec> codecs; ///< best first, as the Selector expects
    bool supports10Bit = false;
    /// The codecs whose encoder answered NV_ENC_CAPS_SUPPORT_YUV444_ENCODE —
    /// H.264 and HEVC on every card that has it, never AV1 (SDK 12).
    std::vector<Codec> codecs444;
    bool supportsIntraRefresh = false;
    bool supportsReferenceInvalidation = false;
};

/// Open a real NVENC session against the adapter with this LUID and ask it what
/// it supports.
///
/// ── Why this has to open a session ─────────────────────────────────────────
///
/// Because the codec list cannot be known any other way. Finding
/// nvEncodeAPI64.dll proves only that an NVIDIA driver is installed somewhere
/// on the machine; it says nothing about which codecs THIS adapter encodes.
/// A GTX 1050 and an RTX 4070 answer to the same DLL and differ on AV1, and
/// deriving that from the device id would be a table to maintain forever and
/// wrong on the day a new chip ships. So the hardware is asked.
///
/// ── What the bench actually showed ─────────────────────────────────────────
///
/// This machine reports five DXGI adapters for three physical GPUs: the two
/// extras are indirect-display drivers (Parsec Virtual Display, Virtual Display
/// Driver) that present NVIDIA's own name and device id under their own LUID.
///
/// The expectation was that they would refuse an encode session and drop out on
/// their own. They do not — they are backed by a real NVIDIA GPU for rendering,
/// so a session opens and reports the same codecs. That turns out to be the
/// right answer rather than a problem: if a display ever hangs off one of them,
/// capturing AND encoding on that same adapter is exactly the pairing that
/// avoids a cross-GPU copy, because DXGI routes both to the same silicon.
///
/// So there is nothing to filter. Adapter identity is settled by DXGI — the
/// adapter that enumerates an output is the one that scans it out — and this
/// query exists purely to learn the codec list, not to second-guess that.
///
/// The result is cached per LUID for the life of the process: a GPU's encoder
/// does not change under a running driver, and a session costs milliseconds.
NvencCaps queryNvencCapabilities(uint64_t adapterLuid);

} // namespace mw::native::encode
