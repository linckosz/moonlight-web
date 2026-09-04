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

#include "IWindowsCapture.h"

#include <cstddef>
#include <cstdint>

// Decoding a mouse pointer into something a shader can draw.
//
// Windows describes a cursor in three different encodings, and the difference
// between them is not cosmetic: read a masked-colour shape as a plain colour one
// and the pointer becomes a black square; read it as an inverter and a white
// page turns black under it. All three reduce to the same thing here — an RGBA
// image plus a per-pixel invert flag — which is what ColorConvert composites.
//
// This lives on its own because TWO capture backends need it and neither owns
// it. Desktop Duplication hands the encodings over directly; Windows.Graphics.
// Capture hands over no pointer at all, so the WGC path reads it from Win32
// (GetIconInfo) and packs it into the same three shapes. The bit twiddling below
// is subtle enough that having it twice would mean fixing it twice.

namespace mw::native::capture {

/// One pointer shape, in whichever of the three encodings Windows used.
///
/// Deliberately not a DXGI type: the Win32 path builds these too, and making it
/// speak DXGI's struct would be a fiction that reads as "this came from Desktop
/// Duplication".
struct ShapeSource
{
    enum class Encoding
    {
        /// Two 1-bit masks stacked in one image — AND on top, XOR below — so the
        /// real height is half the buffer's. This is the text I-beam and most
        /// resize arrows: no colour of their own, defined as inverting whatever
        /// is behind them.
        Monochrome,

        /// BGRA with real coverage in alpha. The ordinary modern cursor.
        Color,

        /// BGRA where alpha is a MASK, not coverage: 0 means "use this colour",
        /// 0xFF means "XOR this colour with the screen".
        MaskedColor,
    };

    Encoding encoding = Encoding::Color;

    /// Width, and the height of the RESULT — for a monochrome shape that is half
    /// the rows in `data`, and the caller has already halved it.
    int width = 0;
    int height = 0;

    /// Bytes per row in `data`.
    int pitch = 0;

    int hotspotX = 0;
    int hotspotY = 0;

    const uint8_t* data = nullptr;
    std::size_t size = 0;
};

/// Decode @p source into @p cursor's `pixels` / `invert` / ink bounds, and bump
/// its `shapeVersion`. The position fields are left alone: they come from
/// somewhere else and change far more often than the shape.
///
/// Does nothing when the source is degenerate, so a cursor that could not be
/// read leaves the previous shape in place rather than blanking the pointer.
void decodeCursorShape(const ShapeSource& source, CursorState& cursor);

} // namespace mw::native::capture
