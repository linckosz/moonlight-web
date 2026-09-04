/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#if defined(_WIN32)
#include "capture/windows/Win32Cursor.h"
#endif

#include <cstdio>

// Only the Windows header above declares the namespace: on Linux the body of
// this file is a single "skipped" line, and opening mw::native would not
// compile — which is exactly what happened on the first Linux build.
#if defined(_WIN32)
using namespace mw::native;
#endif

// The pointer, read from Win32 rather than from the capture.
//
// This is what the Windows.Graphics.Capture path uses: WGC hands over no cursor
// of its own, so the shape comes from GetIconInfo and goes through the same
// decoder Desktop Duplication feeds. It is worth its own test because the two
// halves fail differently and both fail QUIETLY — a mis-decoded shape is a black
// square where the pointer should be, and a mis-scaled position is a pointer
// drawn a few centimetres from where it really is. Neither raises anything.

void run_win32_cursor_tests()
{
    SECTION("Win32 cursor — the shape WGC does not provide");

#if !defined(_WIN32)
    std::fprintf(stderr, "  skipped: Win32 only\n");
#else
    using namespace mw::native::capture;

    // A 2560×1440 display at the desktop origin, captured at its own size: the
    // simple case, where scaling is the identity and any offset is a real bug.
    DesktopRect rect;
    rect.left = 0;
    rect.top = 0;
    rect.right = 2560;
    rect.bottom = 1440;

    Win32Cursor probe;
    CursorState cursor;
    probe.update(cursor, rect, 2560, 1440);

    // A session-0 or headless runner has no pointer at all, which is a fact
    // about the machine and not a failure. Everything below needs one.
    if (!cursor.visible) {
        std::fprintf(stderr, "  skipped: no pointer on this desktop (headless or session 0)\n");
        return;
    }

    std::fprintf(stderr, "  cursor: %dx%d, ink %dx%d, hotspot %d,%d at %d,%d\n", cursor.width,
                 cursor.height, cursor.inkWidth, cursor.inkHeight, probe.hotspotX(),
                 probe.hotspotY(), cursor.x, cursor.y);

    // The shape decoded into something drawable at all.
    CHECK(cursor.width > 0);
    CHECK(cursor.height > 0);
    CHECK_EQ(cursor.pixels.size(), static_cast<size_t>(cursor.width) * cursor.height * 4);
    CHECK_EQ(cursor.invert.size(), static_cast<size_t>(cursor.width) * cursor.height);
    CHECK(cursor.shapeVersion > 0);

    // Ink is what a magnified pointer is sized from, and it must be a real
    // subset of the canvas: Windows pads an arrow into 32×32 and only a corner
    // of it is drawn. Zero ink means the decode produced an empty image — the
    // failure that shows up as an invisible pointer.
    CHECK(cursor.inkWidth > 0);
    CHECK(cursor.inkHeight > 0);
    CHECK(cursor.inkWidth <= cursor.width);
    CHECK(cursor.inkHeight <= cursor.height);

    // The hotspot lies inside the shape. Outside it, a magnified pointer grows
    // around a point that is not on it and slides away from the real cursor.
    CHECK(probe.hotspotX() >= 0);
    CHECK(probe.hotspotY() >= 0);
    CHECK(probe.hotspotX() <= cursor.width);
    CHECK(probe.hotspotY() <= cursor.height);

    // Every pixel is either drawn, inverting, or absent — never both drawn and
    // inverting, which is what a mask read with the wrong polarity produces.
    size_t drawn = 0;
    size_t inverting = 0;
    for (size_t i = 0; i < cursor.invert.size(); ++i) {
        const bool inv = cursor.invert[i] != 0;
        const bool opaque = cursor.pixels[i * 4 + 3] != 0;
        if (inv) {
            ++inverting;
            CHECK(opaque); // an inverting pixel is a visible one
        } else if (opaque) {
            ++drawn;
        }
    }
    CHECK(drawn + inverting > 0);

    // ── The position, which is the half that is easy to get wrong ───────────

    // Asked again with nothing moved: the shape is not re-decoded, because the
    // handle has not changed. That cache is what keeps a cursor that moves over
    // thousands of frames from re-uploading a texture on each of them.
    const uint64_t version = cursor.shapeVersion;
    probe.update(cursor, rect, 2560, 1440);
    CHECK_EQ(cursor.shapeVersion, version);

    // A capture half the size of the desktop rectangle halves the position.
    // This is the DPI-scaled display: the rectangle is virtualized, the texture
    // is in real pixels, and skipping the scale puts the drawn pointer a
    // fraction of the screen away from the real one.
    CursorState scaled;
    Win32Cursor scaledProbe;
    scaledProbe.update(scaled, rect, 1280, 720);
    CHECK(scaled.visible);
    // Both were read from the same unmoved pointer, so the halved one must sit
    // at half the offset — within a pixel, the hotspot and the integer
    // truncation being the only differences.
    const int fullX = cursor.x + probe.hotspotX();
    const int halfX = scaled.x + scaledProbe.hotspotX();
    CHECK(halfX <= fullX / 2 + 1);
    CHECK(halfX >= fullX / 2 - 1);

    // A pointer that is not on this display is not drawn on it. The rectangle
    // here is a second monitor far to the right of wherever the cursor is.
    DesktopRect elsewhere;
    elsewhere.left = 100000;
    elsewhere.top = 100000;
    elsewhere.right = 101920;
    elsewhere.bottom = 101080;
    CursorState offscreen;
    Win32Cursor offscreenProbe;
    offscreenProbe.update(offscreen, elsewhere, 1920, 1080);
    CHECK(!offscreen.visible);
#endif
}
