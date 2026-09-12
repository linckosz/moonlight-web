/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "capture/windows/PaintedPointer.h"

using namespace mw::native::capture;

// Telling a Desktop Duplication that paints the pointer into the picture from
// one that keeps it apart.
//
// On 12/09/2026 the Windows-on-ARM bench (Adreno 618) streamed three sessions
// without Desktop Duplication reporting a single pointer shape: the driver has
// no hardware pointer, so Windows painted it into the desktop. The viewer got a
// small stream-rate copy baked into the video — no pointer of their own on a
// desktop, no magnified one on a phone. The verdict below is what moves such a
// display to Windows.Graphics.Capture.

void run_painted_pointer_tests()
{
    SECTION("Painted pointer — a duplication that never reports the pointer");

    // The bench: the viewer sweeps the mouse, and nothing is ever reported.
    {
        PaintedPointer p;
        CHECK(p.undecided());
        for (int i = 0; i <= PaintedPointer::kMovesBeforeVerdict; ++i)
            p.noteSample(true, 100 + i, 200);
        CHECK(p.paintedIn());
        CHECK(!p.undecided());
    }

    // One move short is not a verdict: a slow first sweep must not switch a
    // capture that might still report.
    {
        PaintedPointer p;
        for (int i = 0; i < PaintedPointer::kMovesBeforeVerdict; ++i)
            p.noteSample(true, i, 0);
        CHECK(!p.paintedIn());
        CHECK(p.undecided());
    }

    // The ordinary machine: the shape comes with the first frame, and the
    // question is closed for good — however much the pointer moves afterwards.
    {
        PaintedPointer p;
        p.noteSample(true, 0, 0);
        p.noteReported();
        CHECK(!p.undecided());
        for (int i = 0; i < 100; ++i)
            p.noteSample(true, i, i);
        CHECK(!p.paintedIn());
    }

    // A pointer that does not move proves nothing: a still mouse over a
    // playing video is thousands of frames with no pointer news on any driver.
    {
        PaintedPointer p;
        for (int i = 0; i < 1000; ++i)
            p.noteSample(true, 640, 360);
        CHECK(!p.paintedIn());
    }

    // Hidden, or on another monitor: those moves are not this display's, and
    // coming back does not count as a move either.
    {
        PaintedPointer p;
        for (int i = 0; i < 100; ++i)
            p.noteSample(false, i * 7, i * 3);
        CHECK(!p.paintedIn());
        p.noteSample(true, 5, 5);
        for (int i = 0; i < PaintedPointer::kMovesBeforeVerdict - 1; ++i) {
            p.noteSample(false, 0, 0);
            p.noteSample(true, 5, 5);
        }
        CHECK(!p.paintedIn());
    }
}
