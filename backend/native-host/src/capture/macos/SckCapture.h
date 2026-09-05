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

#include <CoreVideo/CoreVideo.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

// Screen capture on macOS, through ScreenCaptureKit.
//
// ── Why the converter stage does not exist here ─────────────────────────────
//
// On Windows and Linux the capture hands out the desktop as it is scanned out
// (BGRA, or a tiled XRGB DMA-BUF) and a shader turns it into the NV12 the
// encoder wants. ScreenCaptureKit is the compositor's own output path, and the
// compositor will write NV12 directly: ask for '420v' at the stream's
// resolution and every frame arrives as an IOSurface-backed CVPixelBuffer the
// encoder can read in place — scaled, colour-converted and, when asked, with
// the pointer already composited. So the picture goes capture → encoder with
// nothing in between, and the one copy left is the bitstream leaving VRAM.
//
// ── Push, wrapped as pull ───────────────────────────────────────────────────
//
// SCK delivers frames on a dispatch queue. The session loop is written once,
// against an acquire(timeout) that blocks — because that is how DXGI and KMS
// answer — so the newest frame is parked under a mutex and acquire() waits for
// it. Nothing queues: a frame that arrives before the previous one was taken
// replaces it, which is the "latest frame, never a backlog" rule of the whole
// engine (memory: latency-first-rule).
//
// ── What SCK does not give ──────────────────────────────────────────────────
//
// A pointer shape. With showsCursor on, the pointer is in the picture; with it
// off, the picture is clean and the client draws its own — and the SHAPE for
// that comes from AppKit (NSCursor.currentSystemCursor), polled by the session,
// not from the capture. No PointerOnly status either: a pointer move under
// showsCursor is an ordinary new frame, which is exactly what the composited
// mode wants.
//
// ── The sound comes through here too ────────────────────────────────────────
//
// macOS has no loopback device to open: the only supported way to record what
// the Mac is playing is ScreenCaptureKit's own audio tap (macOS 13+), which is
// a second output on THIS stream rather than a capture of its own. That is why
// setAudioSink() lives on the screen capture: one stream, one permission, and
// the audio stops and restarts with the picture instead of drifting on when
// the display goes away. Samples arrive on a queue of their own, so a slow
// frame never delays a packet.
//
// ── Permission ──────────────────────────────────────────────────────────────
//
// Screen Recording (TCC). Refused, SCK reports every display but streams none;
// the probe answers CapturePermission and asks the OS to prompt once, the way
// every screen-sharing app on macOS has to. The audio tap rides on that same
// permission — there is no separate microphone prompt, because this is not a
// microphone.

namespace mw::native::capture {

/// One captured frame: the compositor's NV12 buffer, retained by the capture
/// until the next acquire() replaces it (or release() gives it back).
struct SckFrame
{
    /// Borrowed: valid until the next acquire() that returns Ok. NV12
    /// ('420v'), BT.709 limited range, at the size start() was given.
    CVPixelBufferRef pixels = nullptr;
    int width = 0;
    int height = 0;

    /// The compositor's own display time of this frame, on the engine's
    /// steady clock — SCK reports it in mach time, converted once.
    int64_t presentUs = 0;
    int64_t capturedUs = 0;
};

class SckCapture
{
public:
    /// Captured host audio, interleaved stereo float at 48 kHz. Delivered on
    /// SCK's audio queue, in whatever chunk sizes it feels like.
    using AudioSampleCallback = std::function<void(const float* interleaved, size_t frames)>;

    /// The refresh rate SCK will be asked to deliver at, in millihertz —
    /// the panel's own, so the session's cadence gate sees every present.
    SckCapture(uint32_t displayId, int outputWidth, int outputHeight, int refreshMilliHz,
               bool showsCursor);
    ~SckCapture();

    SckCapture(const SckCapture&) = delete;
    SckCapture& operator=(const SckCapture&) = delete;

    /// Ask this stream to carry the host's audio as well. Must be called
    /// BEFORE start(): whether a stream captures audio is fixed when it is
    /// built. A null callback turns it back off.
    void setAudioSink(AudioSampleCallback onSamples);

    /// True once start() has actually got an audio tap — false when the sink
    /// was set but the OS is older than macOS 13, which is the one case where
    /// asking for audio is not an error and still gives none.
    bool audioActive() const { return m_AudioActive; }

    /// Resolve the display in SCK's shareable content and start the stream.
    /// Blocks until the stream is running or refused (a second at most).
    bool start(std::string& error);
    void stop();

    /// Wait up to @p timeoutMs for a NEW frame. Timeout on a still screen is
    /// the common case and not an error; Lost when SCK stopped the stream
    /// (display unplugged, resolution changed, permission revoked).
    AcquireStatus acquire(int timeoutMs, SckFrame& frame);

    /// Give the held buffer back early. Optional, as on Linux: a caller that
    /// wants to re-encode the last picture simply keeps it.
    void release();

    /// Draw the pointer into the picture, or not. Applied live to the running
    /// stream; the next frame reflects it.
    bool setShowsCursor(bool shows, std::string& error);

    int width() const { return m_Width; }
    int height() const { return m_Height; }
    int refreshMilliHz() const { return m_RefreshMilliHz; }
    /// The display's rectangle on the desktop, in POINTS — what CGEvent wants
    /// for absolute pointer positions (see CgInput).
    DesktopRect desktopRect() const { return m_Rect; }

    /// Public because the Objective-C stream delegate has to reach it; opaque
    /// here.
    struct Impl;

private:
    std::unique_ptr<Impl> d;

    uint32_t m_DisplayId = 0;
    int m_Width = 0;
    int m_Height = 0;
    int m_RefreshMilliHz = 0;
    bool m_ShowsCursor = true;
    bool m_AudioActive = false;
    DesktopRect m_Rect;
};

} // namespace mw::native::capture
