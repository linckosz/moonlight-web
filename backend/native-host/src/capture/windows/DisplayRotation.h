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

// A display whose desktop is turned against its panel (issue #54).
//
// Windows lets the desktop of a display be turned a quarter, a half or three
// quarters of a turn from the way its panel scans out: a monitor stood on its
// side, a tablet, a phone running Windows whose panel is portrait while the
// desktop is landscape. Two things about such a display are told in the
// PANEL's orientation, not the desktop's, and both broke the stream of the
// first report (a Mi Mix 2S, panel 1080x2160, desktop 2160x1080):
//
//  - QueryDisplayConfig's source mode said 1080x2160, so the session was
//    sized for a portrait display and the landscape desktop arrived
//    letterboxed into a quarter of it;
//  - Desktop Duplication hands back the picture as the panel scans it out,
//    and leaves turning it to the caller (DXGI_OUTDUPL_DESC::Rotation), so
//    what arrived was on its side.
//
// Windows.Graphics.Capture has neither problem: it captures the desktop as
// composed, upright. So a turned display is sized from the desktop's own
// orientation here, and Desktop Duplication refuses it — the session takes
// WGC, and comes back to the duplication when the display is turned upright.
//
// DXGI_MODE_ROTATION and DISPLAYCONFIG_ROTATION number the turns the same way
// (1 none, 2 a quarter, 3 a half, 4 three quarters; DXGI adds 0, unspecified),
// so one function reads both.
//
// Pure, no Windows: tested on every OS, like OutputMatch.h.

namespace mw::native::capture {

/// The turn of a display's desktop against its panel, in degrees clockwise —
/// 0, 90, 180 or 270 — from a DXGI_MODE_ROTATION or DISPLAYCONFIG_ROTATION
/// value. Anything unknown counts as upright, as Windows' own "unspecified".
inline int rotationDegrees(int windowsRotation)
{
    switch (windowsRotation) {
    case 2: return 90;
    case 3: return 180;
    case 4: return 270;
    default: return 0;
    }
}

/// Whether the turn swaps width and height.
inline bool rotationSwapsAxes(int windowsRotation)
{
    const int degrees = rotationDegrees(windowsRotation);
    return degrees == 90 || degrees == 270;
}

/// The size of a display's desktop, from QueryDisplayConfig's source mode and
/// the path's rotation.
///
/// The source mode is read in the panel's orientation (the first report), so
/// a quarter turn swaps it. EnumDisplaySettings' size, when Windows gave one,
/// is the desktop's own and settles it: whichever orientation of the source
/// mode it matches is the one taken, so that a driver reporting the source
/// mode already turned is not turned twice. gdiWidth/gdiHeight are 0 when
/// EnumDisplaySettings said nothing.
inline void desktopOrientedSize(int& width, int& height, int windowsRotation, int gdiWidth,
                                int gdiHeight)
{
    if (width <= 0 || height <= 0 || width == height) return;
    bool swap = rotationSwapsAxes(windowsRotation);
    if (gdiWidth > 0 && gdiHeight > 0) {
        if (gdiWidth == width && gdiHeight == height)
            swap = false;
        else if (gdiWidth == height && gdiHeight == width)
            swap = true;
        // Neither: the two disagree on the size itself (a mode changing
        // between the two reads). The rotation alone decides.
    }
    if (swap) {
        const int w = width;
        width = height;
        height = w;
    }
}

} // namespace mw::native::capture
