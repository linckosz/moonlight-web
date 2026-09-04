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

#include "AudioPacket.h"
#include "Capabilities.h"
#include "EncodedFrame.h"
#include "InputEvent.h"
#include "LinkFeedback.h"
#include "SessionConfig.h"

#include <functional>
#include <memory>
#include <string>

// ─────────────────────────────────────────────────────────────────────────────
//  mw-native-host — MoonlightWeb's own capture & encoding engine.
//
//  This is the ENTIRE public surface. Everything else in this tree is an
//  implementation detail, and nothing outside it may include a header from
//  src/.
//
//  Three rules govern this module, and a build-time test enforces the first:
//
//   1. No Qt, no moonlight-common-c, no GPL dependency. Ever. The module has to
//      stay relicensable on its own (see LICENSE.md).
//   2. Detect → Optimize → Stream. The engine works out the display's GPU, the
//      capture API, the encoder, the codec and every encoding parameter by
//      itself. The only decision left to a human is WHICH DISPLAY.
//   3. Latency first. When quality and latency conflict the engine picks
//      latency, and any buffer, queue, copy or thread hop added here has to
//      justify itself.
// ─────────────────────────────────────────────────────────────────────────────

namespace mw::native {

/// Delivered on the encoder's own thread. `frame` is valid only for the
/// duration of the call — copy it or send it now (see EncodedFrame).
using VideoCallback = std::function<void(const EncodedFrame& frame)>;

/// Delivered on the audio capture thread, same lifetime rule.
using AudioCallback = std::function<void(const AudioPacket& packet)>;

/// The host asked to rumble the client's gamepad.
using RumbleCallback = std::function<void(const RumbleEvent& rumble)>;

/// The mouse pointer, when the engine is NOT drawing it into the picture.
///
/// ── Why a client would want this ────────────────────────────────────────────
///
/// A composited cursor moves at the speed of the video. On a still desktop that
/// is 2 frames a second, and it feels exactly as bad as it sounds. Handed to the
/// client instead, the pointer is drawn by the viewer's own compositor at the
/// viewer's own refresh rate, and its motion stops depending on the stream at
/// all — nothing is captured, converted, encoded or sent when only the mouse
/// moved.
///
/// The engine still tracks it; it just reports the shape rather than burning it
/// in. Sent only when something changes, which for a pointer being moved around
/// is never: one shape lasts thousands of frames.
struct CursorUpdate
{
    /// False means "draw no pointer at all" — a game that hid it, or a pointer
    /// that left this display.
    bool visible = false;

    int width = 0;
    int height = 0;
    /// The point inside the image that IS the pointer position. An arrow's tip,
    /// a crosshair's centre. Ignoring it offsets every cursor by its own shape.
    int hotspotX = 0;
    int hotspotY = 0;

    /// What to multiply this bitmap — and its hotspot — by, to bring it into the
    /// pixels of the FRAME being streamed.
    ///
    /// The two are not the same. The pointer is captured in the host desktop's
    /// own pixels, while the frame is whatever resolution the client negotiated,
    /// and the converter scales the desktop into it. A 1440p desktop streamed at
    /// 1080p arrives three quarters the size it was captured at — every window,
    /// every letter, every icon — and a pointer drawn at its captured size would
    /// be the one thing on screen that is a third too big.
    ///
    /// It is 1 whenever the two agree, which is the common case, and it changes
    /// under a running session: the host switching its own display mode moves
    /// the desktop's size without touching the frame's. An update is therefore
    /// sent whenever it changes, even if the shape did not.
    ///
    /// A ratio and not a resized bitmap: the client is already resampling — it
    /// has its own window-scale to apply on top of this one — and resizing here
    /// would cost a resample that the second one then throws away.
    float scale = 1.0f;

    /// Which of the standard system pointers this is, as a CSS cursor keyword —
    /// "default", "text", "pointer", "ew-resize"… Empty when the application
    /// uses a cursor of its own, which no name can describe.
    ///
    /// Deliberately a NAME and not just the bitmap. A client that draws the
    /// host's exact image gets a pointer that looks foreign on its own desktop —
    /// a Windows arrow on a Mac — while a client given the name can show its own
    /// native pointer and still change shape with the content underneath. Both
    /// are legitimate; the client chooses, and the host is the only place the
    /// name can be worked out.
    ///
    /// Points into static storage: valid indefinitely, unlike `pixels`.
    const char* kind = "";

    /// width × height × 4, BGRA. Valid for the duration of the call only.
    ///
    /// Already flattened: a monochrome cursor's inverting pixels are resolved to
    /// WHITE, with a black outline traced around them.
    ///
    /// Inversion cannot be expressed to a client that composites with the OS —
    /// no image format has a "flip what is behind me" pixel — so a choice has to
    /// be made, and it has to work on both backgrounds. Resolving to black alone
    /// does not: it is right on a white page and invisible on a dark one, which
    /// is the failure the inversion existed to prevent. The shapes that use it
    /// carry no outline of their own to fall back on; the text I-beam is a bare
    /// inverting stroke, which is exactly why it vanishes into a dark text field.
    ///
    /// White on black is the same trick every OS uses for the same reason (the
    /// macOS I-beam is drawn this way natively): the fill answers the dark
    /// background, the outline answers the light one, and the shape reads on
    /// anything in between.
    const uint8_t* pixels = nullptr;
};

using CursorCallback = std::function<void(const CursorUpdate& cursor)>;

/// The session ended on its own — the display went away, the encoder died, the
/// user logged out. `reason` is English, for logs. A session that ends this way
/// never calls stop() on itself; the owner still must.
using SessionEndedCallback = std::function<void(const std::string& reason)>;

/// One live capture → encode → deliver pipeline for one display.
///
/// Created through NativeHost::createSession(), which is the only way to get
/// one. Destroying it stops everything and joins every thread it owns, so a
/// caller can simply let the unique_ptr go.
class Session
{
public:
    virtual ~Session() = default;

    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

protected:
    // Declaring the copy operations above suppresses the implicit default
    // constructor, which subclasses need. Protected rather than public: a bare
    // Session is not a thing anyone should be able to make.
    Session() = default;

public:
    /// Begin capturing and encoding. Callbacks start firing before this
    /// returns is NOT guaranteed — the first frame arrives when the display
    /// next presents, which on a still screen can be a while.
    ///
    /// Returns false and fills `error` when the pipeline could not be built,
    /// in which case nothing was started and the object may be destroyed.
    virtual bool start(std::string& error) = 0;

    /// Stop everything and join. Idempotent; safe from any thread; safe to call
    /// from inside a callback.
    virtual void stop() = 0;

    /// What the engine actually settled on. Valid after a successful start().
    virtual const SessionInfo& info() const = 0;

    /// Inject one input event into the OS, on the calling thread (§8).
    /// Ignored — not queued — when the session is not running.
    virtual void sendInput(const InputEvent& event) = 0;

    /// Whether to draw the mouse pointer into the encoded picture.
    ///
    /// True — the default — burns it in, which is what a client that cannot
    /// draw its own needs, and what a gaming-mode session wants: there the
    /// viewer's real pointer is captured away by pointer lock, so the only
    /// pointer that exists is the one in the frame.
    ///
    /// False reports the shape through the CursorCallback instead and leaves the
    /// picture clean. Runtime-settable because the viewer can switch modes
    /// mid-session, and re-launching the whole pipeline over a pointer would be
    /// absurd. The next frame reflects the change.
    ///
    /// @p cursorFramePx is how WIDE the composited pointer should end up, in
    /// pixels of the frame being encoded; 0 means the size it has on the desktop.
    /// It exists for the small screen: a 32-pixel arrow inside a 1920-wide
    /// picture shown on a phone is four screen pixels across, which is not a
    /// pointer, it is a speck. The client is the only side that knows how large
    /// the picture ends up in front of a viewer — its window, its zoom, its
    /// orientation — so it asks in the one unit both sides share, and the engine
    /// works out the magnification from the shape it actually has. Ignored while
    /// the client draws its own pointer: it can size that one itself.
    virtual void setCompositeCursor(bool composite, int cursorFramePx) = 0;

    /// The lowest rate at which frames must keep arriving while nothing on the
    /// screen moves at all, in frames per second. 0 asks for the engine's own.
    ///
    /// Desktop Duplication produces a frame on damage, so a still screen
    /// produces none, and the engine re-sends the last picture just often
    /// enough for a receiver to tell a quiet stream from a dead one. That rate
    /// is right for a phone on a metered link and wrong for someone reading
    /// text at a desk: a picture only sharpens when a frame carries it (see the
    /// refinement burst in the loop), so how quickly a screen that just stopped
    /// moving settles is exactly this number.
    ///
    /// Which is why the CLIENT names it. It is the side that knows whether the
    /// viewer is on a phone or at a desk, whether they are working in a window
    /// or playing with the pointer locked, and what any of that is worth to
    /// them. The engine only clamps: never faster than the stream's own frame
    /// rate, which the viewer chose and which outranks anything asked here.
    ///
    /// Runtime-settable, like the cursor mode and for the same reason — the
    /// viewer switches modes mid-session. Takes effect on the next wake-up.
    virtual void setFrameFloorFps(int fps) = 0;

    /// Force the next frame to be a keyframe.
    ///
    /// Prefer invalidateReference() when the client can name the frame it
    /// lost: a full IDR costs ~165 KB and inflates the bitrate exactly when the
    /// link is already congested, which is the failure mode MediaTrackRelay
    /// documents in detail. This exists for the case where nothing is known —
    /// a decoder that reports itself unrecoverable, or session start.
    virtual void requestKeyframe() = 0;

    /// Tell the encoder that `frameNumber` never reached the client, so it
    /// re-encodes against an older frame that did (§9.2). Costs a few KB
    /// instead of a full keyframe. Silently degrades to requestKeyframe() on an
    /// encoder that cannot do it.
    virtual void invalidateReference(uint32_t frameNumber) = 0;

    /// Move the viewer's bitrate CEILING. Applied from the next frame, with no
    /// renegotiation of any kind — being in-process is what makes this cheap.
    /// What the encoder actually runs at is this, lowered by what the link
    /// can take (reportLink) and re-dimensioned for the rate frames really
    /// come at (encode::EffectiveCadence).
    virtual void setTargetBitrate(int kbps) = 0;

    /// What the receiver saw of the link over its last window — see
    /// LinkFeedback. The engine's rate governor (encode::RateGovernor) lowers
    /// the encoder's target when the queue builds and raises it back through
    /// quiet, between two frames, with nothing renegotiated (§9.3). Safe from
    /// any thread; a report on a session that is not running is dropped.
    virtual void reportLink(const LinkFeedback& feedback) = 0;
};

/// Entry point to the engine.
///
/// Everything here is free of side effects on the rest of MoonlightWeb: nothing
/// is written to disk, no port is opened, no process is spawned.
class NativeHost
{
public:
    /// Ask the OS what this machine can do. Cheap enough to call at startup and
    /// whenever the display layout changes; it opens no encoder session and
    /// captures no frame beyond what a capability check needs.
    ///
    /// Never throws: a failure comes back as Capabilities::available == false
    /// with a reason, because "this machine cannot do it" is a normal answer
    /// here, not a fault.
    static Capabilities probe();

    /// Ask whether a virtual gamepad can be created here, by asking the driver
    /// itself — see VirtualGamepad.
    ///
    /// Separate from probe() because the answer changes only when someone
    /// installs or removes a driver: call it at startup, and again after an
    /// install attempt. Opens no pad and creates no device.
    static VirtualGamepad probeVirtualGamepad();

    /// Build a session for `config`. Returns nullptr and fills `error` when the
    /// configuration cannot be honoured at all (unknown display, no codec in
    /// common with the client).
    ///
    /// Does NOT start it — see Session::start().
    static std::unique_ptr<Session> createSession(const SessionConfig& config,
                                                  VideoCallback onVideo, AudioCallback onAudio,
                                                  RumbleCallback onRumble, CursorCallback onCursor,
                                                  SessionEndedCallback onEnded, std::string& error);

    /// Route this module's own logging into the host application's logger.
    /// Called once at startup; without it the module logs nowhere, which is the
    /// right default for a library.
    ///
    /// `level`: 0 = debug, 1 = info, 2 = warning, 3 = error.
    static void setLogSink(std::function<void(int level, const std::string& message)> sink);

    /// Version of this module, independent of MoonlightWeb's — it may one day
    /// ship on its own.
    static const char* version();
};

} // namespace mw::native
