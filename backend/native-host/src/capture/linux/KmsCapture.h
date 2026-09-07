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
#include <vector>

// Screen capture on Linux, from the kernel's own scanout buffer.
//
// ── Why KMS and not the display server ──────────────────────────────────────
//
// The compositor composes the desktop into a framebuffer and hands it to the
// kernel to scan out. DRM/KMS lets a privileged process read WHICH buffer is on
// the screen right now (GETFB2) and export it as a DMA-BUF — the picture the
// panel shows, at the moment it shows it, with nothing copied. That is the
// Desktop Duplication of Linux: the same place in the pipeline, the same
// zero-copy property, and the same "wake on the display's own clock" through
// the vblank event.
//
// It needs CAP_SYS_ADMIN (the kernel refuses buffer handles to anyone else).
// The package delivers it through a launcher that carries the file capability
// and execs the app with it in the ambient set (a capability on the app itself
// would put glibc in secure mode and break its $ORIGIN rpath); the app keeps it
// PERMITTED only, and this file raises it into the calling thread's effective
// set around the one ioctl that checks it — Sunshine's posture. It does not need
// a display server at all — X11, Wayland or a bare console look identical from
// here — and it does not need to be inside anybody's session, which is what
// lets a host start before anyone logs in. The portal route (PipeWire) has
// neither property; it is the fallback, not the default.
//
// ── What KMS does not give ──────────────────────────────────────────────────
//
// The pointer is on its own hardware plane, so the primary buffer does not
// contain it — exactly as with Desktop Duplication — and it is read from that
// plane here: its ARGB image and its position. Its hotspot, however, is
// unknown: the compositor has already applied it before placing the plane, so
// what KMS reports is where the image goes, not where it aims. The composited
// pointer is therefore exact; a CLIENT-drawn pointer lands hotspot-offset by a
// few pixels until a source for it exists.
//
// And the scanout buffer is TILED — on AMD with DCC metadata in extra planes —
// so it cannot be handed to VA-API directly (radeonsi refuses the modifier on
// import, measured 04/09/2026). It goes through EGL, which accepts it: see
// GlConvert.

namespace mw::native::capture {

/// One scanout framebuffer, exported as DMA-BUF planes. Borrowed: the fds
/// belong to the capture and are valid only until release().
struct KmsFrame
{
    int width = 0;
    int height = 0;
    /// DRM fourcc of the buffer, e.g. DRM_FORMAT_XRGB8888.
    uint32_t fourcc = 0;
    /// Layout of the memory, DRM_FORMAT_MOD_*. Tiled on every real desktop, and
    /// the whole reason the frame goes to EGL rather than straight to VA-API.
    uint64_t modifier = 0;

    /// Planes as GETFB2 reports them. A DCC-compressed AMD buffer has three:
    /// the pixels, then two of compression metadata — and an importer that is
    /// only told about the first gets EGL_BAD_MATCH, not a picture.
    int planeCount = 0;
    int fds[4] = {-1, -1, -1, -1};
    uint32_t offsets[4] = {};
    uint32_t pitches[4] = {};

    /// When the display scanned this frame out — the vblank that presented it,
    /// on the engine's steady clock. A real measurement, like DDA's
    /// LastPresentTime, not the moment we noticed.
    int64_t presentUs = 0;
    int64_t capturedUs = 0;
};

/// One display as KMS sees it, for the probe and for choosing what to capture.
struct KmsOutput
{
    std::string cardPath; ///< /dev/dri/cardN
    uint32_t connectorId = 0;
    uint32_t crtcId = 0;
    std::string name; ///< "HDMI-A-1", "DP-2"…
    int width = 0;
    int height = 0;
    int refreshMilliHz = 0;
    int x = 0; ///< position on the virtual desktop
    int y = 0;
    bool connected = false;
    bool active = false; ///< has a CRTC with a framebuffer — something is shown
};

class KmsCapture
{
public:
    /// Every connector of one card, connected or not. Cheap; no capability
    /// needed. The probe calls this per card; the test uses it to find a display.
    static std::vector<KmsOutput> listOutputs(const std::string& cardPath, std::string& error);

    /// Whether this process may read framebuffer handles — CAP_SYS_ADMIN, or
    /// DRM master. Answered by trying, on the first CRTC that has a buffer:
    /// GETFB2 succeeds for anyone, but hands a zero handle to the unprivileged.
    static bool canReadFramebuffers(const std::string& cardPath, std::string& why);

    KmsCapture(std::string cardPath, uint32_t connectorId);
    ~KmsCapture();

    KmsCapture(const KmsCapture&) = delete;
    KmsCapture& operator=(const KmsCapture&) = delete;

    bool start(std::string& error);

    /// Wait up to @p timeoutMs for a NEW scanout buffer. Blocks on the CRTC's
    /// vblank, not on a timer: the compositor page-flips at vblank, so this
    /// wakes on the display's own clock and sees the new buffer within one
    /// vblank of it being shown. A still desktop flips nothing and answers
    /// Timeout — or PointerOnly when only the cursor plane moved.
    AcquireStatus acquire(int timeoutMs, KmsFrame& frame);

    /// Close the frame's fds early. Optional: acquire() closes the previous
    /// frame itself when a new buffer replaces it, and a caller that wants to
    /// re-convert the last picture (pointer moved, screen did not) simply keeps
    /// it. Call this to give the buffer back sooner than that.
    void release();
    void stop();

    /// The DRM card fd, for callers that need the same device — the render
    /// node path is what EGL and VA-API want, and it is derived from this.
    int cardFd() const { return m_Card; }
    std::string renderNodePath() const { return m_RenderNode; }

    int width() const { return m_Width; }
    int height() const { return m_Height; }
    int refreshMilliHz() const { return m_RefreshMilliHz; }
    uint32_t fourcc() const { return m_Fourcc; }
    DesktopRect desktopRect() const { return m_Rect; }

    const CursorState& cursor() const { return m_Cursor; }

private:
    /// Resolve connector → encoder → CRTC → the primary and cursor planes
    /// attached to that CRTC. Re-run on every start(), because a mode change
    /// can re-route all of it.
    bool resolveTopology(std::string& error);
    /// Export the primary plane's current buffer into @p frame.
    bool exportFramebuffer(uint32_t fbId, KmsFrame& frame, std::string& error);
    /// Read the cursor plane: position every time, image when its buffer changed.
    /// Returns true when anything visible about the pointer moved.
    bool updateCursor();
    void closeFrameFds();

    std::string m_CardPath;
    std::string m_RenderNode;
    uint32_t m_ConnectorId = 0;

    int m_Card = -1;
    uint32_t m_CrtcId = 0;
    int m_CrtcIndex = -1;
    uint32_t m_PrimaryPlane = 0;
    uint32_t m_CursorPlane = 0;
    /// Property ids of the cursor plane, looked up once: the position is read
    /// every acquire, and a name lookup per read would be a dozen ioctls.
    uint32_t m_PropCrtcX = 0;
    uint32_t m_PropCrtcY = 0;
    uint32_t m_PropFbId = 0;

    int m_Width = 0;
    int m_Height = 0;
    int m_RefreshMilliHz = 0;
    uint32_t m_Fourcc = 0;
    DesktopRect m_Rect;

    /// The buffer last handed out, so an unchanged one is a Timeout and not a
    /// duplicate frame.
    uint32_t m_LastFbId = 0;

    // ── Drivers without a vblank ────────────────────────────────────────────
    //
    // A virtual display adapter (hyperv_drm, the Hyper-V bench) has no vblank
    // interrupt to wait on — drmWaitVBlank fails on the first call — and ONE
    // framebuffer the compositor draws into in place, so its id never changes
    // either. Neither of the two signals acquire() normally reads exists. What
    // does exist is the picture: in this mode the capture wakes on its own
    // clock at the display's rate and folds the mapped buffer into a
    // fingerprint; a new fingerprint is a new frame, the same one is a Timeout.
    // About a millisecond per poll for an 8 MB buffer, on a machine that has
    // no GPU to spare it anyway.
    bool m_Polled = false;
    int64_t m_NextPollUs = 0;
    /// The frame last exported, re-handed out in polled mode when its content
    /// changed under the same id.
    KmsFrame m_LastFrame;
    uint64_t m_LastFingerprint = 0;
    /// The held buffer's first plane, mapped for the life of the hold.
    void* m_HeldMap = nullptr;
    size_t m_HeldMapLength = 0;
    /// Fold the held mapping into one number; 0 when nothing is mapped.
    uint64_t fingerprintHeld();
    /// The vblank sequence at start, for converting reply timestamps.
    int64_t m_SteadyOriginUs = 0;

    /// fds of the frame currently held, closed by release().
    int m_HeldFds[4] = {-1, -1, -1, -1};
    int m_HeldCount = 0;

    CursorState m_Cursor;
    uint32_t m_CursorFbId = 0;
    int m_CursorPlaneX = 0;
    int m_CursorPlaneY = 0;
};

} // namespace mw::native::capture
