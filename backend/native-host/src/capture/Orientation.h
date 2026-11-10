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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

// A picture that does not lie the way it is seen (issue #54, Linux and macOS).
//
// A display can be turned against its panel: a monitor stood on its side, a
// tablet, a handheld whose panel is mounted portrait. The compositor then puts
// the desktop on the panel turned, and a capture that reads what the panel
// scans out rather than the desktop as composed gets the picture turned too,
// with the pointer. KMS reads the scanout (KmsCapture). A portal on wlroots
// (xdg-desktop-portal-wlr, Hyprland's) hands over the output's buffer as it is
// and says how it lies in a buffer of metadata (PortalCapture).
//
// Every source says it the same way, which is why there is one type for all.
// wl_output's transform (Mutter's is the same), RandR's rotation, the DRM
// plane's "rotation" property and PipeWire's video transform all mean: the
// picture mirrored left to right first, when it is, then turned that many
// quarters counter-clockwise. An Orientation is that pair, read as what was
// done to the upright picture to make what is held. The converters stand the
// picture upright as they read it (orientedTexel, in texels and in uv).
// Everything else — the session's size, the pointer, the mouse — is in the
// upright picture.
//
// Windows numbers its turns its own way (DisplayRotation.h: one direction, no
// mirror), and its quarter turn is this one's: panelTexel and orientedTexel
// agree.
//
// Pure, no platform: tested on every OS.

namespace mw::native::capture {

struct Orientation
{
    /// Quarter turns counter-clockwise, 0 to 3.
    int turns = 0;
    /// Mirrored left to right, before the turn.
    bool mirrored = false;

    bool upright() const { return (turns & 3) == 0 && !mirrored; }
    /// A quarter or three quarters of a turn: width and height swap.
    bool swapsAxes() const { return (turns & 1) != 0; }
    /// For the log: 0, 90, 180 or 270.
    int degrees() const { return (turns & 3) * 90; }

    bool operator==(const Orientation& other) const
    {
        return (turns & 3) == (other.turns & 3) && mirrored == other.mirrored;
    }
    bool operator!=(const Orientation& other) const { return !(*this == other); }
};

/// wl_output's transform, which Mutter's and PipeWire's video transform
/// number the same way: 0 none, 1 to 3 a quarter, a half and three quarters
/// counter-clockwise, 4 to 7 the same after a mirror. Anything else: upright.
inline Orientation orientationFromWayland(uint32_t transform)
{
    if (transform > 7) return {};
    return Orientation{static_cast<int>(transform & 3), transform >= 4};
}

/// @p second done after @p first.
///
/// A mirror reverses the direction of a turn made before it: mirroring a
/// picture turned a quarter one way is mirroring it, then turning it the
/// quarter the other way.
inline Orientation compose(Orientation second, Orientation first)
{
    const int turns = second.mirrored ? (second.turns - first.turns) : (second.turns + first.turns);
    return Orientation{((turns % 4) + 4) % 4, second.mirrored != first.mirrored};
}

/// What undoes @p o. A mirrored orientation undoes itself.
inline Orientation inverse(Orientation o)
{
    if (o.mirrored) return Orientation{o.turns & 3, true};
    return Orientation{(4 - (o.turns & 3)) & 3, false};
}

/// A DRM plane's "rotation" property: rotate-0, -90, -180 and -270 are bits 0
/// to 3, reflect-x and reflect-y bits 4 and 5. The kernel reflects first, then
/// turns counter-clockwise, from the plane's buffer to what the screen shows
/// (drm_rect_rotate). reflect-y is a mirror and a half turn; both together, a
/// half turn. A value with no turn, or more than one, says nothing: upright.
inline Orientation orientationFromDrmRotation(uint64_t value)
{
    int turns = -1;
    for (int i = 0; i < 4; ++i) {
        if ((value & (uint64_t{1} << i)) == 0) continue;
        if (turns >= 0) return {};
        turns = i;
    }
    if (turns < 0) return {};
    const bool x = (value & (uint64_t{1} << 4)) != 0;
    const bool y = (value & (uint64_t{1} << 5)) != 0;
    Orientation reflect;
    if (x && y)
        reflect = Orientation{2, false};
    else if (x)
        reflect = Orientation{0, true};
    else if (y)
        reflect = Orientation{2, true};
    return compose(Orientation{turns, false}, reflect);
}

/// RandR's rotation of a CRTC: RR_Rotate_0, _90, _180 and _270 are 1, 2, 4 and
/// 8, the picture turned counter-clockwise on the panel (xrandr's "left" is
/// 90). Its reflections, 16 and 32, are not taken: the order RandR applies
/// them in has not been checked against a screen, and a mirrored X screen is
/// rare enough to stay mirrored. @p reflected says one was there, for the log.
inline Orientation orientationFromRandr(unsigned rotation, bool& reflected)
{
    reflected = (rotation & (16u | 32u)) != 0;
    switch (rotation & 15u) {
    case 2: return Orientation{1, false};
    case 4: return Orientation{2, false};
    case 8: return Orientation{3, false};
    default: return {};
    }
}

/// The size a @p width × @p height upright picture is held at: height × width
/// for a quarter turn.
inline void orientedSize(int width, int height, Orientation o, int& heldWidth, int& heldHeight)
{
    heldWidth = o.swapsAxes() ? height : width;
    heldHeight = o.swapsAxes() ? width : height;
}

/// Where pixel (@p x, @p y) of the upright @p width × @p height picture is
/// held. A quarter turn puts the held picture's top row down the upright
/// one's right side — DisplayRotation.h's panelTexel, which Microsoft's
/// DesktopDuplication sample and OBS draw with.
inline void orientedTexel(int x, int y, int width, int height, Orientation o, int& tx, int& ty)
{
    if (o.mirrored) x = width - 1 - x;
    switch (o.turns & 3) {
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

/// The other way: the upright pixel that held texel (@p tx, @p ty) shows, the
/// upright picture being @p width × @p height. Outside the picture too — a
/// pointer half off an edge is mapped the same way.
inline void uprightPixel(int tx, int ty, int width, int height, Orientation o, int& x, int& y)
{
    switch (o.turns & 3) {
    case 1:
        x = width - 1 - ty;
        y = tx;
        break;
    case 2:
        x = width - 1 - tx;
        y = height - 1 - ty;
        break;
    case 3:
        x = ty;
        y = height - 1 - tx;
        break;
    default:
        x = tx;
        y = ty;
        break;
    }
    if (o.mirrored) x = width - 1 - x;
}

/// The upright rectangle that a @p w × @p h rectangle at (@p x, @p y) of the
/// held picture covers, the upright picture being @p width × @p height.
inline void uprightRect(int x, int y, int w, int h, int width, int height, Orientation o, int& ux,
                        int& uy, int& uw, int& uh)
{
    int ax = 0, ay = 0, bx = 0, by = 0;
    uprightPixel(x, y, width, height, o, ax, ay);
    uprightPixel(x + w - 1, y + h - 1, width, height, o, bx, by);
    ux = (std::min)(ax, bx);
    uy = (std::min)(ay, by);
    orientedSize(w, h, o, uw, uh);
}

/// Rows [@p rowBegin, @p rowEnd) of the upright @p width × @p height picture,
/// four bytes a pixel, out of where it is held as @p o says: @p src the held
/// picture's first byte, @p srcPitch its row length in bytes. One read and one
/// write a pixel; for an upright orientation, one copy a row.
inline void uprightCopy32(const uint8_t* src, size_t srcPitch, int width, int height, Orientation o,
                          uint8_t* dst, size_t dstPitch, int rowBegin, int rowEnd)
{
    if (width <= 0 || height <= 0) return;
    rowBegin = (std::max)(rowBegin, 0);
    rowEnd = (std::min)(rowEnd, height);
    for (int y = rowBegin; y < rowEnd; ++y) {
        uint8_t* out = dst + static_cast<size_t>(y) * dstPitch;
        int tx = 0, ty = 0;
        orientedTexel(0, y, width, height, o, tx, ty);
        if (o.upright()) {
            std::memcpy(out, src + static_cast<size_t>(ty) * srcPitch + static_cast<size_t>(tx) * 4,
                        static_cast<size_t>(width) * 4);
            continue;
        }
        // Along an upright row the held texel moves by one whole step, the
        // same for every pixel of the row: a column (a row of the held
        // picture's height) for a quarter turn, a pixel for a half.
        int nx = tx, ny = ty;
        if (width > 1) orientedTexel(1, y, width, height, o, nx, ny);
        // Offsets, not a walking pointer: one step past the last pixel may lie
        // before the buffer's first byte, which a pointer may not even name.
        const ptrdiff_t pitch = static_cast<ptrdiff_t>(srcPitch);
        const ptrdiff_t step = static_cast<ptrdiff_t>(ny - ty) * pitch + (nx - tx) * 4;
        const ptrdiff_t first = static_cast<ptrdiff_t>(ty) * pitch + static_cast<ptrdiff_t>(tx) * 4;
        for (int x = 0; x < width; ++x)
            std::memcpy(out + static_cast<size_t>(x) * 4, src + (first + x * step), 4);
    }
}

} // namespace mw::native::capture
