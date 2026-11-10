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

#include <cmath>

// A Mac display turned against its panel (issue #54, on macOS).
//
// System Settings turns a display a quarter, a half or three quarters of a
// turn (Displays → Rotation). ScreenCaptureKit captures the desktop as
// composed, upright, so the picture needs nothing. Its SIZE does: the session
// sizes the stream, the Selector's frame and the pointer's scale from the
// display mode's pixels, and on a turned display the documentation does not
// say whether CGDisplayModeGetPixelWidth/Height stand the panel's way or the
// desktop's. Read the panel's way, a portrait desktop is captured into a
// landscape frame, letterboxed.
//
// CGDisplayBounds is the desktop's own rectangle, turned with it, so its shape
// decides — as the desktop's rectangle decides on Windows (DisplayRotation.h's
// duplicationUprightSize) — under a quarter turn CGDisplayRotation confirms.
// A mode that already stands the desktop's way is left as it is, so whichever
// way macOS reads its modes, they come out the desktop's.
//
// Measured on macOS 15.6 (an M1's built-in panel turned 90° and 270°): the
// mode already reads the desktop's way, 2338 × 3600 pixels for bounds of
// 1169 × 1800 points, so nothing is swapped there. The swap guards a Mac or a
// release that reads it the panel's way.
//
// Pure, tested on every OS.

namespace mw::native::platform {

/// Whether a mode of @p modeWidth × @p modeHeight pixels reads the panel's way
/// on a display whose desktop is @p boundsWidth × @p boundsHeight points,
/// turned @p rotationDegrees (CGDisplayRotation): it must then be swapped to
/// stand the desktop's way. Only a quarter turn can make the two disagree; a
/// square mode has no way to read. Bounds with no shape (square, or none)
/// leave the turn to decide alone.
inline bool modeReadsPanelWay(int modeWidth, int modeHeight, double boundsWidth,
                              double boundsHeight, double rotationDegrees)
{
    const long quarters = ((std::lround(rotationDegrees / 90.0) % 4) + 4) % 4;
    if (quarters != 1 && quarters != 3) return false;
    if (modeWidth <= 0 || modeHeight <= 0 || modeWidth == modeHeight) return false;
    if (boundsWidth > 0 && boundsHeight > 0 && boundsWidth != boundsHeight)
        return (modeWidth > modeHeight) != (boundsWidth > boundsHeight);
    return true;
}

/// @p width × @p height stood the desktop's way, where it reads the panel's.
inline void uprightModeSize(int& width, int& height, double boundsWidth, double boundsHeight,
                            double rotationDegrees)
{
    if (!modeReadsPanelWay(width, height, boundsWidth, boundsHeight, rotationDegrees)) return;
    const int w = width;
    width = height;
    height = w;
}

} // namespace mw::native::platform
