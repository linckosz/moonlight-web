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

// The pointer a ScreenCast portal is asked for (core/PortalCursorMode.h): what
// the portal offers decides, and it is never asked for a mode it would refuse,
// which would refuse the stream with it.

#include "core/PortalCursorMode.h"
#include "native_test_framework.h"

#include <cstdint>
#include <initializer_list>

using namespace mw::native;

void run_portal_cursor_mode_tests()
{
    const uint32_t all = kPortalCursorHidden | kPortalCursorEmbedded | kPortalCursorMetadata;

    SECTION("Portal cursor — the pointer beside the pictures where the portal offers it, "
            "painted in where that is wanted");
    {
        // GNOME's and KDE's portals offer the three.
        CHECK_EQ(portalCursorMode(all, false), kPortalCursorMetadata);
        CHECK_EQ(portalCursorMode(kPortalCursorMetadata, false), kPortalCursorMetadata);
        // A GNOME virtual monitor before GNOME 48 wants it painted in.
        CHECK_EQ(portalCursorMode(all, true), kPortalCursorEmbedded);
        CHECK_EQ(portalCursorMode(kPortalCursorEmbedded, true), kPortalCursorEmbedded);
    }

    SECTION("Portal cursor — a portal without the pointer beside the pictures (wlroots: hidden "
            "and embedded) is asked for it painted in, never for what it refuses");
    {
        const uint32_t wlr = kPortalCursorHidden | kPortalCursorEmbedded;
        CHECK_EQ(portalCursorMode(wlr, false), kPortalCursorEmbedded);
        CHECK_EQ(portalCursorMode(kPortalCursorEmbedded, false), kPortalCursorEmbedded);
        CHECK_EQ(portalCursorMode(wlr, true), kPortalCursorEmbedded);
        // And the other way: painted in wanted, only beside them offered.
        CHECK_EQ(portalCursorMode(kPortalCursorMetadata, true), kPortalCursorMetadata);
        CHECK_EQ(portalCursorMode(kPortalCursorHidden | kPortalCursorMetadata, true),
                 kPortalCursorMetadata);
        // Hidden alone: a picture with no pointer rather than no picture.
        CHECK_EQ(portalCursorMode(kPortalCursorHidden, false), kPortalCursorHidden);
        CHECK_EQ(portalCursorMode(kPortalCursorHidden, true), kPortalCursorHidden);
        // Whatever it is asked, it is one the portal offered.
        for (uint32_t offered = 1; offered <= all; ++offered)
            for (bool embed : {false, true})
                CHECK((portalCursorMode(offered, embed) & offered) != 0);
    }

    SECTION("Portal cursor — a portal that does not say what it offers is asked as before");
    {
        CHECK_EQ(portalCursorMode(0, false), kPortalCursorMetadata);
        CHECK_EQ(portalCursorMode(0, true), kPortalCursorEmbedded);
        // Bits no portal defines are not modes: what is asked stays one of the three.
        CHECK_EQ(portalCursorMode(8 | 16, false), kPortalCursorMetadata);
        CHECK_EQ(portalCursorMode(8 | kPortalCursorEmbedded, false), kPortalCursorEmbedded);
    }
}
