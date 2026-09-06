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

// The displays as macOS lists them, in the one order both the probe and the
// session read them in.
//
// The Selector hands a session the GPU (its Metal registry id) and the
// display's index among that GPU's displays — the same contract DXGI's
// "output index within its adapter" fills on Windows. The probe numbers the
// displays from this list; the session resolves the index back from the same
// list, seconds later, in the same process. CGGetActiveDisplayList is stable
// over that span, and a display that vanished in between is a start() error
// with a plain sentence, not a wrong screen.

namespace mw::native::platform {

struct MacDisplay
{
    uint32_t displayId = 0;     ///< CGDirectDisplayID — what ScreenCaptureKit wants back
    uint64_t gpuRegistryId = 0; ///< the Metal device that scans it out
    std::string gpuName;
    std::string name; ///< "Built-in Retina Display", "LG UltraFine"…
    int pixelWidth = 0;
    int pixelHeight = 0;
    int refreshMilliHz = 0;
    bool isMain = false;
    /// The panel is dark (display sleep). Still online, still capturable
    /// once woken — which a session does.
    bool isAsleep = false;
    /// How far above SDR white the panel can go — Extended Dynamic Range,
    /// which is what macOS calls HDR. 1.0 is an SDR panel; a Liquid Retina
    /// XDR answers 16. There is no HDR switch on macOS: a panel that has
    /// headroom always has it, and the compositor uses it whenever content
    /// asks. `hdr` is headroom > 1.
    double edrHeadroom = 1.0;
    bool hdr = false;
    /// The display's rectangle in POINTS on the global desktop — the space
    /// CGEvent positions live in.
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
};

/// Every active display, in the OS's order.
std::vector<MacDisplay> listDisplays();

} // namespace mw::native::platform
