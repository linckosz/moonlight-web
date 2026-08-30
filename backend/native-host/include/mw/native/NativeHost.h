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

    /// Move the target bitrate. Applied from the next frame, with no
    /// renegotiation of any kind — being in-process is what makes this cheap
    /// enough to drive from live client feedback (§9.3).
    virtual void setTargetBitrate(int kbps) = 0;
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

    /// Build a session for `config`. Returns nullptr and fills `error` when the
    /// configuration cannot be honoured at all (unknown display, no codec in
    /// common with the client).
    ///
    /// Does NOT start it — see Session::start().
    static std::unique_ptr<Session> createSession(const SessionConfig& config,
                                                  VideoCallback onVideo, AudioCallback onAudio,
                                                  RumbleCallback onRumble,
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
