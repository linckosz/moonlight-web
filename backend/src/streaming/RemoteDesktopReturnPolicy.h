/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include <QString>
#include <QtGlobal>

/**
 * The decisions behind "a session left by Remote Desktop goes back to this
 * PC's screen, locked" — without a single Windows call, so that every rule can
 * be pinned by a test on any platform. RemoteDesktopReturn (the server) and
 * WorkerService (the SYSTEM launcher) feed them what Windows says.
 *
 * ── The case ────────────────────────────────────────────────────────────────
 *
 * On client Windows, a Remote Desktop logon takes the user's session off the
 * console. Closing mstsc leaves that session Disconnected: it keeps running,
 * our server in it, but no display at all is attached to it — real or
 * virtual — and the console shows the sign-in screen of another, empty
 * session. The native host then has nothing to capture ("No screen"), and
 * the launcher refuses a worker to a caller that is not on the console.
 * Windows reconnects the session to the console when the user signs in on
 * the PC; `tscon <id> /dest:console` from SYSTEM does the same without a
 * password. The launcher does that, and the server locks the session at once,
 * so the PC ends exactly as "signed in on the PC, then Win+L" — which streams.
 */
namespace RemoteDesktopReturnPolicy {

/// WTS_CONNECTSTATE_CLASS, by value (wtsapi32.h), so this header needs no
/// Windows include.
enum SessionState : int
{
    Active = 0,
    Connected = 1,
    ConnectQuery = 2,
    Shadow = 3,
    Disconnected = 4,
    Idle = 5,
    Listen = 6,
    Reset = 7,
    Down = 8,
    Init = 9,
};

/// "No session" for a session id, as WTSGetActiveConsoleSessionId says it.
constexpr quint32 kNoSession = 0xFFFFFFFFu;

/// What the launcher knows when a caller asks to be put back on the console.
struct ConsoleReturnFacts
{
    /// The caller is this same executable (its image was checked).
    bool callerIsThisExecutable = false;
    /// The session the caller's token belongs to.
    quint32 callerSession = kNoSession;
    /// That session's state (SessionState).
    int callerState = -1;
    /// The console session now (WTSGetActiveConsoleSessionId).
    quint32 consoleSession = kNoSession;
    /// Somebody is signed in on the console session — locked or not.
    bool consoleUserPresent = true;
};

/// Why the launcher must NOT connect the caller's session to the console, in
/// English for its log; empty when it may. The only session it ever moves is
/// the caller's own, only when Windows has left it Disconnected, and never
/// over somebody who is signed in on the PC: a second account at the console
/// keeps its screen.
QString consoleReturnRefusal(const ConsoleReturnFacts& facts);

/// Why the launcher must NOT disconnect the caller's session again ("hide!",
/// the way back when a return could not lock the session); empty when it may.
/// Only the caller's own session, only while it is Active on the console —
/// the state a failed return leaves it in. consoleUserPresent is not looked
/// at: the user signed in there is the caller.
QString hideRefusal(const ConsoleReturnFacts& facts);

/// The card a session left by Remote Desktop offers, and opening it is what
/// brings the session back. A fixed id beside the virtual display's (1000),
/// out of the displays' range and below the owner's apps' (1100).
constexpr int kReturnAppId = 1001;

/// What the server sees of its own session, every few seconds.
struct Observation
{
    /// SessionState of the session the server runs in.
    int state = -1;
    /// The session is a Remote Desktop one at this moment (WTSClientProtocolType
    /// is RDP). Read while Active; a disconnected session's answer is not used.
    bool remote = false;
};

/**
 * The server's side, as a small state machine fed one observation per tick.
 *
 * Only a session that was last seen ACTIVE THROUGH REMOTE DESKTOP counts as
 * "left by Remote Desktop". A session the user switched away from on the PC
 * itself (Switch user, so another account can sign in) is Disconnected too,
 * and pulling it back would throw the other person off the sign-in screen:
 * that one was last seen active on the console, and is left alone.
 *
 * Nothing is ever asked from here: the return starts when somebody opens the
 * native host's card to stream it (decision of 11/11/2026), never because
 * mstsc was closed. The PC's screen shows the session only when a stream is
 * about to use it. The tracker says whether that request may go out now
 * (returnRefusal).
 */
class Tracker
{
public:
    /// Remote Desktop moving a session from one client to another passes
    /// through Disconnected for an instant: a return is only asked once the
    /// session has been seen Disconnected on this many ticks in a row.
    static constexpr int kSettleTicks = 2;

    /// Whether the session was left by Remote Desktop and is still away from
    /// the screen: the card says so rather than "No screen", and offers the
    /// way back.
    bool observe(const Observation& now);

    /// Why a return must NOT be asked now, in English; empty when it may.
    /// @p allowed: the setting is on, the native host is enabled, and the
    /// launcher is there to ask.
    QString returnRefusal(bool allowed) const;

    /// The session reached the screen but could not be locked, and was sent
    /// away again: no return until the session is seen Active (used through
    /// Remote Desktop again, or signed in on the PC). Each try of a return
    /// that cannot lock would show the desktop again for a few seconds.
    void giveUp() { m_GaveUp = true; }

private:
    bool m_GaveUp = false;
    bool m_LastActiveRemote = false;
    bool m_Left = false;
    int m_DisconnectedTicks = 0;
};

} // namespace RemoteDesktopReturnPolicy
