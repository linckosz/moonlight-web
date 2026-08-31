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

namespace mw::native::platform {

/// Fill @p gpu.encoders and @p gpu.codecs with what this adapter can really do.
///
/// Two different strengths of evidence, and they are kept apart on purpose:
///
///  - **Which encoder API is present** can be established cheaply and safely:
///    the vendor runtime ships with the driver, so finding nvEncodeAPI64.dll
///    (and its entry point) alongside an NVIDIA adapter is solid.
///
///  - **Which codecs it can produce** cannot. That needs a real encoder session
///    against this specific adapter — a GTX 1050 and an RTX 4070 both answer to
///    the same DLL and differ on AV1. Guessing from the device id would be a
///    table to maintain forever and wrong on the day a new chip ships.
///
/// So a codec list is only ever filled by the vendor API answering. NVIDIA is
/// asked for real (NvencCapabilities opens a session); AMD and Intel are not
/// yet, so they report a runtime but no codec — which leaves them unusable by
/// the availability rule in core/Probe.cpp. That is the intended behaviour:
/// offering a host that fails the moment it is clicked is worse than offering
/// none.
///
/// ── What the bench taught, and what it corrected ───────────────────────────
///
/// A machine here reports THREE physical GPUs (two RTX 5060 Ti and an AMD iGPU)
/// but FIVE DXGI adapters. The extra two are indirect-display adapters — a
/// Parsec Virtual Display and a Virtual Display Driver — presenting NVIDIA's
/// own name and device id (10de:2d04) under a LUID of their own.
///
/// The guess was that they would refuse an encode session and disqualify
/// themselves. They do not: being backed by a real NVIDIA GPU, they open a
/// session and report the same codecs. Which is the correct outcome — a display
/// on such an adapter should be captured AND encoded there, since DXGI routes
/// both to the same silicon.
///
/// The lesson is narrower than "filter the fakes": it is that the vendor id
/// picks WHICH API to ask, and only the API's own answer says what a codec list
/// contains. NVIDIA does that now. AMD and Intel do not yet, so they claim no
/// codec at all and stay unusable rather than promising something unverified.
void probeEncoders(GpuInfo& gpu);

} // namespace mw::native::platform
