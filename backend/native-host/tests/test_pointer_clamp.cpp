/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "input/linux/X11Pointer.h"

#include <cstdio>

using namespace mw::native::input;

// Bringing a stray pointer back onto the captured display.
//
// The X half of that (reading the pointer, warping it) needs an X server and is
// exercised by using the thing. This is the other half — the arithmetic that
// decides WHERE the pointer lands — and it is the half that fails quietly: a
// display taken as width/height instead of right/bottom parks the pointer in the
// wrong screen, and an off-by-one at the far edge parks it one pixel into the
// neighbour, from where the next swipe walks it straight back out.
//
// It is tested on every platform because it is plain integers, which is also why
// X11Pointer.h keeps it out of the class.

void run_pointer_clamp_tests()
{
    SECTION("Pointer clamp — bringing a stray pointer home");

    int x = -1;
    int y = -1;

    // A second display to the right of the captured one: the layout where this
    // whole thing matters, and 1920 is the first column that is no longer ours.
    CHECK(clampIntoRect(0, 0, 1920, 1080, 2500, 400, x, y));
    CHECK_EQ(x, 1919);
    CHECK_EQ(y, 400);

    // Already inside: nothing is moved, and nothing is claimed to be. A false
    // here that should be true is a pointer that never comes back; a true that
    // should be false is a warp on every mouse movement of every session.
    CHECK(!clampIntoRect(0, 0, 1920, 1080, 960, 540, x, y));
    CHECK(!clampIntoRect(0, 0, 1920, 1080, 0, 0, x, y));
    CHECK(!clampIntoRect(0, 0, 1920, 1080, 1919, 1079, x, y));

    // The far edges are exclusive: right and bottom name the first pixel that
    // belongs to the neighbour, exactly as DesktopRect does everywhere else.
    CHECK(clampIntoRect(0, 0, 1920, 1080, 1920, 1080, x, y));
    CHECK_EQ(x, 1919);
    CHECK_EQ(y, 1079);

    // A display that is not at the origin — the second monitor of the host,
    // captured. The origin is not a scale: an offset dropped here would send the
    // pointer to the top-left of the desktop instead of of the display.
    CHECK(clampIntoRect(1920, 0, 3840, 1080, 100, 500, x, y));
    CHECK_EQ(x, 1920);
    CHECK_EQ(y, 500);

    // Negative coordinates are ordinary: a monitor placed to the left of the
    // primary one has them, and X reports them as they are.
    CHECK(clampIntoRect(-1920, -200, 0, 880, 50, 60, x, y));
    CHECK_EQ(x, -1);
    CHECK_EQ(y, 60);
    CHECK(!clampIntoRect(-1920, -200, 0, 880, -1000, -100, x, y));

    // Both axes at once, and the corner it produces.
    CHECK(clampIntoRect(0, 0, 1920, 1080, -50, 4000, x, y));
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 1079);

    // No rectangle yet — before setDisplayRect, or a capture that reported
    // nothing. Warping to a rectangle of zero width would put the pointer at a
    // corner of the desktop and leave it there.
    CHECK(!clampIntoRect(0, 0, 0, 0, 500, 500, x, y));
    CHECK(!clampIntoRect(0, 0, 1920, 0, 500, 500, x, y));
    CHECK(!clampIntoRect(100, 100, 50, 400, 500, 500, x, y));
}
