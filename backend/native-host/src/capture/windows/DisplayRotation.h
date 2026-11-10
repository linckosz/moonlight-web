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
// So a turned display is sized from the desktop's own orientation here, and
// the conversion turns the duplication's picture upright as it reads it — the
// mapping of panelTexel() below, which the shaders of ConvertHlsl.h make in
// texels and in uv — as Sunshine and OBS do. Everything else goes by the
// desktop, upright: the session's size, the pointer, the mouse.
//
// The first fix had Desktop Duplication refuse a turned display and left it
// to Windows.Graphics.Capture, which captures the desktop as composed,
// upright. It held on the bench and left the installed host with no picture
// at all: its worker runs as SYSTEM, for the secure desktop, and WGC does not
// exist for SYSTEM (0x80070424, backend/src/streaming/CaptureKey.h) — the
// report came back on 10/11/2026.
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

/// The same turn in quarter turns, 0 to 3: what the converters take
/// (IWindowsCapture::turn).
inline int quarterTurns(int windowsRotation)
{
    return rotationDegrees(windowsRotation) / 90;
}

/// Whether the turn swaps width and height.
inline bool rotationSwapsAxes(int windowsRotation)
{
    const int degrees = rotationDegrees(windowsRotation);
    return degrees == 90 || degrees == 270;
}

/// Where pixel (@p x, @p y) of the upright picture lies in the texture Desktop
/// Duplication hands over for a display turned @p turns quarter turns, the
/// upright picture being @p width × @p height — so the texture is height ×
/// width for an odd turn. The mapping of Microsoft's DesktopDuplication
/// sample (SetDirtyVert, SetMoveRect), by which OBS's monitor capture draws
/// too: a quarter turn puts the texture's top row down the picture's right
/// side.
inline void panelTexel(int x, int y, int width, int height, int turns, int& tx, int& ty)
{
    switch (turns & 3) {
    case 1:
        tx = y;
        ty = width - 1 - x;
        break;
    case 2:
        tx = width - 1 - x;
        ty = height - 1 - y;
        break;
    case 3:
        tx = height - 1 - y;
        ty = x;
        break;
    default:
        tx = x;
        ty = y;
        break;
    }
}

/// Desktop Duplication's mode size (DXGI_OUTDUPL_DESC::ModeDesc), stood the
/// way the desktop is. The first report's mode was the desktop's already
/// (2160x1080 over a 1080x2160 panel), and the docs promise nothing either
/// way, so the desktop's rectangle (DXGI_OUTPUT_DESC::DesktopCoordinates)
/// decides: its shape survives the DPI virtualization its size does not.
/// Only a quarter turn can disagree; a square, or no rectangle, says nothing.
inline void duplicationUprightSize(int& width, int& height, int windowsRotation, int desktopWidth,
                                   int desktopHeight)
{
    if (!rotationSwapsAxes(windowsRotation)) return;
    if (width <= 0 || height <= 0 || width == height) return;
    if (desktopWidth <= 0 || desktopHeight <= 0 || desktopWidth == desktopHeight) return;
    if ((width > height) != (desktopWidth > desktopHeight)) {
        const int w = width;
        width = height;
        height = w;
    }
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
