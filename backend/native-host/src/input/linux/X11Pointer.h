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

#pragma once

namespace mw::native::input {

/// Where a pointer at (@p x, @p y) has to be put to sit inside the rectangle
/// [left, right) × [top, bottom). False when it is already inside, or when the
/// rectangle is empty — in both cases there is nothing to do and nothing should
/// be moved.
///
/// The NEAREST point, not the centre: a pointer that has just crossed an edge
/// comes back onto that same edge, which is where the viewer last saw it going.
/// Dropping it in the middle of the screen would be a jump they did not ask for.
///
/// Pure arithmetic, deliberately in the header and free of X: it is the half of
/// the warp that can be wrong in a boring way (an off-by-one at the far edge,
/// a rectangle read as width/height instead of right/bottom), and this way the
/// tests reach it on every platform, including the ones with no X at all.
inline bool clampIntoRect(int left, int top, int right, int bottom, int x, int y, int& outX,
                          int& outY)
{
    if (right <= left || bottom <= top) return false;
    if (x >= left && x < right && y >= top && y < bottom) return false;
    outX = x < left ? left : (x >= right ? right - 1 : x);
    outY = y < top ? top : (y >= bottom ? bottom - 1 : y);
    return true;
}

/// The X server's pointer — read and moved, without linking to X.
///
/// ── Why this exists ─────────────────────────────────────────────────────────
///
/// uinput sends DELTAS, and a delta moves the pointer from wherever the
/// compositor currently has it. On a host with several monitors that may be a
/// screen the viewer is not looking at, and then no amount of swiping brings it
/// back: the pointer just runs along an edge of a screen nobody can see. Windows
/// has the same problem and solves it by reading the pointer and warping it
/// (Win32Input::bringCursorOntoDisplay); a kernel device cannot, because it has
/// no idea where the pointer ended up.
///
/// X does know. XQueryPointer gives the position in root coordinates and
/// XWarpPointer puts it back. Both are X11 and only X11: under Wayland no client
/// may read or move another client's pointer, by design, and there is no
/// fallback to write — a Wayland host simply keeps the behaviour it has today.
///
/// ── Loaded, not linked ──────────────────────────────────────────────────────
///
/// libX11 is resolved with dlopen at runtime rather than linked. UinputInput
/// builds on any Linux with no -dev package at all, and that is a property worth
/// keeping: a headless KMS host, a Wayland desktop and an X desktop all build
/// and run from one binary, and only the last of the three finds the library. A
/// missing library, an unset DISPLAY and a refused connection all give the same
/// answer — the pointer cannot be reached here — and motion is then left exactly
/// as it was.
///
/// ── Threads ─────────────────────────────────────────────────────────────────
///
/// Xlib wants either XInitThreads or a caller that serialises its calls. The
/// second is what happens here: everything goes through UinputInput under its
/// m_Mutex, the same lock that already serialises the uinput writes.
class X11Pointer
{
public:
    X11Pointer() = default;
    ~X11Pointer();

    X11Pointer(const X11Pointer&) = delete;
    X11Pointer& operator=(const X11Pointer&) = delete;

    /// Attach to the display named by DISPLAY. False when there is none, which
    /// is the ordinary answer on Wayland and on a headless host — not a failure,
    /// and not worth more than a debug line.
    bool open();
    void close();
    bool isOpen() const { return m_Display != nullptr; }

    /// The pointer in root coordinates. False when it cannot be read, which
    /// includes the pointer being on another X screen of the same display (a
    /// Xinerama-less multi-head layout, where warping would not help either).
    bool position(int& x, int& y);

    /// Put the pointer at (@p x, @p y) in root coordinates, and flush. The flush
    /// is not optional: left in the output buffer the request would reach the
    /// server after the delta that follows it, and the delta would be applied
    /// from the old position — which is the entire bug, back again.
    bool warp(int x, int y);

private:
    /// Xlib's own types, spelled out so no X header is needed to declare them.
    /// Display is opaque everywhere; XID is unsigned long on every platform X
    /// supports, which is the guarantee the protocol itself is written against.
    using XDisplay = void;
    using XWindow = unsigned long;

    void* m_Lib = nullptr;
    XDisplay* m_Display = nullptr;
    XWindow m_Root = 0;

    XDisplay* (*m_XOpenDisplay)(const char*) = nullptr;
    int (*m_XCloseDisplay)(XDisplay*) = nullptr;
    XWindow (*m_XDefaultRootWindow)(XDisplay*) = nullptr;
    int (*m_XQueryPointer)(XDisplay*, XWindow, XWindow*, XWindow*, int*, int*, int*, int*,
                           unsigned int*) = nullptr;
    int (*m_XWarpPointer)(XDisplay*, XWindow, XWindow, int, int, unsigned int, unsigned int, int,
                          int) = nullptr;
    int (*m_XFlush)(XDisplay*) = nullptr;
};

} // namespace mw::native::input
