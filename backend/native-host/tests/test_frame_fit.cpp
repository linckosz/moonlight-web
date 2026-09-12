/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "platform/macos/FrameFit.h"

using namespace mw::native::platform;

// The letterbox ScreenCaptureKit puts between the desktop and the frame.
//
// The numbers below are the bench Mac's: a 14" MacBook Pro (3024x1964, 1.54:1)
// streamed at 1920x1080. SCK keeps the desktop's aspect, so the picture is 1663
// pixels wide with a 129-pixel black bar on each side — and on 12/09/2026 both
// halves of the pipeline ignored that. The pointer the engine draws for a phone
// was placed across the whole frame, so near either edge it landed ON a bar,
// where nothing repaints over it: one pointer left behind per place it went. A
// tap was mapped the same way and landed short of what it was aimed at.
//
// Plain arithmetic, so it is tested on every platform — which is why FrameFit.h
// is a header with no macOS in it.

void run_frame_fit_tests()
{
    SECTION("Frame fit — the desktop inside a frame of another shape");

    // ── The fit itself ───────────────────────────────────────────────────────

    // The bench Mac: pillarboxed, 129 pixels a side, and the scale is the one
    // that made the HEIGHT fit — using the widths' ratio instead is the bug.
    {
        const FrameFit fit = frameFit(3024, 1964, 1920, 1080);
        CHECK_EQ(static_cast<int>(std::lround(fit.offsetX)), 129);
        CHECK_EQ(static_cast<int>(std::lround(fit.offsetY)), 0);
        CHECK(fit.scale > 0.549f && fit.scale < 0.550f);
        // The right edge of the desktop is the right edge of the PICTURE, 129
        // pixels short of the frame — which is where the trail used to start.
        CHECK_EQ(static_cast<int>(std::lround(fit.offsetX + 3024 * fit.scale)), 1791);
    }

    // Same shape on both sides: no bars, and the identity when the sizes match.
    {
        const FrameFit fit = frameFit(1920, 1080, 1920, 1080);
        CHECK_EQ(static_cast<int>(std::lround(fit.offsetX)), 0);
        CHECK_EQ(static_cast<int>(std::lround(fit.offsetY)), 0);
        CHECK(fit.scale > 0.999f && fit.scale < 1.001f);
    }
    {
        const FrameFit fit = frameFit(3840, 2160, 1920, 1080);
        CHECK_EQ(static_cast<int>(std::lround(fit.offsetX)), 0);
        CHECK_EQ(static_cast<int>(std::lround(fit.offsetY)), 0);
        CHECK(fit.scale > 0.499f && fit.scale < 0.501f);
    }

    // The other way round: a desktop WIDER than the frame's shape gets bars
    // above and below. An ultrawide streamed at 16:9 is the real case.
    {
        const FrameFit fit = frameFit(3440, 1440, 1920, 1080);
        CHECK_EQ(static_cast<int>(std::lround(fit.offsetX)), 0);
        CHECK_EQ(static_cast<int>(std::lround(fit.offsetY)), 138);
    }

    // Nothing is known yet: the identity, so a caller placing a pointer before
    // the session has geometry places it where it always did.
    {
        const FrameFit fit = frameFit(0, 0, 1920, 1080);
        CHECK(fit.scale == 1.0f);
        CHECK(fit.offsetX == 0.0f);
        CHECK(fit.offsetY == 0.0f);
    }

    // ── The inverse, for a point the client aimed at the frame ───────────────

    const FrameFit mac = frameFit(3024, 1964, 1920, 1080);
    int x = -1;
    int y = -1;

    // The middle is the middle whatever the bars do.
    CHECK(unletterboxPoint(mac, 1920, 1080, 960, 540, x, y));
    CHECK_EQ(x, 960);
    CHECK_EQ(y, 540);

    // The left edge of the picture is the left edge of the desktop. Without the
    // fit this point reported itself 129 frame-pixels in, which is 200 points of
    // desktop the viewer could not reach.
    CHECK(unletterboxPoint(mac, 1920, 1080, mac.offsetX, 540, x, y));
    CHECK_EQ(x, 0);
    CHECK_EQ(y, 540);

    // And the right edge of the picture is the right edge of the desktop.
    CHECK(unletterboxPoint(mac, 1920, 1080, 1920 - mac.offsetX, 540, x, y));
    CHECK_EQ(x, 1920 - 1);

    // A point on a bar belongs to no pixel of the desktop: it clamps to the
    // edge it is nearest rather than wrapping round or going out of range.
    CHECK(unletterboxPoint(mac, 1920, 1080, 0, 540, x, y));
    CHECK_EQ(x, 0);
    CHECK(unletterboxPoint(mac, 1920, 1080, 1919, 540, x, y));
    CHECK_EQ(x, 1919);

    // Vertically there are no bars here, so nothing moves on that axis.
    CHECK(unletterboxPoint(mac, 1920, 1080, 960, 0, x, y));
    CHECK_EQ(y, 0);
    CHECK(unletterboxPoint(mac, 1920, 1080, 960, 1079, x, y));
    CHECK_EQ(y, 1079);

    // No letterbox, no change — the common case, and it must cost nothing.
    {
        const FrameFit none = frameFit(3840, 2160, 1920, 1080);
        CHECK(unletterboxPoint(none, 1920, 1080, 17, 933, x, y));
        CHECK_EQ(x, 17);
        CHECK_EQ(y, 933);
    }

    // An empty reference is refused rather than mapped to nonsense: the caller
    // leaves the event alone, exactly as the input sink already does with one.
    x = -1;
    y = -1;
    CHECK(!unletterboxPoint(mac, 0, 1080, 100, 100, x, y));
    CHECK_EQ(x, -1);
}
