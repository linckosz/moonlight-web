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

namespace mw::native::convert {

/// How the pointer is placed on the picture, beyond the position and shape the
/// capture already reports.
///
/// Separate from CursorState because none of it comes from the capture: the
/// magnification is a client request, and the hotspot — which CursorState has
/// already subtracted out of its position — has to come back for the pointer to
/// grow around the point it aims with rather than around its top-left corner.
///
/// Shared by every platform's converter: the compositing is the same shader
/// math on D3D11 and on GLES, and this is its one input that is not a texture.
struct CursorDraw
{
    /// Multiplier on the drawn size. 1 is the size the pointer has on the
    /// desktop, which is what everything but a small screen wants.
    float magnify = 1.0f;

    /// The point inside the shape that IS the pointer position, in cursor
    /// pixels. Only consulted when magnifying: it is the fixed point of the
    /// growth, so the tip of an enlarged arrow stays exactly where the real one
    /// was.
    int hotspotX = 0;
    int hotspotY = 0;
};

} // namespace mw::native::convert
