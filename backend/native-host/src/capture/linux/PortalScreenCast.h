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

#include <cstdint>
#include <memory>
#include <string>

// The xdg-desktop-portal ScreenCast handshake: what turns "may I capture the
// screen?" into a PipeWire node the stream can read.
//
// ── Why this exists at all ──────────────────────────────────────────────────
//
// KmsCapture reads the scanout directly and needs CAP_SYS_ADMIN. An AppImage
// can carry no capability — FUSE mounts it nosuid (§19.8) — so on that one
// packaging the portal is the ONLY route to a picture. Everywhere else KMS is
// better and is tried first: no dialog, no compositor in the path, no session
// that dies when the portal does.
//
// ── ⚠️ The licence, in one paragraph ────────────────────────────────────────
//
// The portal is reachable only over D-Bus, and every C way to speak D-Bus is
// copyleft or worse: libdbus is `GPL-2+ or AFL-2.1` (the GPL arm would end this
// module's separability, the AFL arm is GPL-incompatible), GDBus and sd-bus are
// LGPL. sd-bus was chosen on 08/09/2026 as a deliberate, BOUNDED exception —
// one library, this file only, behind MW_NATIVE_LINUX_PORTAL. LICENSE.md
// § "L'exception sd-bus" says exactly what it costs and what it does not.
//
// ── The shape of the conversation ───────────────────────────────────────────
//
// A portal method does not answer: it returns an object PATH, and the real
// answer arrives later as a Response signal on that path. So each step must
// derive the path it is about to be answered on, subscribe to it, and only then
// make the call — a subscription placed after the call misses the reply and
// waits for ever. The path is built from our own unique bus name (leading ':'
// dropped, dots turned into underscores) and a token we choose.
//
//   CreateSession   → a session handle
//   SelectSources   → what to capture: one monitor, cursor as METADATA (the
//                     shape and position come beside the picture, so the
//                     client keeps drawing its own pointer as everywhere else)
//   Start           → ⚠️ RAISES A DIALOG. The user picks a screen and accepts.
//                     Answers with the PipeWire node id, and — because we ask
//                     for persist_mode 2 — a restore token that makes every
//                     later session silent.
//   OpenPipeWireRemote → the fd to connect a PipeWire client on.
//
// The dialog is not a detail: it is why the portal is a fallback and not the
// default. A host that asks permission at every launch is not the zero-setup
// product the mission describes.

namespace mw::native::capture {

/// What a completed handshake yields.
struct PortalStream
{
    /// The PipeWire node to connect to. Zero when nothing was granted.
    uint32_t nodeId = 0;

    /// The fd for pw_context_connect_fd(). Owned by the caller once returned:
    /// close it, or hand it to PipeWire which will.
    int pipewireFd = -1;

    /// Opaque, from the portal. Store it and give it back next time to skip
    /// the dialog. Empty when the portal declined to issue one.
    std::string restoreToken;

    /// The size the portal reports for the stream, when it says so. Zero means
    /// "ask PipeWire once the format is negotiated" — which is the normal case.
    int width = 0;
    int height = 0;

    bool valid() const { return nodeId != 0 && pipewireFd >= 0; }
};

class PortalScreenCast
{
public:
    PortalScreenCast();
    ~PortalScreenCast();

    PortalScreenCast(const PortalScreenCast&) = delete;
    PortalScreenCast& operator=(const PortalScreenCast&) = delete;

    /// Whether a ScreenCast portal answers on this machine right now. Cheap:
    /// one property read, no session, no dialog. False on a headless box, on a
    /// desktop without a portal backend, and inside a service with no session
    /// bus — all of which mean "use KMS or nothing".
    ///
    /// ⚠️ Also false in any process the kernel marked AT_SECURE — one that
    /// gained privilege from a FILE capability — because libsystemd reads the
    /// bus address with secure_getenv and gets nothing. That is not the case
    /// for MoonlightWeb as it ships (`moonlightweb-launch` passes the capability
    /// over an exec that gains nothing, so AT_SECURE is 0, measured 08/09/2026)
    /// but it IS the case for anything setcap'd directly, the test binary
    /// included. A false answer here on a desktop that plainly has a portal is
    /// almost always this.
    static bool available(std::string& reason);

    /// Run the whole handshake. Blocking, and ⚠️ WAITS ON A HUMAN unless
    /// @p restoreToken replays an earlier grant: the portal shows a dialog and
    /// nothing comes back until it is answered. @p timeoutMs bounds that wait.
    ///
    /// Returns false with @p error set when the portal is absent, the user
    /// declines, or the wait runs out.
    bool start(const std::string& restoreToken, int timeoutMs, PortalStream& out,
               std::string& error);

    /// Drop the session. The portal ends the cast; PipeWire's node goes away.
    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mw::native::capture
