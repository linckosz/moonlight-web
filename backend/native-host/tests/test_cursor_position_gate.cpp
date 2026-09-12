/*
 * MoonlightWeb — native capture & encoding engine, test suite.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>. GPLv3.
 */
#include "native_test_framework.h"

#include "core/CursorPositionGate.h"

using namespace mw::native;

// How often the host tells a self-drawing client where its pointer is.
//
// The client on a touch screen moves the pointer from its own finger and only
// needs the host to correct the drift, so the host speaks at most once per
// interval while the pointer moves, and once more when it stops.

void run_cursor_position_gate_tests()
{
    SECTION("Cursor position gate — sparse, final position always out");

    constexpr int64_t kUs = CursorPositionGate::kIntervalUs;

    // The first word is free: a client that just took the pointer over has
    // seen nothing, whatever the interval says.
    {
        CursorPositionGate g;
        CHECK(g.due(true, 10, 20, 0));
    }

    // A sweep of the mouse: one message per interval, not one per event.
    {
        CursorPositionGate g;
        int sent = 0;
        for (int64_t t = 0; t < 10 * kUs; t += kUs / 10)
            if (g.due(true, static_cast<int>(t), 0, t)) sent++;
        CHECK(sent == 10);
    }

    // The pointer stops between two intervals: its resting place still goes
    // out, on the first look after the interval — the client corrects itself
    // against THAT position, not against one from mid-sweep.
    {
        CursorPositionGate g;
        CHECK(g.due(true, 0, 0, 0));
        CHECK(!g.due(true, 500, 0, kUs / 2)); // mid-interval, held back
        CHECK(g.due(true, 500, 0, kUs));      // interval over: out it goes
        CHECK(!g.due(true, 500, 0, 5 * kUs)); // still there: nothing to say
    }

    // A pointer that does not move says nothing, however long it sits.
    {
        CursorPositionGate g;
        CHECK(g.due(true, 7, 7, 0));
        for (int64_t t = kUs; t < 100 * kUs; t += kUs)
            CHECK(!g.due(true, 7, 7, t));
    }

    // Hidden is announced once; where it was last seen stops mattering.
    {
        CursorPositionGate g;
        CHECK(g.due(true, 7, 7, 0));
        CHECK(g.due(false, 7, 7, kUs));
        CHECK(!g.due(false, 999, 999, 2 * kUs));
        CHECK(g.due(true, 999, 999, 3 * kUs)); // back on the display
    }

    // Reset: the pointer goes out again even where it already was — a client
    // that switched to drawing its own pointer mid-session starts from nothing.
    {
        CursorPositionGate g;
        CHECK(g.due(true, 7, 7, 0));
        CHECK(!g.due(true, 7, 7, kUs));
        g.reset();
        CHECK(g.due(true, 7, 7, kUs));
    }
}
