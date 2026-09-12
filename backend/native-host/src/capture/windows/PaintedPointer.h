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

// A Desktop Duplication that hands the pointer over already painted into the
// picture.
//
// Desktop Duplication normally leaves the pointer OUT of the frame and reports
// it apart — a shape, then positions — because the pointer lives on a hardware
// plane the desktop image never contains. A display driver with no hardware
// pointer has no such plane: Windows then draws the pointer into the desktop
// itself, and the duplication reports nothing about it, ever. The Adreno 618 of
// the Windows-on-ARM bench is one (12/09/2026: three sessions, not one shape).
//
// Nothing fails when that happens, which is what makes it worth detecting. The
// pointer the viewer sees is a small, stream-rate copy baked into the video: a
// desktop browser cannot draw its own over it, and a phone cannot have it
// magnified. Windows.Graphics.Capture can leave that same pointer out, so the
// session switches to it once this says so.
//
// The tell: the pointer visibly moves across this display, and the duplication
// has still not said a word about it. A duplication with a hardware pointer
// sends the shape with the very first frame the pointer is seen in, so a
// handful of moves without one is not a slow driver — it is a driver that will
// never send one.
//
// Pure bookkeeping, no Windows: tested on every OS, like FrameFit.h.

namespace mw::native::capture {

class PaintedPointer
{
public:
    /// Moves over this display, with no pointer news, before the verdict. Far
    /// more than a hardware pointer ever takes to be reported (zero), and few
    /// enough that the viewer's first sweep of the mouse settles it.
    static constexpr int kMovesBeforeVerdict = 10;

    /// The duplication reported the pointer — a shape, or a failed attempt to
    /// read one, which still proves the pointer is kept apart. Settles it.
    void noteReported() { m_Reported = true; }

    /// What the OS says about the pointer at one acquire. @p onDisplay is false
    /// when it is hidden or on another monitor: those moves prove nothing.
    void noteSample(bool onDisplay, long x, long y)
    {
        if (m_Reported || !onDisplay) {
            m_HaveLast = false;
            return;
        }
        if (m_HaveLast && (x != m_LastX || y != m_LastY)) m_UnreportedMoves++;
        m_HaveLast = true;
        m_LastX = x;
        m_LastY = y;
    }

    /// Whether the OS still needs to be asked. False once either verdict is in,
    /// so a working duplication stops paying for the question at its first frame.
    bool undecided() const { return !m_Reported && m_UnreportedMoves < kMovesBeforeVerdict; }

    /// The pointer is in the picture and will stay there.
    bool paintedIn() const { return !m_Reported && m_UnreportedMoves >= kMovesBeforeVerdict; }

private:
    bool m_Reported = false;
    int m_UnreportedMoves = 0;
    bool m_HaveLast = false;
    long m_LastX = 0;
    long m_LastY = 0;
};

} // namespace mw::native::capture
