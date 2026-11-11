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
#include "FpsStep.h"
#include "InputEvent.h"
#include "LinkFeedback.h"
#include "MouseKeys.h"
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
///
/// The POSITION travels too, but apart and sparingly — see `positionOnly`. A
/// client with a real pointer never needs it. A client on a touch screen that
/// draws its own pointer moves it from its own finger and uses the host's word
/// only to correct the drift, so that word is throttled (CursorPositionGate)
/// and carries nothing but the position.
struct CursorUpdate
{
    /// False means "draw no pointer at all" — a game that hid it, or a pointer
    /// that left this display.
    bool visible = false;

    /// True for a position report: `visible`, `x` and `y` are meaningful and
    /// nothing else is — no shape, no pixels, no hotspot. Sent while the client
    /// draws the pointer, at most every CursorPositionGate::kIntervalUs, and
    /// only when the position changed.
    bool positionOnly = false;

    /// Where the pointer is, in FRAME pixels: the hotspot itself, not the
    /// image's corner, so a client places it with no knowledge of the shape.
    /// Only with `positionOnly`.
    float x = 0.0f;
    float y = 0.0f;

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

/// Whether the viewer's keyboard and mouse currently reach the host, and why
/// not when they do not. Reported on change only.
///
/// Two things close the gate, and the viewer cannot tell them apart from the
/// picture — a frozen cursor looks the same either way — which is why this
/// exists:
///
///  - "policy": the focused window runs elevated (as administrator) and the
///    session was opened without SessionConfig::allowElevatedInput. The engine
///    COULD inject and chooses not to: a viewer who is not the machine's
///    administrator does not get to type into its administrator windows.
///  - "uipi": the focused window runs elevated and the engine's own process
///    does not, so the OS drops the injection itself (Windows User Interface
///    Privilege Isolation). Nothing the engine can do; the process has to run
///    elevated.
///
/// Pointer MOTION is never gated — moving over a window changes nothing on the
/// host — and neither are releases, so a key held when the gate closed is
/// still let go. Presses, text, scrolling: gated. A click on another, ordinary
/// window still lands, and reopens the gate by taking focus away.
struct InputGate
{
    bool blocked = false;
    /// "policy" or "uipi" when blocked; "" when open. Static storage.
    const char* reason = "";
    /// The window in the way, for the viewer: its title and executable.
    std::string window;
};

using InputGateCallback = std::function<void(const InputGate& gate)>;

/// The host's display changed under a running session: its mode, its shape,
/// or whether it is in HDR. Reported once the capture is running again on the
/// new mode, and only when one of these fields moved.
///
/// Two things a viewer cannot learn from the picture alone, which is why this
/// exists. The frame's shape may not have followed (SessionConfig::
/// followDisplayShape off), and an HDR switch changes nothing a decoder can
/// see in an SDR session: DXGI tone-maps the new HDR desktop into the same
/// 8-bit frames. The client weighs this against its own screen and decides
/// whether a new session is worth it.
struct DisplayFormat
{
    /// The desktop, in the display's own pixels.
    int displayWidth = 0;
    int displayHeight = 0;
    /// What is encoded from now on.
    int frameWidth = 0;
    int frameHeight = 0;
    /// The display is in an HDR mode now.
    bool displayHdr = false;
    /// The frames are HDR now. An HDR session whose display left HDR drops to
    /// SDR on its own; an SDR session never rises to HDR on its own — the
    /// client relaunches, because its renderer is chosen at stream start.
    bool hdr = false;
    /// Same as SessionInfo::hdrCapable.
    bool hdrCapable = false;
};

using DisplayFormatCallback = std::function<void(const DisplayFormat& format)>;

/// The session ended on its own — the display went away, the encoder died, the
/// user logged out. `reason` is English, for logs. A session that ends this way
/// never calls stop() on itself; the owner still must.
using SessionEndedCallback = std::function<void(const std::string& reason)>;

/// A consent to remember — see Session::setPortalGrantCallback. The string is
/// opaque: it means something to the desktop portal that issued it and to
/// nothing else, so it is stored and handed back verbatim.
using PortalGrantCallback = std::function<void(const std::string& token)>;

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
    /// picture clean; the position goes the same way, throttled, for a client
    /// that draws its own pointer with no pointer device to move it.
    /// Runtime-settable because the viewer can switch modes mid-session, and
    /// re-launching the whole pipeline over a pointer would be absurd. The next
    /// frame reflects the change.
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

    /// The client's display changed under a running session — its window
    /// moved to another screen, or its refresh rate changed. Same contract as
    /// SessionConfig::clientRefreshMilliHz / clientVsync: the stream's cadence
    /// is re-chosen against it (§9.11), between two frames, and the encoder's
    /// per-frame budget follows. Nothing else moves — the encoder keeps the
    /// rate it was built for. Safe from any thread; ignored when the session
    /// runs at the host's own rate.
    virtual void setClientRefresh(int milliHz, bool vsync) = 0;

    /// The client cannot take the frame rate the viewer set — its decoder
    /// keeps a queue it never empties — and asks for no more than @p fps.
    /// Zero lifts the cap. It only ever lowers the rate: the cadence is
    /// re-chosen between two frames like a client screen that changed, and the
    /// encoder's per-frame budget follows. Safe from any thread.
    virtual void setClientFpsCap(int fps) = 0;

    /// How many frames wait at the client's decoder input, said the moment a
    /// second one does and again when it is back to one (a `decodequeue`
    /// message, frontend DecodeQueueSignal.js). Read only under the bench's
    /// cadence=host-guarded, which skips presents while it is full (design
    /// §33); every other cadence, and every platform but Windows, ignores it.
    /// Safe from any thread.
    virtual void setClientDecodeQueue(int /*depth*/) {}

    /// Whether video waits outside the transport's own buffer right now —
    /// asked by the capture loop at each picture, from its thread, so it must
    /// be cheap and safe from any thread. Empty: never.
    using LinkBusyProbe = std::function<bool()>;

    /// The relay's view of its send queue, for the bench's linkhold= (plan
    /// Wi-Fi W2 C): while @p probe answers true, a picture is held, not
    /// encoded, and the freshest goes once it answers false — what the
    /// host-guarded cadence does for the client's decoder, done for the link.
    /// Every platform but Windows ignores it. Safe from any thread.
    virtual void setLinkBusyProbe(LinkBusyProbe /*probe*/) {}

    /// When the client's screen refreshes (a `vsyncgrid` message, frontend
    /// VsyncGrid.js): every @p periodUs, one refresh at @p phaseUs on this
    /// host's steady clock, a frame needing @p leadUs from being taken here to
    /// being ready there; whether its canvas is @p tearing, whether its link
    /// is @p steady enough to aim through, and the frames a second it takes
    /// when frames go as they come (@p budgetFps). Read only under the bench's
    /// cadence=deadline, which takes one picture per refresh that lead before
    /// it, or sends each picture as it comes to a canvas that tears or over an
    /// uneven link (DeadlineCadence.h); every other cadence, and every platform
    /// but Windows, ignores it. Safe from any thread.
    virtual void setClientVsyncGrid(double /*periodUs*/, int64_t /*phaseUs*/, int64_t /*leadUs*/,
                                    bool /*tearing*/, bool /*steady*/, double /*budgetFps*/)
    {}

    /// What each pong tells the client about its grid.
    struct VsyncGridStatus
    {
        /// The cadence would aim at a grid: the client should send one.
        bool wanted = false;
        /// The client's grid is being followed right now.
        bool followed = false;
        /// ...by aiming each picture at a refresh (vsync), not by sending each
        /// as it comes (a canvas that tears, an uneven link).
        bool aimed = false;
        /// The host display's refresh period, µs; 0 when unknown.
        int presentUs = 0;
    };
    /// Safe from any thread.
    virtual VsyncGridStatus vsyncGridStatus() const { return {}; }

    /// "Auto" with detection (an `fpsstep` message, frontend CadenceStepper.js):
    /// the client asks for the stream to run at @p fps, above its own rate,
    /// while it measures whether what it shows gets younger — or, with 0, to
    /// come back to its own rate. Answered at once (FpsStep: applied, capped at
    /// the display's refresh, or refused and why); the loop applies it between
    /// two frames, and the bitrate does not move. Only the Windows engine steps;
    /// elsewhere every step is refused and the stream keeps its cadence. Safe
    /// from any thread.
    virtual FpsStep setClientFpsStep(int fps)
    {
        FpsStep refused;
        refused.askedFps = fps;
        refused.why = "this platform keeps its cadence";
        return refused;
    }

    /// What the client's detection reads of the cadence, once a second with
    /// the stats: how fast the content changes, the stream's own rate, the
    /// step in force. Zero-filled where nothing steps. Safe from any thread.
    virtual CadenceStatus cadenceStatus() const { return {}; }

    /// Whether the capture held its display over the session's dense content
    /// (core/CaptureHold.h), for the server's `vddmatch`: a host that did not
    /// hold a display at twice the client's rate gets the client's own from
    /// the next stream on.
    struct CaptureHoldStatus
    {
        /// Enough dense content to say anything.
        bool judged = false;
        /// Within every threshold; true when not judged.
        bool held = true;
        /// The captured display's refresh, mHz; 0 when unknown.
        int displayMilliHz = 0;
        /// The GPU that captured and encoded it.
        std::string gpu;
        int denseSeconds = 0;
        /// Of the gate's calls, 0..1; -1 when the gate was never called.
        double locked = -1;
        double carried = 0;
        double folded = 0;
        double busy = 0;
        /// The first threshold missed; empty when held.
        std::string why;
    };
    /// Final once stop() returned; before, it says nothing. Only the Windows
    /// engine judges.
    virtual CaptureHoldStatus captureHold() const { return {}; }

    /// Where to hear that the viewer's input stopped reaching the host, or
    /// started again — see InputGate. Delivered on the thread that injects,
    /// i.e. the caller's own sendInput() thread, at most once per change.
    /// Optional: a session with no listener still gates, it just says nothing.
    /// Only the Windows engine has anything to report today; elsewhere this
    /// is accepted and never called.
    virtual void setInputGateCallback(InputGateCallback callback) { (void)callback; }

    /// Where to hear that the host's display changed mode, shape or dynamic
    /// range — see DisplayFormat. Delivered on the capture thread, once the
    /// capture runs again on the new mode. Optional; accepted and never called
    /// on a platform that does not watch for it.
    virtual void setDisplayFormatCallback(DisplayFormatCallback callback) { (void)callback; }

    /// Where to hear that the desktop portal issued a consent worth keeping.
    ///
    /// ⚠️ Must be set BEFORE start(), because that is where the grant happens:
    /// asking for a screencast is what raises the dialog, and the token comes
    /// back with the user's answer. A listener registered afterwards is not
    /// late by a little, it has missed the only call there will ever be.
    ///
    /// Called at most once per session, on the thread that called start(), and
    /// only when the grant is NEW — a session that replayed a stored token
    /// raised no dialog and has nothing to report. Store the token and hand it
    /// back in SessionConfig::portalRestoreToken next time; that is the whole
    /// difference between one dialog per installation and one per session.
    ///
    /// Optional, and today only the Linux engine on the portal route ever
    /// calls it. Everywhere else this is accepted and ignored.
    virtual void setPortalGrantCallback(PortalGrantCallback callback) { (void)callback; }

    /// Where to hear that the session turned Windows' Mouse Keys on, so that
    /// the host draws its pointer although it has no pointing device, and
    /// what it found before (MouseKeys.h) — or that it no longer holds them.
    /// For whoever outlives the session's process: a process that dies
    /// holding them cannot put them back, its parent can (restoreMouseKeys).
    ///
    /// Must be set before start(), which may already turn them on. Called on
    /// the thread that called start(), then the capture thread, then the one
    /// that calls stop(). Only the Windows engine calls it; elsewhere it is
    /// accepted and ignored.
    virtual void setMouseKeysCallback(MouseKeysCallback callback) { (void)callback; }

    /// A session whose pictures come from elsewhere (SessionConfig::
    /// videoSource External): the display now sits at this rectangle of the
    /// host's desktop — its mode changed under the feed, which says so.
    /// Absolute pointer positions land there from the next one. Safe from any
    /// thread; ignored by a session that captures, whose own capture knows.
    virtual void setExternalDesktop(int left, int top, int right, int bottom)
    {
        (void)left;
        (void)top;
        (void)right;
        (void)bottom;
    }

    /// Get the viewer out of a closed gate, when they cannot click their way
    /// out because the pointer itself is stuck.
    ///
    /// A window at a higher integrity level takes not just its own input but
    /// ALL of it: Windows refuses the whole SendInput call while it holds the
    /// foreground, so the pointer does not move, so the viewer cannot click on
    /// anything else, so the foreground never changes. That is a dead end, and
    /// no amount of input can open it — the way out has to not be input.
    ///
    /// So this asks the SHELL to minimise the desktop's windows. The shell is
    /// allowed to do to that window what MoonlightWeb is not, exactly as it is
    /// when the viewer presses Win+D on a keyboard of their own; the window in
    /// the way loses the foreground, and the session comes back to life.
    ///
    /// Minimising only the offending window would be the polite version and is
    /// not available: ShowWindow across integrity levels is dropped exactly
    /// like SendInput, measured on Windows 11 on 07/09/2026. What is done
    /// instead is to note which windows were up, minimise everything, and put
    /// back the ones that can be put back — so the desktop comes home minus the
    /// window in the way, and minus any other elevated window, which cannot be
    /// restored for the same reason it could not be minimised alone.
    ///
    /// The client offers it under the policy gate as well (an invited player
    /// before an administrator window, 22/10/2026): the pointer works there,
    /// but a game run as administrator that fills the screen leaves nothing
    /// else to click. The same act, the owner's game minimised with the rest.
    ///
    /// The gate is re-reported when it is over, so a viewer who is not touching
    /// anything still learns whether it worked. Returns false when the gate is
    /// not closed, and where the platform has no such thing — today, everywhere
    /// but Windows.
    virtual bool releaseInputBlock() { return false; }

    /// The bench's click trace (EncoderTuning::clickTrace) as CSV text, empty
    /// when it was not asked for or the platform has none. The engine writes
    /// nothing to disk: the consumer puts it next to its log.
    virtual std::string clickTraceCsv() const { return {}; }
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

    /// Report, for every printable key injected, how THIS host's own keyboard
    /// layout read it: the character a text field will show, the physical key a
    /// game will see, and whether the character the client asked for is the one
    /// that comes back out.
    ///
    /// Only this module can answer that question — the layout is the host's,
    /// and a client can at best predict. Off by default: it is one log line per
    /// keystroke, for a bench session, not for a stream someone is playing on.
    ///
    /// Process-wide rather than per session: it is set once from the settings
    /// file at startup, like the rest of the module's diagnostics, and a knob
    /// that had to be threaded through SessionConfig would have to be threaded
    /// through three platform sessions to reach the one place that can use it.
    static void setKeyboardDiagnostics(bool on);
    static bool keyboardDiagnostics();

    /// How this process has Ctrl+Alt+Suppr pressed on its host (Windows).
    /// Windows honours SendSAS from a service in session 0 only, which a
    /// worker in the console session is not, SYSTEM as it may be: the host
    /// application hands the press to its launcher service through @p sender,
    /// which returns false with the reason in its argument. Unset, the engine
    /// calls SendSAS itself. Either way, only a SYSTEM worker asks: it alone
    /// can follow the screen that opens, and take the Esc that closes it.
    static void setSecureAttentionSender(std::function<bool(std::string& error)> sender);

    /// Whether the input is on a desktop other than the user's own: on Windows
    /// the secure one, `Winlogon`, where the lock screen's PIN and the UAC
    /// prompt's password are typed — or one this process cannot even read,
    /// which from below SYSTEM is that same desktop. The keyboard diagnostics,
    /// this module's and the input codec's, keep silent while it answers true:
    /// what is typed there never belongs in a log, whatever a settings file
    /// says. Asked live, on a key press, only when the diagnostics are on.
    /// False on the other platforms.
    static bool secureDesktopHasInput();

    /// Sign-in screen mode, for the rest of the process (Windows): the
    /// sessions it creates send a picture and inject an input only while the
    /// sign-in screen shows and nobody is signed in on the session, and end
    /// for good the first time that is not so. No audio either. One way:
    /// nothing leaves it. The host application calls it from its worker's
    /// command line, which only its launcher service writes, before any
    /// session exists. Returns false where the mode does not exist (every
    /// other platform), and the caller then must not stream at all.
    static bool enterSignInScreenOnly();
    static bool signInScreenOnly();

    /// Version of this module, independent of MoonlightWeb's — it may one day
    /// ship on its own.
    static const char* version();
};

} // namespace mw::native
