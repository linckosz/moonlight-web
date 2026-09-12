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
 */

#pragma once

#include <cstdint>

// When the host's pointer POSITION is worth a message to the client.
//
// A client that draws its own pointer on a touch screen moves it from its own
// finger, ahead of the host, and needs the host's word only to correct the
// drift: a pointer stopped by a screen edge, moved by an application, or
// accelerated by the host's own mouse settings. That word is small and it is
// late by definition, so it goes out sparingly — one message per interval while
// the pointer moves, and the final resting place once it stops — rather than
// once per mouse event, which would be hundreds of messages a second describing
// a pointer the client has already drawn somewhere else.
//
// Pure bookkeeping, no OS: tested on every platform, like PaintedPointer.h.

namespace mw::native {

class CursorPositionGate
{
public:
    /// The least time between two position messages while the pointer moves.
    /// Short enough that a pointer parked by the host is corrected within a
    /// blink; long enough that a sweep of the mouse costs a handful of
    /// messages rather than a stream of them.
    static constexpr int64_t kIntervalUs = 50 * 1000;

    /// Whether this position should go out now. Records it when it does.
    ///
    /// A hidden pointer is one position: it is announced once, and where it
    /// was last seen stops mattering.
    bool due(bool visible, int x, int y, int64_t nowUs)
    {
        const bool same = m_Sent && visible == m_Visible && (!visible || (x == m_X && y == m_Y));
        if (same) return false;
        if (m_Sent && nowUs - m_SentUs < kIntervalUs) return false;
        m_Sent = true;
        m_Visible = visible;
        m_X = x;
        m_Y = y;
        m_SentUs = nowUs;
        return true;
    }

    /// Forget what was sent, so the next position goes out whatever it is.
    /// For a client that has just taken the pointer over: it has seen nothing.
    void reset() { m_Sent = false; }

private:
    bool m_Sent = false;
    bool m_Visible = false;
    int m_X = 0;
    int m_Y = 0;
    int64_t m_SentUs = 0;
};

} // namespace mw::native
