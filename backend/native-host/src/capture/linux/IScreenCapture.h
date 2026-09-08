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

#include <cstdint>
#include <string>

// What the Linux session asks of whatever is giving it pictures.
//
// Two things answer it, for two different machines:
//
//  - KmsCapture reads the scanout buffer directly. It needs CAP_SYS_ADMIN, it
//    sees the display's own vblank, and it is what every package installs.
//  - PortalCapture asks the compositor through xdg-desktop-portal. It needs no
//    capability, which is why it is the ONLY route an AppImage can have
//    (§19.8) — and it costs a consent dialog, which is why it is the fallback.
//
// The interface is small on purpose: it is the list of questions the session
// actually asks, no more. Everything that is peculiar to one route — the card
// path and connector of KMS, the consent token of the portal — stays on the
// concrete class, where the session's start-up already knows which one it has.
//
// ⚠️ The frame type is KmsFrame for both, and that is not an oversight: it is a
// DMA-BUF descriptor, which is what BOTH routes produce (CaptureTypes.h says
// why it kept the name). Sharing it is what lets GlConvert and CpuConvert take
// frames from either without knowing which.

namespace mw::native::capture {

class IScreenCapture
{
public:
    virtual ~IScreenCapture() = default;

    /// Open the source. Everything below is only meaningful after this returns
    /// true — including the size, which the portal does not know until the
    /// compositor has negotiated it.
    virtual bool start(std::string& error) = 0;

    /// Wait up to @p timeoutMs for a new picture.
    ///
    /// Timeout is not an error and not a dropped frame: a still desktop
    /// genuinely produces nothing. PointerOnly means the desktop image is
    /// unchanged but the pointer moved — a visible change on a route that
    /// composites the cursor, which is why it is not folded into Timeout.
    virtual AcquireStatus acquire(int timeoutMs, KmsFrame& frame) = 0;

    /// Give the last frame back. Both routes hold exactly one.
    virtual void release() = 0;
    virtual void stop() = 0;

    virtual int width() const = 0;
    virtual int height() const = 0;
    virtual int refreshMilliHz() const = 0;
    virtual uint32_t fourcc() const = 0;

    /// The render node the converter and encoder should open. KMS names the
    /// card's own; the portal names nothing, and the caller picks.
    virtual std::string renderNodePath() const = 0;

    /// Where this picture sits on the desktop, for mapping absolute input.
    virtual DesktopRect desktopRect() const = 0;

    /// The pointer, reported beside the picture so the client can draw its own.
    virtual const CursorState& cursor() const = 0;
};

} // namespace mw::native::capture
