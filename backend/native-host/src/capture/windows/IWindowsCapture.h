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

#include <d3d11.h>

#include <cstdint>
#include <string>
#include <vector>

// Screen capture on Windows.
//
// ── Why this interface is Windows-shaped, not cross-platform ────────────────
//
// It hands back an ID3D11Texture2D, which is the whole point: the frame stays
// in VRAM on the adapter that scanned it out, and the encoder registers that
// exact texture. A platform-neutral interface could not name this type, so it
// would have to erase it behind a void* or copy through system memory — and a
// copy through system memory is precisely the cost this engine exists to avoid.
//
// Cross-platform abstraction therefore happens one level up, at Session: each
// platform pairs its own capture with its own encoder, and nothing generic ever
// has to hold a frame.
//
// Two implementations share this: Desktop Duplication (primary) and
// Windows.Graphics.Capture (automatic fallback). Both yield the same texture
// type, which is what lets the encoder stage be written once.

namespace mw::native::capture {

/// What one acquire() attempt produced.
enum class AcquireStatus
{
    /// A new frame is in `CapturedFrame::texture`, and release() must be called
    /// before the next acquire().
    Ok,

    /// Nothing was presented within the timeout. Not an error, and the common
    /// case on a still desktop: the display genuinely has nothing new. The
    /// caller must NOT treat this as a dropped frame.
    Timeout,

    /// Only the mouse moved or changed shape — the desktop image is untouched,
    /// so DXGI hands back no usable texture and release() has already happened.
    ///
    /// Worth its own status rather than being folded into Timeout: the cursor is
    /// composited by us, so a pointer move IS a visible change even though not
    /// one pixel of the desktop moved. Reporting it as "nothing happened" is
    /// what made the cursor sit still on a quiet screen.
    PointerOnly,

    /// The duplication became invalid — a mode change, a resolution change, a
    /// desktop switch (UAC / lock screen), or the GPU being reset. Recoverable:
    /// the caller re-runs start() and carries on.
    Lost,

    /// Unrecoverable. `error` on the capture object says what happened.
    Failed,
};

/// One captured frame. The texture is BORROWED: it belongs to the capture and
/// is valid only until release(). Nothing here owns anything.
struct CapturedFrame
{
    ID3D11Texture2D* texture = nullptr;

    /// When the OS says this frame was actually put on the display, in
    /// microseconds on the same steady clock as the rest of the engine.
    ///
    /// This is a real measurement (DXGI's LastPresentTime, a QPC value), not
    /// the time we happened to notice the frame. Every latency figure the
    /// engine reports is measured from here, so an approximation would quietly
    /// corrupt the whole benchmark story.
    int64_t presentUs = 0;

    /// When acquire() returned. `capturedUs - presentUs` is the capture
    /// latency, and it is the first number worth watching.
    int64_t capturedUs = 0;
};

/// The mouse pointer, as Desktop Duplication reports it.
///
/// ── Why we have to draw it ourselves ────────────────────────────────────────
///
/// The duplicated desktop image does NOT contain the cursor. Windows composites
/// the pointer at scan-out, so a captured frame is the desktop with a hole where
/// the user is looking. DXGI hands the pointer over separately — a position, and
/// a shape that changes only when the cursor does — and compositing the two is
/// the caller's job.
///
/// The shape arrives in three encodings, and all three are reduced here to one:
/// an RGBA image plus a per-pixel invert flag. Monochrome cursors (the text
/// I-beam, most resize arrows) are the reason the flag exists — they carry no
/// colour of their own and are defined as inverting whatever is behind them,
/// which is what keeps an I-beam visible on both black and white text areas.
struct CursorState
{
    /// Whether the pointer is on THIS display right now.
    bool visible = false;

    /// Top-left of the cursor image in captured-frame pixels — the hotspot has
    /// already been subtracted, so this is where the image goes.
    int x = 0;
    int y = 0;

    int width = 0;
    int height = 0;

    /// width × height × 4, BGRA order as D3D11 wants it. Alpha is real coverage:
    /// colour cursors antialias their edges.
    std::vector<uint8_t> pixels;

    /// width × height, 255 where the pixel inverts the background instead of
    /// replacing it. Zero everywhere for an ordinary colour cursor.
    std::vector<uint8_t> invert;

    /// Bumped every time `pixels`/`invert` change. Lets the consumer re-upload
    /// the small textures only when the shape actually changed — a moving
    /// cursor keeps the same shape for thousands of frames.
    uint64_t shapeVersion = 0;
};

/// Where a display sits on the Windows desktop.
///
/// ── This is the DPI-VIRTUALIZED rectangle, on purpose ───────────────────────
///
/// DXGI reports desktop coordinates through the same DPI virtualization that
/// made a 2560×1440 monitor at 125% measure 2048×1152 — the bug that sent the
/// probe to QueryDisplayConfig for the real mode. Do not "fix" this one the
/// same way. SendInput's absolute coordinates are expressed against
/// SM_XVIRTUALSCREEN/SM_CXVIRTUALSCREEN, which are virtualized identically, so
/// the two agree exactly. Substituting the true pixel size here would put the
/// cursor in the wrong place on every scaled display.
struct DesktopRect
{
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    int width() const { return right - left; }
    int height() const { return bottom - top; }
    bool valid() const { return right > left && bottom > top; }
};

class IWindowsCapture
{
public:
    virtual ~IWindowsCapture() = default;

    IWindowsCapture(const IWindowsCapture&) = delete;
    IWindowsCapture& operator=(const IWindowsCapture&) = delete;

    /// Open (or re-open) the capture. Safe to call again after Lost, which is
    /// how a mode change is recovered from.
    virtual bool start(std::string& error) = 0;

    /// Wait up to @p timeoutMs for the display to present a new frame.
    ///
    /// Blocking on the OS rather than polling is deliberate: Desktop
    /// Duplication wakes the caller ON the present, so a frame is picked up as
    /// soon as it exists instead of on the next tick of a timer we chose. That
    /// is worth more than any amount of downstream tuning.
    virtual AcquireStatus acquire(int timeoutMs, CapturedFrame& frame) = 0;

    /// Give the frame back. Every Ok from acquire() must be matched by exactly
    /// one release(), and the texture must not be touched afterwards: Desktop
    /// Duplication refuses the next acquire() while a frame is still held.
    virtual void release() = 0;

    virtual void stop() = 0;

    /// The D3D11 device the frames live on — created on the adapter that drives
    /// the captured display, so the encoder can attach to it with no copy.
    virtual ID3D11Device* device() const = 0;

    /// That device's immediate context. Exposed so the caller can issue GPU
    /// work against the captured textures — a copy, most usefully — without
    /// making a second context that would need its own synchronisation.
    virtual ID3D11DeviceContext* context() const = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;

    /// The texture format frames arrive in: B8G8R8A8_UNORM for SDR,
    /// R16G16B16A16_FLOAT when the output is scanning out HDR. The colour
    /// conversion stage keys off this rather than off what was requested.
    virtual DXGI_FORMAT format() const = 0;

    /// Where the captured display sits on the desktop, for aiming absolute
    /// mouse input at it. Valid only once start() has succeeded.
    virtual DesktopRect desktopRect() const = 0;

    /// The pointer as of the last acquire(), whatever that acquire returned.
    /// Updated on Ok and on PointerOnly alike.
    virtual const CursorState& cursor() const = 0;

protected:
    IWindowsCapture() = default;
};

} // namespace mw::native::capture
