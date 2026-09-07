/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "input/linux/X11Pointer.h"

#include <cstdio>

using namespace mw::native::input;

// Placing an absolute mouse position on a Linux host.
//
// uinput's absolute pointer reports a fraction of a fixed 0..32767 axis, and the
// compositor stretches that fraction over the WHOLE desktop — the way a graphics
// tablet covers the whole desk, not one screen. So a position has to travel
// through two mappings: onto the captured display (which is where the client's
// picture actually is), then onto the desktop (which is what the device's range
// means). Dropping the second one is what this pair of functions exists to
// prevent: it aims every display as if it were the entire desktop, which is
// invisible on a host with one monitor and wrong on every other.
//
// Both halves are plain integers, tested on every platform for the same reason
// clampIntoRect is — the failures are quiet ones. An origin left out parks the
// pointer on the wrong screen; an off-by-one at the far edge makes the last
// column unreachable, which is exactly the column a viewer aims at to reach the
// screen next door.

void run_absolute_map_tests()
{
    SECTION("Absolute map — a client position onto the right screen");

    int x = -1;
    int y = -1;

    // ── displayPointToDesktop: the client's picture onto the display ─────────

    // The ordinary case: a 960x540 picture on a 1920x1080 primary display.
    // Halfway across the picture is halfway across the screen.
    CHECK(displayPointToDesktop(0, 0, 1920, 1080, 480, 270, 960, 540, x, y));
    CHECK_EQ(x, 960);
    CHECK_EQ(y, 540);

    // The same picture, but the captured display is the SECOND monitor. This is
    // the whole bug: without the origin, the centre of the picture lands at 960
    // — the middle of the primary — instead of the middle of the display the
    // viewer is actually looking at.
    CHECK(displayPointToDesktop(1920, 0, 3840, 1080, 480, 270, 960, 540, x, y));
    CHECK_EQ(x, 2880);
    CHECK_EQ(y, 540);

    // The origin of the picture is the origin of the display, not of the desktop.
    CHECK(displayPointToDesktop(1920, 0, 3840, 1080, 0, 0, 960, 540, x, y));
    CHECK_EQ(x, 1920);
    CHECK_EQ(y, 0);

    // A monitor placed to the LEFT of the primary one has negative coordinates,
    // and they are ordinary.
    CHECK(displayPointToDesktop(-1920, -200, 0, 880, 480, 270, 960, 540, x, y));
    CHECK_EQ(x, -960);
    CHECK_EQ(y, 340);

    // The far edge is exclusive, and reachable: a client that reports the very
    // last column of its picture must land on the last column of the display,
    // never one pixel into the neighbour.
    CHECK(displayPointToDesktop(0, 0, 1920, 1080, 960, 540, 960, 540, x, y));
    CHECK_EQ(x, 1919);
    CHECK_EQ(y, 1079);

    // A client whose aspect ratio differs by a pixel reports slightly outside
    // its own picture. It gets clamped into the display rather than walking onto
    // the screen next door — the same guarantee Win32Input::toAbsolute gives.
    CHECK(displayPointToDesktop(0, 0, 1920, 1080, 980, 560, 960, 540, x, y));
    CHECK_EQ(x, 1919);
    CHECK_EQ(y, 1079);
    CHECK(displayPointToDesktop(0, 0, 1920, 1080, -5, -5, 960, 540, x, y));
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 0);

    // Nothing to map against: no rectangle yet, or a degenerate reference
    // surface. False, so the caller can fall back rather than divide by zero.
    CHECK(!displayPointToDesktop(0, 0, 0, 0, 10, 10, 960, 540, x, y));
    CHECK(!displayPointToDesktop(0, 0, 1920, 1080, 10, 10, 0, 540, x, y));
    CHECK(!displayPointToDesktop(0, 0, 1920, 1080, 10, 10, 960, 0, x, y));

    // ── desktopToAbsoluteRange: the desktop onto the device's axis ───────────

    // A single-monitor host: the display IS the desktop, and the two ends of the
    // screen are the two ends of the device's range.
    CHECK(desktopToAbsoluteRange(0, 0, 1920, 1080, 0, 0, 32767, x, y));
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 0);
    CHECK(desktopToAbsoluteRange(0, 0, 1920, 1080, 1919, 1079, 32767, x, y));
    CHECK_EQ(x, 32767);
    CHECK_EQ(y, 32767);

    // Two monitors side by side. The captured display's own centre is now a
    // quarter and three quarters of the way along the device's axis — which is
    // the number the compositor needs to put the pointer on the right screen.
    CHECK(desktopToAbsoluteRange(0, 0, 3840, 1080, 960, 540, 32767, x, y));
    CHECK_EQ(x, 8193);
    CHECK(desktopToAbsoluteRange(0, 0, 3840, 1080, 2880, 540, 32767, x, y));
    CHECK_EQ(x, 24581);

    // A desktop that does not start at the origin — a monitor to the left of the
    // primary. The desktop's own origin is subtracted before the fraction, or
    // every position on it comes out negative and gets clamped to zero.
    CHECK(desktopToAbsoluteRange(-1920, 0, 1920, 1080, -1920, 0, 32767, x, y));
    CHECK_EQ(x, 0);
    CHECK(desktopToAbsoluteRange(-1920, 0, 1920, 1080, 1919, 1079, 32767, x, y));
    CHECK_EQ(x, 32767);

    // Out of the desktop entirely: clamped to the range, never wrapped around
    // to the opposite edge.
    CHECK(desktopToAbsoluteRange(0, 0, 1920, 1080, -500, 5000, 32767, x, y));
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 32767);

    // Nothing usable: unknown desktop, one column wide, or no range.
    CHECK(!desktopToAbsoluteRange(0, 0, 0, 0, 10, 10, 32767, x, y));
    CHECK(!desktopToAbsoluteRange(0, 0, 1, 1080, 0, 10, 32767, x, y));
    CHECK(!desktopToAbsoluteRange(0, 0, 1920, 1080, 10, 10, 0, x, y));

    // ── The two together, which is how UinputInput uses them ────────────────

    // The case that was broken: a 1280x720 client picture of the SECOND of two
    // 1920x1080 monitors. The centre of the picture must come out in the right
    // half of the device's axis; before the fix it came out at 16383 — the
    // middle, which is the seam between the two screens.
    int onDeskX = 0;
    int onDeskY = 0;
    CHECK(displayPointToDesktop(1920, 0, 3840, 1080, 640, 360, 1280, 720, onDeskX, onDeskY));
    CHECK_EQ(onDeskX, 2880);
    CHECK(desktopToAbsoluteRange(0, 0, 3840, 1080, onDeskX, onDeskY, 32767, x, y));
    CHECK(x > 32767 / 2);
    CHECK_EQ(x, 24581);
    CHECK_EQ(y, 16398);

    // And the first monitor of that same pair stays in the left half.
    CHECK(displayPointToDesktop(0, 0, 1920, 1080, 640, 360, 1280, 720, onDeskX, onDeskY));
    CHECK(desktopToAbsoluteRange(0, 0, 3840, 1080, onDeskX, onDeskY, 32767, x, y));
    CHECK(x < 32767 / 2);
    CHECK_EQ(x, 8193);
}
