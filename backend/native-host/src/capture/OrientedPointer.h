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

#include "CaptureTypes.h"
#include "Orientation.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

// The pointer of a display turned against its panel (issue #54), stood the
// way its picture is (Orientation.h).
//
// KMS keeps the pointer on a plane of its own, placed in the PANEL's pixels,
// and its image is turned for the panel too: Mutter, wlroots and an X server
// turn the sprite into the plane's buffer rather than ask the hardware to
// (where one does ask, the plane's own rotation says so, and is undone the
// same way). So both are turned back here: the image, and the plane's
// rectangle, which becomes a place on the upright picture.
//
// The image is the plane's whole buffer — a 64 × 64 or 256 × 256 square with
// the sprite in one corner. Turned back, the sprite lands in another corner,
// and where the capture does not know the hotspot (everywhere but a virtual
// machine's adapter) the image's top-left corner stands for it: the arrow's
// tip, on an upright display. So the empty columns and rows before the sprite
// are cut away. The arrow's tip is at the top-left again, and the image is
// placed that much further in, exactly where the compositor drew it.
//
// Pure, tested on every OS.

namespace mw::native::capture {

/// The pointer's image as its plane holds it.
struct HeldPointer
{
    int width = 0;
    int height = 0;
    /// width × height × 4, BGRA.
    std::vector<uint8_t> pixels;
    /// In these pixels; only meaningful when known.
    bool hotspotKnown = false;
    int hotspotX = 0;
    int hotspotY = 0;
};

/// What placeUprightPointer needs of the image, kept from standPointerUpright.
struct UprightPointerCut
{
    /// The plane's buffer turned upright, before the cut.
    int fullWidth = 0;
    int fullHeight = 0;
    /// The empty columns cut on the left, and rows at the top.
    int cutX = 0;
    int cutY = 0;
};

/// The image, once per shape: @p held stood upright from how @p image says the
/// plane's buffer holds the upright pointer, then cut to start at its ink.
/// Fills the shape of @p out — size, pixels, invert, ink, hotspot (0,0 when
/// unknown, as CursorState has it) — and leaves its version to the caller,
/// which knows whether the shape changed.
inline void standPointerUpright(const HeldPointer& held, Orientation image, CursorState& out,
                                UprightPointerCut& cut)
{
    cut = UprightPointerCut{};
    if (held.width <= 0 || held.height <= 0 ||
        held.pixels.size() < static_cast<size_t>(held.width) * held.height * 4) {
        out.width = out.height = out.inkWidth = out.inkHeight = 0;
        out.hotspotX = out.hotspotY = 0;
        out.pixels.clear();
        out.invert.clear();
        return;
    }
    // The buffer holds the upright pointer as `image` says, so the upright one
    // is the buffer's size, swapped by a quarter turn.
    int fullW = 0, fullH = 0;
    orientedSize(held.width, held.height, image, fullW, fullH);
    std::vector<uint8_t> full(static_cast<size_t>(fullW) * fullH * 4);
    uprightCopy32(held.pixels.data(), static_cast<size_t>(held.width) * 4, fullW, fullH, image,
                  full.data(), static_cast<size_t>(fullW) * 4, 0, fullH);

    int minX = fullW, minY = fullH, maxX = -1, maxY = -1;
    for (int y = 0; y < fullH; ++y)
        for (int x = 0; x < fullW; ++x)
            if (full[(static_cast<size_t>(y) * fullW + x) * 4 + 3] != 0) {
                minX = (std::min)(minX, x);
                minY = (std::min)(minY, y);
                maxX = (std::max)(maxX, x);
                maxY = (std::max)(maxY, y);
            }
    // No ink at all: nothing to cut against, the whole buffer stays.
    if (maxX < 0) minX = minY = 0;

    cut.fullWidth = fullW;
    cut.fullHeight = fullH;
    cut.cutX = minX;
    cut.cutY = minY;

    out.width = fullW - minX;
    out.height = fullH - minY;
    out.pixels.assign(static_cast<size_t>(out.width) * out.height * 4, 0);
    for (int y = 0; y < out.height; ++y)
        std::memcpy(&out.pixels[static_cast<size_t>(y) * out.width * 4],
                    &full[(static_cast<size_t>(y + minY) * fullW + minX) * 4],
                    static_cast<size_t>(out.width) * 4);
    out.invert.assign(static_cast<size_t>(out.width) * out.height, 0);
    out.inkWidth = maxX < 0 ? 0 : maxX - minX + 1;
    out.inkHeight = maxY < 0 ? 0 : maxY - minY + 1;
    out.hotspotX = out.hotspotY = 0;
    if (held.hotspotKnown) {
        int hx = 0, hy = 0;
        uprightPixel(held.hotspotX, held.hotspotY, fullW, fullH, image, hx, hy);
        out.hotspotX = hx - minX;
        out.hotspotY = hy - minY;
    }
}

/// The place, at every move: where the image stood upright by
/// standPointerUpright goes on the upright picture, @p width × @p height.
///
/// @param planeX/planeY where the plane's buffer starts on the panel (KMS:
///                      CRTC_X/Y, signed, possibly off the panel's edge)
/// @param panel         how the panel shows the upright desktop — the
///                      compositor's transform
inline void placeUprightPointer(int planeX, int planeY, const UprightPointerCut& cut,
                                Orientation panel, int width, int height, int& x, int& y)
{
    // What the panel shows of the plane is the upright image turned as the
    // panel turns everything.
    int shownW = 0, shownH = 0;
    orientedSize(cut.fullWidth, cut.fullHeight, panel, shownW, shownH);
    int ux = 0, uy = 0, uw = 0, uh = 0;
    uprightRect(planeX, planeY, shownW, shownH, width, height, panel, ux, uy, uw, uh);
    x = ux + cut.cutX;
    y = uy + cut.cutY;
}

} // namespace mw::native::capture
