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

// The pointer a ScreenCast portal is asked for (PortalScreenCast's
// SelectSources), out of the ones it offers. Pure, so that it is tested
// everywhere.

#include <cstdint>

namespace mw::native {

/// The portal's cursor modes, as its AvailableCursorModes bitmask and
/// SelectSources' cursor_mode number them.
inline constexpr uint32_t kPortalCursorHidden = 1;
inline constexpr uint32_t kPortalCursorEmbedded = 2;
inline constexpr uint32_t kPortalCursorMetadata = 4;

/// The cursor_mode to ask for, @p offered being the portal's
/// AvailableCursorModes (0 when it could not be read).
///
/// METADATA, the pointer beside the pictures, which lets the client draw its
/// own; EMBEDDED, painted into them, when that is what is wanted (@p embed).
/// A portal refuses the whole request for a mode it does not offer, and the
/// stream never starts: the wlroots one (xdg-desktop-portal-wlr 0.7) offers
/// hidden and embedded only. So the one wanted when it is offered, else the
/// other of the two that show a pointer, else none at all. A portal that says
/// nothing is asked for the one wanted, as before.
inline uint32_t portalCursorMode(uint32_t offered, bool embed)
{
    const uint32_t wanted = embed ? kPortalCursorEmbedded : kPortalCursorMetadata;
    const uint32_t other = embed ? kPortalCursorMetadata : kPortalCursorEmbedded;
    if (offered == 0 || (offered & wanted) != 0) return wanted;
    if ((offered & other) != 0) return other;
    if ((offered & kPortalCursorHidden) != 0) return kPortalCursorHidden;
    return wanted;
}

} // namespace mw::native
