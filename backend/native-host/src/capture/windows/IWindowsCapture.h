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

#include "../CaptureTypes.h"

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

    // For the click trace (core/ClickTrace.h), left at 0 / -1 where the
    // capture cannot tell.
    /// The OS's own present stamp, before any correction: WGC's runs up to
    /// ~9 ms ahead of the moment it hands the frame over, and presentUs is
    /// clamped to capturedUs.
    int64_t presentRawUs = 0;
    /// DDA: when the pointer last moved or changed, 0 when this frame says
    /// nothing of it.
    int64_t mouseUs = 0;
    /// DDA: the presents folded into this frame (AccumulatedFrames).
    int accumulated = -1;

    /// The desktop as it stood when retake() was answered, not a present:
    /// presentUs says nothing worth measuring from.
    bool retaken = false;
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

    /// Have the next acquire() hand the desktop over as it stands, still
    /// screen or not, flagged CapturedFrame::retaken — what a fresh start()
    /// delivers first, on the same device, so that whatever was built on it
    /// stays. For the pointer drawn into the picture over a desktop nobody
    /// kept a copy of (see WindowsSession's retakeDesktop()). The caller lets
    /// go of every texture it kept from this capture first.
    ///
    /// False with @p error when the capture cannot. Where it had already let
    /// go of what it had, the next acquire() says Lost, and the capture is
    /// opened again the ordinary way.
    virtual bool retake(std::string& error)
    {
        error = "this capture cannot hand the desktop over again";
        return false;
    }

    /// The D3D11 device the frames live on — created on the adapter that drives
    /// the captured display, so the encoder can attach to it with no copy.
    virtual ID3D11Device* device() const = 0;

    /// That device's immediate context. Exposed so the caller can issue GPU
    /// work against the captured textures — a copy, most usefully — without
    /// making a second context that would need its own synchronisation.
    virtual ID3D11DeviceContext* context() const = 0;

    /// The picture as the desktop stands, upright: what the session is sized
    /// by, the pointer placed in and the mouse aimed at.
    virtual int width() const = 0;
    virtual int height() const = 0;

    /// How the frames' textures stand against that picture, in quarter turns
    /// (DisplayRotation.h's quarterTurns): 0 for a picture that arrives
    /// upright — every capture's, but Desktop Duplication's of a display
    /// turned against its panel, which hands it over as the panel scans it
    /// out (issue #54). The converter turns it upright as it reads it. For an
    /// odd turn the textures are height() × width(): whatever copies one goes
    /// by the texture's own size, never by width() and height().
    virtual int turn() const { return 0; }

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

    /// The point inside the shape that IS the pointer position. Already
    /// subtracted from CursorState::x/y, so compositing does not need it — but a
    /// client drawing its own pointer does, to place the image against its
    /// cursor rather than against its top-left corner.
    virtual int cursorHotspotX() const = 0;
    virtual int cursorHotspotY() const = 0;

    /// True once the capture has shown that the pointer comes painted into the
    /// picture, where it can be neither left out nor magnified. The session
    /// answers by moving to a capture that can. See PaintedPointer.h.
    virtual bool pointerPaintedIn() const { return false; }

    /// Presents the display made that no acquire ever returned: several
    /// presents between two acquires come back as ONE frame, the latest. The
    /// sum since start(), or -1 where the capture cannot tell.
    virtual int64_t foldedPresents() const { return -1; }

    /// The display's refresh period as the mode this capture opened on says,
    /// in ns — for the lattice the stream's gate locks on
    /// (core/PresentLattice.h). 0 where the capture cannot tell.
    virtual int64_t refreshPeriodNs() const { return 0; }

protected:
    IWindowsCapture() = default;
};

} // namespace mw::native::capture
