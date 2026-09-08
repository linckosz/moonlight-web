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

#include <cstddef>
#include <cstdint>
#include <vector>

// The parts of a screen capture that every platform shares.
//
// The frame itself is platform-shaped on purpose — an ID3D11Texture2D on
// Windows, a set of DMA-BUF planes on Linux — because naming the real type is
// what keeps the hand-off to the encoder zero-copy (see IWindowsCapture.h). But
// what an acquire() attempt MEANT, where the display sits, and what the pointer
// looks like are the same questions on every OS, and the colour conversion and
// the session loop are written against these answers once.

namespace mw::native::capture {

/// One Linux frame, as DMA-BUF planes or as mapped memory. Borrowed: the fds
/// and the pointer belong to the capture and are valid only until release().
///
/// Named for KMS because that is where it started and what every converter and
/// the Linux session already say, but it is produced by BOTH Linux captures now
/// — the scanout reader and the portal's PipeWire stream — which is why it
/// lives here with the rest of the shared vocabulary.
struct KmsFrame
{
    int width = 0;
    int height = 0;
    /// DRM fourcc of the buffer, e.g. DRM_FORMAT_XRGB8888.
    uint32_t fourcc = 0;
    /// Layout of the memory, DRM_FORMAT_MOD_*. Tiled on every real desktop, and
    /// the whole reason a scanout frame goes to EGL rather than straight to
    /// VA-API.
    uint64_t modifier = 0;

    /// Planes as GETFB2 reports them. A DCC-compressed AMD buffer has three:
    /// the pixels, then two of compression metadata — and an importer that is
    /// only told about the first gets EGL_BAD_MATCH, not a picture.
    int planeCount = 0;
    int fds[4] = {-1, -1, -1, -1};
    uint32_t offsets[4] = {};
    uint32_t pitches[4] = {};

    /// Already-mapped pixels, when the producer has them and there is no fd to
    /// map — which happens on the PORTAL route, where a compositor may hand
    /// over shared memory rather than a DMA-BUF. Null on the KMS route, always:
    /// a scanout buffer is a dma-buf or it is nothing.
    ///
    /// A consumer that can take either should prefer this when it is set: it is
    /// the same pixels without the mmap.
    const uint8_t* mapped = nullptr;
    size_t mappedSize = 0;

    /// When the display scanned this frame out — the vblank that presented it,
    /// on the engine's steady clock. A real measurement, like DDA's
    /// LastPresentTime, not the moment we noticed.
    int64_t presentUs = 0;
    int64_t capturedUs = 0;
};

/// What one acquire() attempt produced.
enum class AcquireStatus
{
    /// A new frame is available, and release() must be called before the next
    /// acquire().
    Ok,

    /// Nothing was presented within the timeout. Not an error, and the common
    /// case on a still desktop: the display genuinely has nothing new. The
    /// caller must NOT treat this as a dropped frame.
    Timeout,

    /// Only the mouse moved or changed shape — the desktop image is untouched,
    /// so no usable frame comes with this.
    ///
    /// Worth its own status rather than being folded into Timeout: the cursor is
    /// composited by us, so a pointer move IS a visible change even though not
    /// one pixel of the desktop moved. Reporting it as "nothing happened" is
    /// what made the cursor sit still on a quiet screen.
    PointerOnly,

    /// The capture became invalid — a mode change, a resolution change, a
    /// desktop switch (UAC / lock screen), or the GPU being reset. Recoverable:
    /// the caller re-runs start() and carries on.
    Lost,

    /// Unrecoverable. `error` on the capture object says what happened.
    Failed,
};

/// The mouse pointer, as the capture reports it.
///
/// ── Why we have to draw it ourselves ────────────────────────────────────────
///
/// The captured desktop image does NOT contain the cursor. Windows composites
/// the pointer at scan-out and KMS keeps it on its own hardware plane, so a
/// captured frame is the desktop with a hole where the user is looking. The
/// pointer is handed over separately — a position, and a shape that changes
/// only when the cursor does — and compositing the two is the caller's job.
///
/// The shape arrives in several encodings, and all are reduced here to one: an
/// RGBA image plus a per-pixel invert flag. Monochrome cursors (the text I-beam,
/// most resize arrows) are the reason the flag exists — they carry no colour of
/// their own and are defined as inverting whatever is behind them, which is what
/// keeps an I-beam visible on both black and white text areas.
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

    /// How many columns (from the left) and rows (from the top) actually hold
    /// ink — a pixel that is either coloured or inverting. Zero when the shape
    /// is empty.
    ///
    /// The buffer is not the pointer: Windows pads an arrow into a 32×32 (or
    /// 48, or 64 on a scaled display) canvas and only a corner of it is drawn.
    /// Anything sized from `width` — a pointer magnified to a target size on a
    /// phone — would change size with the padding, not with the pointer, and two
    /// arrows that look the same on the desktop would come out at two sizes.
    /// Both axes, because shapes are not square: an I-beam is tall and narrow,
    /// a horizontal resize arrow wide and flat, and sizing on one axis alone
    /// blows the other kind up.
    int inkWidth = 0;
    int inkHeight = 0;

    /// width × height × 4, BGRA order. Alpha is real coverage: colour cursors
    /// antialias their edges.
    std::vector<uint8_t> pixels;

    /// width × height, 255 where the pixel inverts the background instead of
    /// replacing it. Zero everywhere for an ordinary colour cursor.
    std::vector<uint8_t> invert;

    /// Bumped every time `pixels`/`invert` change. Lets the consumer re-upload
    /// the small textures only when the shape actually changed — a moving
    /// cursor keeps the same shape for thousands of frames.
    uint64_t shapeVersion = 0;
};

/// Where a display sits on the desktop.
///
/// ── On Windows this is the DPI-VIRTUALIZED rectangle, on purpose ────────────
///
/// DXGI reports desktop coordinates through the same DPI virtualization that
/// made a 2560×1440 monitor at 125% measure 2048×1152 — the bug that sent the
/// probe to QueryDisplayConfig for the real mode. Do not "fix" this one the
/// same way. SendInput's absolute coordinates are expressed against
/// SM_XVIRTUALSCREEN/SM_CXVIRTUALSCREEN, which are virtualized identically, so
/// the two agree exactly. Substituting the true pixel size here would put the
/// cursor in the wrong place on every scaled display.
///
/// On Linux it is the CRTC's position and mode: KMS has no virtualization.
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

} // namespace mw::native::capture
