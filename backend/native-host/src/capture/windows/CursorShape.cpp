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

#include "CursorShape.h"

#include "../../core/Log.h"

#include <string>

namespace mw::native::capture {

void decodeCursorShape(const ShapeSource& source, CursorState& cursor)
{
    const int width = source.width;
    const int height = source.height;
    const int pitch = source.pitch;
    if (width <= 0 || height <= 0 || pitch <= 0 || !source.data) return;

    cursor.width = width;
    cursor.height = height;
    cursor.pixels.assign(static_cast<size_t>(width) * height * 4, 0);
    cursor.invert.assign(static_cast<size_t>(width) * height, 0);
    ++cursor.shapeVersion;

    // Rightmost column and lowest row holding ink, +1. See CursorState::inkWidth.
    int inkWidth = 0;
    int inkHeight = 0;
    const auto noteInk = [&inkWidth, &inkHeight](int x, int y) {
        if (x + 1 > inkWidth) inkWidth = x + 1;
        if (y + 1 > inkHeight) inkHeight = y + 1;
    };

    const uint8_t* data = source.data;
    const size_t size = source.size;
    const bool monochrome = source.encoding == ShapeSource::Encoding::Monochrome;

    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const size_t out = (static_cast<size_t>(y) * width + x);
            uint8_t* px = &cursor.pixels[out * 4];

            if (monochrome) {
                // Bit per pixel, most significant bit first. The AND mask
                // selects between "the XOR bit is a colour" and "the XOR bit
                // says whether to invert".
                const size_t andOffset = static_cast<size_t>(y) * pitch + (x / 8);
                const size_t xorOffset = static_cast<size_t>(y + height) * pitch + (x / 8);
                if (xorOffset >= size) continue;
                const uint8_t mask = static_cast<uint8_t>(0x80 >> (x % 8));
                const bool andBit = (data[andOffset] & mask) != 0;
                const bool xorBit = (data[xorOffset] & mask) != 0;

                if (!andBit) {
                    // Opaque: black where the XOR bit is clear, white where set.
                    const uint8_t v = xorBit ? 0xFF : 0x00;
                    px[0] = px[1] = px[2] = v;
                    px[3] = 0xFF;
                    noteInk(x, y);
                } else if (xorBit) {
                    cursor.invert[out] = 0xFF;
                    px[3] = 0xFF;
                    noteInk(x, y);
                }
                // andBit && !xorBit → transparent, already zeroed.
                continue;
            }

            const size_t in = static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * 4;
            if (in + 3 >= size) continue;

            if (source.encoding == ShapeSource::Encoding::MaskedColor) {
                // Here the alpha byte is not coverage but a mask: 0 means "use
                // this colour", 0xFF means "XOR this colour with the screen".
                //
                // XOR, not invert — the distinction is the whole shape. The
                // empty canvas around the pointer is encoded as mask 0xFF with a
                // BLACK colour: XOR with zero leaves the screen alone, so those
                // pixels are transparent. Reading them as "invert" turned the
                // whole 32×32 canvas into an inverter, which on a white page is
                // a black square with a ghost of the pointer inside. A white
                // colour under the mask is a true inversion; other colours are
                // rare enough that inverting is the nearest thing we draw.
                if (data[in + 3] == 0xFF) {
                    if ((data[in + 0] | data[in + 1] | data[in + 2]) == 0) continue;
                    cursor.invert[out] = 0xFF;
                    px[3] = 0xFF;
                } else {
                    px[0] = data[in + 0];
                    px[1] = data[in + 1];
                    px[2] = data[in + 2];
                    px[3] = 0xFF;
                }
                noteInk(x, y);
                continue;
            }

            // Plain colour: BGRA with real coverage in alpha.
            px[0] = data[in + 0];
            px[1] = data[in + 1];
            px[2] = data[in + 2];
            px[3] = data[in + 3];
            if (px[3] != 0) noteInk(x, y);
        }
    }

    cursor.inkWidth = inkWidth;
    cursor.inkHeight = inkHeight;

    // Debug only: rare (a shape lasts thousands of frames) but pure diagnosis.
    // It is the one line that explains a pointer drawn at the wrong size or as
    // a black square: the encoding, the canvas, and how much of the canvas is
    // actually pointer.
    if (!log::enabled(log::Level::Debug)) return;
    log::debug("[native] cursor shape: " +
               std::string(monochrome                                              ? "monochrome"
                           : source.encoding == ShapeSource::Encoding::MaskedColor ? "masked-color"
                                                                                   : "color") +
               " " + std::to_string(width) + "x" + std::to_string(height) + ", ink " +
               std::to_string(inkWidth) + "x" + std::to_string(inkHeight) + ", hotspot " +
               std::to_string(source.hotspotX) + "," + std::to_string(source.hotspotY));
}

} // namespace mw::native::capture
