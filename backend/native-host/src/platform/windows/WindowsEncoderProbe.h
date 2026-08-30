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
/// So codecs stay empty until the encoder backend that can genuinely ask lands.
/// An empty codec list makes the engine report itself unavailable, which is the
/// truthful answer: it cannot encode anything yet. Offering a host that fails
/// on click would be worse than offering none.
///
/// ── Why the vendor test is not enough, measured on a real machine ──────────
///
/// A bench here reports THREE physical GPUs (two RTX 5060 Ti and an AMD iGPU)
/// but FIVE DXGI adapters. The extra two are indirect-display adapters — a
/// Parsec Virtual Display and a Virtual Display Driver — and they present
/// NVIDIA's own name and device id (10de:2d04) under a LUID of their own.
///
/// They therefore pass the vendor test and get NVENC credited to them, while
/// no NVENC session can be created on them at all. There is no DXGI flag that
/// separates them (DXGI_ADAPTER_FLAG_SOFTWARE is not set), and picking them
/// apart by VRAM or by name would be a heuristic that is wrong the day a new
/// virtual display driver ships.
///
/// The fix is not a better guess: it is to stop guessing. When the encoder
/// backend lands it opens a session against each adapter, and an adapter that
/// cannot host one gets an empty codec list and is excluded by the availability
/// rule in core/Probe.cpp. Until then, what this function reports is a hint for
/// the log — never a decision.
void probeEncoders(GpuInfo& gpu);

} // namespace mw::native::platform
