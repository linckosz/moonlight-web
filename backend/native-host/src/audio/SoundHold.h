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

namespace mw::native::audio {

/// A frame of sound about to be encoded, while the desktop gate holds the
/// stream back (a TV on plain HTTP, core/DesktopGate.h): digital silence
/// instead. The track goes on at its clock, a packet every frame, so nothing
/// is renegotiated and the sound comes back with the picture; nothing of what
/// the host played goes out meanwhile. True when it held the frame.
inline bool holdFrame(float* frame, size_t floats, bool held)
{
    if (!held) return false;
    std::fill(frame, frame + floats, 0.0f);
    return true;
}

} // namespace mw::native::audio
