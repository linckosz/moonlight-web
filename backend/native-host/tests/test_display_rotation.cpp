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

#include <vector>

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

    SECTION("Display rotation — Desktop Duplication's picture, stood upright");

    CHECK_EQ(quarterTurns(kUnspecified), 0);
    CHECK_EQ(quarterTurns(kIdentity), 0);
    CHECK_EQ(quarterTurns(kRotate90), 1);
    CHECK_EQ(quarterTurns(kRotate180), 2);
    CHECK_EQ(quarterTurns(kRotate270), 3);
    CHECK_EQ(quarterTurns(99), 0);

    // The duplication's mode, stood the way the desktop rectangle is. The
    // first report's was already (2160x1080 over a 1080x2160 panel), and
    // stays; one in the panel's orientation is turned. Its rectangle at 150 %
    // (DPI-virtualized) says the same: only its shape is read.
    {
        int w = 2160, h = 1080;
        duplicationUprightSize(w, h, kRotate270, 2160, 1080);
        CHECK_EQ(w, 2160);
        CHECK_EQ(h, 1080);
    }
    {
        int w = 1080, h = 2160;
        duplicationUprightSize(w, h, kRotate270, 1440, 720);
        CHECK_EQ(w, 2160);
        CHECK_EQ(h, 1080);
    }
    {
        int w = 1920, h = 1080;
        duplicationUprightSize(w, h, kRotate90, 1080, 1920);
        CHECK_EQ(w, 1080);
        CHECK_EQ(h, 1920);
    }
    // A half turn, an upright display, a square, no rectangle: as it came.
    {
        int w = 1920, h = 1080;
        duplicationUprightSize(w, h, kRotate180, 1080, 1920);
        CHECK_EQ(w, 1920);
        CHECK_EQ(h, 1080);
        duplicationUprightSize(w, h, kIdentity, 1080, 1920);
        CHECK_EQ(w, 1920);
        duplicationUprightSize(w, h, kRotate90, 1000, 1000);
        CHECK_EQ(w, 1920);
        duplicationUprightSize(w, h, kRotate90, 0, 0);
        CHECK_EQ(w, 1920);
        CHECK_EQ(h, 1080);
    }

    // Where the upright picture's pixels lie in the panel's texture: a
    // quarter turn puts the texture's top row down the picture's right side
    // (Microsoft's DesktopDuplication sample). A 4x2 picture, so the texture
    // is 2x4 for an odd turn.
    {
        const int w = 4, h = 2;
        int tx = -1, ty = -1;
        panelTexel(0, 0, w, h, 0, tx, ty);
        CHECK(tx == 0 && ty == 0);
        panelTexel(3, 0, w, h, 1, tx, ty); // top-right: the texture's top-left
        CHECK(tx == 0 && ty == 0);
        panelTexel(3, 1, w, h, 1, tx, ty); // bottom-right: the texture's top-right
        CHECK(tx == 1 && ty == 0);
        panelTexel(0, 0, w, h, 1, tx, ty); // top-left: the texture's bottom-left
        CHECK(tx == 0 && ty == 3);
        panelTexel(0, 0, w, h, 2, tx, ty); // upside down
        CHECK(tx == 3 && ty == 1);
        panelTexel(0, 0, w, h, 3, tx, ty); // top-left: the texture's top-right
        CHECK(tx == 1 && ty == 0);
        panelTexel(3, 1, w, h, 3, tx, ty); // bottom-right: the texture's bottom-left
        CHECK(tx == 0 && ty == 3);
    }

    // Every turn of a 6x4 picture: each pixel lands inside the texture, and
    // on a texel of its own; three quarters undo one quarter.
    for (int turns = 0; turns < 4; ++turns) {
        const int w = 6, h = 4;
        const int tw = (turns & 1) ? h : w;
        const int th = (turns & 1) ? w : h;
        bool inside = true;
        bool once = true;
        std::vector<int> seen(static_cast<size_t>(tw * th), 0);
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int tx = -1, ty = -1;
                panelTexel(x, y, w, h, turns, tx, ty);
                if (tx < 0 || ty < 0 || tx >= tw || ty >= th) {
                    inside = false;
                    continue;
                }
                once = once && ++seen[static_cast<size_t>(ty * tw + tx)] == 1;
            }
        }
        CHECK(inside);
        CHECK(once);
    }
    {
        // The texture of a quarter turn, h × w, read as a picture of its own
        // and turned three quarters, gives the picture back.
        const int w = 6, h = 4;
        bool back = true;
        for (int y = 0; y < h; ++y) {
            for (int x = 0; x < w; ++x) {
                int tx = 0, ty = 0;
                panelTexel(x, y, w, h, 1, tx, ty);
                // (tx, ty) in the h × w texture; a quarter turn of the
                // picture is three quarters of the texture's own frame.
                int bx = 0, by = 0;
                panelTexel(tx, ty, h, w, 3, bx, by);
                back = back && bx == x && by == y;
            }
        }
        CHECK(back);
    }
}
