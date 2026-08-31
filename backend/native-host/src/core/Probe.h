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

// The platform seam for capability detection.
//
// Each OS backend implements these three; core/Probe.cpp owns the order they
// are asked in and the rules for combining their answers, so the "why is the
// engine unavailable" logic exists exactly once instead of three times.

namespace mw::native {

/// The full capability probe: asks the platform seam below in a fixed order and
/// combines the answers. NativeHost::probe() forwards to this; platform code
/// calls it directly when it needs a fresh view of the machine.
Capabilities probe();

} // namespace mw::native

namespace mw::native::platform {

/// Whether this process can reach an interactive desktop at all.
///
/// This is the check that Windows makes unavoidable: MoonlightWeb normally runs
/// as an NSSM service in session 0, where neither DXGI Desktop Duplication nor
/// SendInput work — so a native session has to be launched into the console
/// session instead. Answering it here, before anything else is attempted, is
/// what turns a confusing cascade of API failures into one clear verdict.
///
/// Non-Windows platforms answer from their own display-server presence.
bool hasInteractiveSession();

/// Whether the OS is recent enough for the APIs this engine requires:
/// Windows 10 2004+, macOS 12.3+, or Linux with PipeWire 0.3 and a ScreenCast
/// portal.
bool isOsSupported();

/// Enumerate GPUs and displays, and work out which GPU drives which display
/// (§12 — the association that decides whether the pipeline is zero-copy).
///
/// Fills `caps.gpus`, `caps.displays` and `caps.capture`. Returns the reason on
/// failure, or Unavailability::None on success. Must not throw.
Unavailability enumerate(Capabilities& caps);

} // namespace mw::native::platform
