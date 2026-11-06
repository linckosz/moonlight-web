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

#include "native_test_framework.h"

#include "capture/windows/DisplayRotation.h"

using namespace mw::native::capture;

// A display whose desktop is turned against its panel (DisplayRotation.h). The
// first case is issue #54: a phone's 1080x2160 panel under a 2160x1080
// desktop, which QueryDisplayConfig's source mode reported as 1080x2160.

namespace {

// DXGI_MODE_ROTATION / DISPLAYCONFIG_ROTATION, spelled out so that the test
// needs no Windows header.
constexpr int kUnspecified = 0;
constexpr int kIdentity = 1;
constexpr int kRotate90 = 2;
constexpr int kRotate180 = 3;
constexpr int kRotate270 = 4;

struct Size
{
    int width;
    int height;
};

Size oriented(int width, int height, int rotation, int gdiWidth, int gdiHeight)
{
    desktopOrientedSize(width, height, rotation, gdiWidth, gdiHeight);
    return {width, height};
}

} // namespace

void run_display_rotation_tests()
{
    SECTION("Display rotation — the desktop's orientation, not the panel's");

    CHECK_EQ(rotationDegrees(kUnspecified), 0);
    CHECK_EQ(rotationDegrees(kIdentity), 0);
    CHECK_EQ(rotationDegrees(kRotate90), 90);
    CHECK_EQ(rotationDegrees(kRotate180), 180);
    CHECK_EQ(rotationDegrees(kRotate270), 270);
    CHECK_EQ(rotationDegrees(99), 0);

    CHECK(!rotationSwapsAxes(kIdentity));
    CHECK(rotationSwapsAxes(kRotate90));
    CHECK(!rotationSwapsAxes(kRotate180));
    CHECK(rotationSwapsAxes(kRotate270));

    // Issue #54: the source mode in the panel's orientation, GDI with the
    // desktop's. The desktop's wins.
    {
        const Size s = oriented(1080, 2160, kRotate90, 2160, 1080);
        CHECK_EQ(s.width, 2160);
        CHECK_EQ(s.height, 1080);
    }
    {
        const Size s = oriented(1080, 2160, kRotate270, 2160, 1080);
        CHECK_EQ(s.width, 2160);
        CHECK_EQ(s.height, 1080);
    }

    // No GDI size: the rotation alone turns the source mode.
    {
        const Size s = oriented(1080, 2160, kRotate90, 0, 0);
        CHECK_EQ(s.width, 2160);
        CHECK_EQ(s.height, 1080);
    }

    // A driver that reports the source mode already turned: GDI agrees with
    // it, so it is not turned a second time.
    {
        const Size s = oriented(1440, 2560, kRotate90, 1440, 2560);
        CHECK_EQ(s.width, 1440);
        CHECK_EQ(s.height, 2560);
    }

    // Upright, and turned half way: nothing moves.
    {
        const Size s = oriented(2560, 1440, kIdentity, 2560, 1440);
        CHECK_EQ(s.width, 2560);
        CHECK_EQ(s.height, 1440);
    }
    {
        const Size s = oriented(2560, 1440, kRotate180, 0, 0);
        CHECK_EQ(s.width, 2560);
        CHECK_EQ(s.height, 1440);
    }

    // GDI says another size altogether (the mode changed between the two
    // reads): the rotation decides.
    {
        const Size s = oriented(1080, 1920, kRotate90, 1280, 720);
        CHECK_EQ(s.width, 1920);
        CHECK_EQ(s.height, 1080);
    }

    // Nothing known: nothing invented.
    {
        const Size s = oriented(0, 0, kRotate90, 2160, 1080);
        CHECK_EQ(s.width, 0);
        CHECK_EQ(s.height, 0);
    }
}
