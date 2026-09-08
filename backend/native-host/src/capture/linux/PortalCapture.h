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
#include "PortalScreenCast.h"

#include <cstddef>

#include <cstdint>
#include <memory>
#include <string>

// The screen as the compositor hands it over, through the ScreenCast portal's
// PipeWire stream — the capture route for an AppImage, which can hold no
// capability and therefore cannot read the scanout the way KmsCapture does.
//
// ── The same shape as KmsCapture, deliberately ──────────────────────────────
//
// start / acquire / release / stop, and frames delivered as KmsFrame. That is
// not laziness: KmsFrame is already a DMA-BUF descriptor — fds, offsets,
// pitches, a fourcc and a modifier — which is exactly what PipeWire hands over
// for a screen buffer. Wearing the same shape means GlConvert and CpuConvert
// take these frames unchanged, and the session's loop does not learn a second
// vocabulary.
//
// ── What differs from KMS, and matters ──────────────────────────────────────
//
//  - The compositor decides the format and can change it mid-stream: the
//    negotiated size is not known until the first param_changed, so start()
//    waits for it rather than promising a size it has not been told.
//  - Buffers arrive by PUSH on PipeWire's own thread. acquire() therefore hands
//    back the last one that arrived, exactly like the KMS path hands back the
//    last scanout — one frame is held, and release() lets it go.
//  - Some compositors give DMA-BUF (the fast path, importable by EGL) and some
//    give shared memory. Both are carried; `dmabuf()` says which, because the
//    GPU pipeline needs the first and only the CPU pipeline can use the second.
//  - The pointer comes as METADATA beside the picture (that is what the
//    handshake asks for), so it is reported like KMS reports its cursor plane
//    and the client keeps drawing its own.

namespace mw::native::capture {

class PortalCapture
{
public:
    PortalCapture();
    ~PortalCapture();

    PortalCapture(const PortalCapture&) = delete;
    PortalCapture& operator=(const PortalCapture&) = delete;

    /// Ask the portal, connect to the node it names, and wait for the first
    /// negotiated format. ⚠️ Raises the portal's dialog unless @p restoreToken
    /// replays an earlier grant — see PortalScreenCast::start.
    ///
    /// On success the size, fourcc and buffer kind are known and stable until
    /// the compositor renegotiates.
    bool start(const std::string& restoreToken, std::string& error);

    /// The grant to store and hand back next time, so the dialog never returns.
    /// Empty when the portal issued none.
    std::string restoreToken() const;

    /// The last buffer that arrived, if there is a new one. Timeout when the
    /// screen has not changed — the same contract KmsCapture offers, and what
    /// lets the session's still-screen floor work unchanged.
    AcquireStatus acquire(int timeoutMs, KmsFrame& frame);
    void release();
    void stop();

    int width() const;
    int height() const;
    int refreshMilliHz() const;
    uint32_t fourcc() const;
    /// True when buffers arrive as DMA-BUF and the GPU pipeline can import
    /// them; false when they are shared memory and only the CPU pair can.
    bool dmabuf() const;
    /// Empty: the portal names no render node. The GPU pipeline picks one.
    std::string renderNodePath() const;
    DesktopRect desktopRect() const;
    const CursorState& cursor() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mw::native::capture
