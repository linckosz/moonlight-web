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
#include "IScreenCapture.h"
#include "PortalScreenCast.h"

#include <cstddef>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

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
//    A compositor hands a DMA-BUF only to a client that names the modifiers it
//    can import (PipeWire's DMA-BUF negotiation): GNOME gave shared memory to
//    this route until it did (offerDmabuf, 29/09/2026).
//  - The pointer comes as METADATA beside the picture (that is what the
//    handshake asks for), so it is reported like KMS reports its cursor plane
//    and the client keeps drawing its own.

namespace mw::native::capture {

class PortalCapture final : public IScreenCapture
{
public:
    PortalCapture();
    ~PortalCapture() override;

    PortalCapture(const PortalCapture&) = delete;
    PortalCapture& operator=(const PortalCapture&) = delete;

    /// The grant to replay so the consent dialog never returns. Set before
    /// start(); it is the one thing this route needs that KMS does not, which
    /// is why it is here and not on the interface.
    void setRestoreToken(std::string token);

    /// Capture a VIRTUAL monitor the compositor creates for this session, at
    /// @p width x @p height and @p fps — the size and cadence are what this
    /// side asks for in the PipeWire format, which is how the compositor sizes
    /// it. Set before start().
    void setVirtualMonitor(int width, int height, int fps);

    /// Make that virtual monitor through KWin rather than the portal (KDE
    /// Plasma 6, KwinVirtualOutput.h): an output named after @p name, at the
    /// size setVirtualMonitor gave. Its stream is read on the session's own
    /// PipeWire. Before start().
    void setKwinVirtualOutput(std::string name);

    /// Ask Mutter itself rather than the portal — GNOME's own screen cast,
    /// with no dialog (MutterScreenCast.h): the virtual monitor
    /// setVirtualMonitor describes or, with @p connector, the monitor of that
    /// name as it is — another stream's virtual one, for a guest — whose
    /// format is the monitor's own. Its stream is read on the session's own
    /// PipeWire, and a session Mutter closes is a lost display. Before start().
    /// @p realMonitor: a screen of the desktop ("Screen" without the scanout),
    /// whose pointer stays beside the picture, as on any real monitor.
    void setMutter(std::string connector, bool realMonitor = false);

    /// Read gamescope's own node @p node instead — an app in its own gamescope
    /// (GamescopeSession.h), on the session's PipeWire, no portal, nobody to
    /// ask — and its pointer from its Xwayland, @p xDisplay (XFixesCursor.h).
    /// gamescope's process @p pid gone is the picture lost: its stream only
    /// pauses. Before start().
    void setGamescope(uint32_t node, std::string xDisplay, int pid);

    /// What a GPU can import, offered to the compositor for DMA-BUF: its
    /// render node, and per DRM fourcc the modifiers (GlConvert::
    /// importableModifiers). Offered before shared memory, which stays the
    /// fallback: a compositor that takes none of them hands that over.
    struct DmabufOffer
    {
        std::string renderNode;
        std::vector<std::pair<uint32_t, std::vector<uint64_t>>> modifiers;
        /// The bench's portaldmabuf=1: offered even where the frames keep
        /// trails of the pointer (GNOME's virtual monitor before GNOME 48), to
        /// measure what the shared memory taken there instead costs.
        bool evenWithTrails = false;
    };
    /// Set before start(). Without an offer the portal is asked for shared
    /// memory only, as this route always did.
    void offerDmabuf(DmabufOffer offer);
    /// The render node of the GPU that converts and encodes, DMA-BUF or not:
    /// the Vulkan conversion reads shared memory too (C13.10), on that GPU.
    void setRenderNode(std::string renderNode);

    /// Ask the portal, connect to the node it names, and wait for the first
    /// negotiated format. ⚠️ Raises the portal's dialog unless a restore token
    /// was set — see PortalScreenCast::start.
    ///
    /// On success the size, fourcc and buffer kind are known and stable until
    /// the compositor renegotiates.
    bool start(std::string& error) override;

    /// The grant to store, after a start that raised the dialog. Empty when the
    /// portal issued none — in which case it will ask again next time.
    std::string restoreToken() const;

    /// After a start() on Mutter's route that failed: Mutter refused the route
    /// outright, where the portal may still serve (MutterScreenCast::refused).
    bool mutterRefused() const;

    /// The last buffer that arrived, if there is a new one. Timeout when the
    /// screen has not changed — the same contract KmsCapture offers, and what
    /// lets the session's still-screen floor work unchanged.
    AcquireStatus acquire(int timeoutMs, KmsFrame& frame) override;
    void release() override;
    void stop() override;

    int width() const override;
    int height() const override;
    int refreshMilliHz() const override;
    uint32_t fourcc() const override;
    /// True when buffers arrive as DMA-BUF and the GPU pipeline can import
    /// them; false when they are shared memory and only the CPU pair can.
    /// Not on the interface: it is the one thing the session must ask THIS
    /// route, to know which pipeline pair can take its frames. Known when
    /// start() returns: it waits for the first buffer.
    bool dmabuf() const;
    /// GNOME's virtual monitor before GNOME 48 with a DMA-BUF offer: its
    /// DMA-BUF frames keep trails of a pointer that moves over a still
    /// desktop, so it starts in shared memory, and setDmabufWhilePointerHidden
    /// moves it to DMA-BUF and back. False everywhere else, and before start().
    bool switchesDmabufWithPointer() const;
    /// Offer DMA-BUF again while the pointer is hidden (@p hidden), shared
    /// memory alone while it may show: the formats offered again on the live
    /// stream, which the compositor renegotiates in place. True when the
    /// offer changed; the buffers that follow say what was settled on
    /// (dmabuf()). Capture thread.
    bool setDmabufWhilePointerHidden(bool hidden);
    /// The compositor has renegotiated the format again and again since
    /// start() without a single picture: it settles on one it cannot fill —
    /// KWin with a DMA-BUF modifier its renderer cannot allocate (Plasma 6.3
    /// in a VM, hundreds a second). A slow first picture is not this: one
    /// format, then the wait.
    bool renegotiatingWithoutPicture() const;
    /// The GPU the session named (setRenderNode), else the offer's when the
    /// buffers are DMA-BUF; empty when neither says one.
    std::string renderNodePath() const override;
    DesktopRect desktopRect() const override;
    const CursorState& cursor() const override;
    /// A GNOME virtual monitor before GNOME 48: its pointer is asked painted
    /// into every picture (MutterScreenCast::embedsPointer), and its frames
    /// are taken in shared memory while it may show — Mutter's DMA-BUF ones
    /// leave trails of it (setDmabufWhilePointerHidden). Never on a monitor
    /// KWin makes, nor from GNOME 48. And any stream of a portal that hands
    /// over no pointer beside the pictures (PortalStream::cursorMode), such as
    /// the wlroots one: it is painted in, and the client draws none.
    bool cursorInPicture() const override;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mw::native::capture
