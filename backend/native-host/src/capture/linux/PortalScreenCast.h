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
// one library, behind MW_NATIVE_LINUX_PORTAL, in this file, in
// MutterDisplayConfig.cpp (GNOME's layout around the virtual monitor) and in
// MutterScreenCast.cpp (GNOME's own screen cast). LICENSE.md § "L'exception
// sd-bus" says exactly what it costs and what it does not.
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
//
// ── ⚠️ Who may ask ──────────────────────────────────────────────────────────
//
// xdg-desktop-portal learns who is calling by opening /proc/<pid>/root of the
// caller (1.18, Ubuntu 24.04), and the kernel lets it only if its own
// capabilities cover the caller's: the package's worker holds CAP_SYS_NICE
// (moonlightweb-launch; CAP_SYS_ADMIN too, then), the portal holds none, so
// every call came back "Portal operation not allowed: Unable to open /proc/<pid>/root"
// (the .deb on the UM790Pro, 30/09/2026). A process that holds capabilities
// therefore asks through a helper of its own making — the same binary, exec'd
// with every capability dropped and none to regain — which holds the portal
// session for the stream's length and hands the PipeWire descriptor back over
// a socket. The capture keeps its capabilities (the queues at high priority);
// a process without any (the AppImage, a test run by hand) asks directly.

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

    /// Where the streamed monitor sits in the compositor's coordinate space —
    /// LOGICAL pixels, the same space a Wayland compositor spreads an absolute
    /// pointer across — when the portal says (Mutter, KWin and Hyprland's all
    /// do, for a monitor stream). hasPosition false when it did not, which
    /// leaves the picture at the origin: right on one screen, wrong past it.
    bool hasPosition = false;
    int x = 0;
    int y = 0;

    /// The node is on the session's own PipeWire, reached without a
    /// descriptor: KWin's virtual output (setKwinVirtualOutput) streams there.
    bool sessionPipeWire = false;

    bool valid() const { return nodeId != 0 && (pipewireFd >= 0 || sessionPipeWire); }
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

    /// The portal's AvailableSourceTypes bitmask (1 monitor, 2 window, 4
    /// virtual), 0 when no portal answers. GNOME 42 says 3, GNOME 48 says 7.
    static uint32_t sourceTypes();
    static constexpr uint32_t kSourceMonitor = 1;
    static constexpr uint32_t kSourceVirtual = 4;

    /// Ask for a VIRTUAL source rather than a monitor: the compositor creates
    /// a monitor for this session alone, sized by the PipeWire format the
    /// consumer negotiates, and removes it when the session closes. Before
    /// start().
    void setVirtual(bool virtualMonitor);

    /// Ask KWin for the virtual monitor instead of the portal — KDE Plasma 6,
    /// whose portal offers no VIRTUAL source (KwinVirtualOutput.h): an output
    /// named after @p name, of @p width × @p height. No dialog and no token;
    /// the same helper carries the request where this process holds a
    /// capability, since KWin cannot read such a process either. Before start().
    void setKwinVirtualOutput(const std::string& name, int width, int height);

    /// Ask Mutter itself instead of the portal — GNOME's own screen cast
    /// (MutterScreenCast.h): a virtual monitor of @p width × @p height at
    /// @p refreshHz or, with @p connector, that monitor as it is. No dialog,
    /// no token, and never through the helper: Mutter looks at no caller.
    /// The node is on the session's own PipeWire. Before start().
    void setMutter(const std::string& connector, int width, int height, int refreshHz);

    /// The pointer painted into the pictures rather than sent beside them:
    /// cursor_mode 2 instead of 4 on the portal, 1 instead of 2 on Mutter's
    /// route. A GNOME virtual monitor before GNOME 48, whose pictures carry it
    /// anyway (MutterScreenCast::embedsPointer). Before start().
    void setEmbedCursor(bool embed);

    /// Whether the compositor has ended the stream of its own accord since
    /// start(): the monitor it recorded went, the desktop was locked. Mutter's
    /// route says so; the others never do, and this is false there.
    bool ended();

    /// After a start() on Mutter's route that failed: Mutter refused the
    /// route outright (MutterScreenCast::refused), where the portal may serve.
    bool routeRefused() const;

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
    /// The handshake in this process, or through the helper (see above).
    bool startHere(const std::string& restoreToken, int timeoutMs, PortalStream& out,
                   std::string& error);
    bool startInHelper(const std::string& restoreToken, int timeoutMs, PortalStream& out,
                       std::string& error);
    void stopHelper();

    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace mw::native::capture
