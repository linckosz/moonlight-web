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

#include "CursorShape.h"
#include "IWindowsCapture.h"

#include <windows.h>

#include <cstdint>

// The mouse pointer, read from Win32 rather than from the capture.
//
// ── Why this exists ─────────────────────────────────────────────────────────
//
// Desktop Duplication hands the pointer over with the frame: a position, and a
// shape whenever it changed. Windows.Graphics.Capture hands over nothing of the
// sort — it can composite the pointer INTO the picture, and that is all it
// offers. Baking it in is not good enough here for two reasons: the client can
// ask to draw its own pointer (which needs the shape as data, not as pixels),
// and a phone magnifies it (which needs it separable from the desktop).
//
// So the WGC path asks Windows directly. GetCursorInfo gives the position and
// the HCURSOR; GetIconInfo turns that into the same three encodings Desktop
// Duplication reports, which CursorShape then decodes exactly as before.
//
// ── The one thing that cannot be had this way ───────────────────────────────
//
// GetCursorInfo describes the pointer as the DESKTOP sees it, so a shape is
// re-read only when the HCURSOR handle changes. That handle is stable per shape,
// which is precisely the "thousands of frames, one shape" property the decoder
// wants — but it is a handle, not a version: Windows may hand back the same
// handle for a shape it re-created. In practice it does not, and the cost of
// being wrong is one stale pointer image until the next shape change.

namespace mw::native::capture {

/// Reads the pointer for a capture backend that does not report one.
///
/// Not thread-safe, and not meant to be: it is called from the capture thread,
/// once per acquired frame, and holds a cache of the last shape it decoded.
class Win32Cursor
{
public:
    Win32Cursor() = default;
    ~Win32Cursor();

    Win32Cursor(const Win32Cursor&) = delete;
    Win32Cursor& operator=(const Win32Cursor&) = delete;

    /// Refresh @p cursor for a display occupying @p rect on the virtual desktop
    /// and captured at @p captureWidth × @p captureHeight pixels.
    ///
    /// The two sizes differ on a scaled display — `rect` is DPI-virtualized (see
    /// DesktopRect) while the captured texture is in real pixels — so the
    /// position is scaled between them, exactly as Desktop Duplication's own
    /// figures already are.
    ///
    /// Returns true when anything the consumer can see changed: the position,
    /// the visibility, or the shape. A pointer that did not move at all makes
    /// this false, which is what lets the capture loop leave a still screen
    /// alone.
    bool update(CursorState& cursor, const DesktopRect& rect, int captureWidth, int captureHeight);

    int hotspotX() const { return m_HotspotX; }
    int hotspotY() const { return m_HotspotY; }

private:
    /// Decode @p handle into @p cursor, unless it is the shape already held.
    bool refreshShape(HCURSOR handle, CursorState& cursor);

    HCURSOR m_Shape = nullptr;
    int m_HotspotX = 0;
    int m_HotspotY = 0;

    /// Scratch for the packed shape handed to decodeCursorShape. Kept between
    /// calls so a shape change does not allocate on the capture thread.
    std::vector<uint8_t> m_Buffer;
};

} // namespace mw::native::capture
