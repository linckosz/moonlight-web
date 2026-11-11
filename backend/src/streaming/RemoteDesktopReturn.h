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

#include "RemoteDesktopReturnPolicy.h"

#include <QList>
#include <QObject>
#include <QTimer>

#include <atomic>
#include <functional>

/**
 * The server's watch over its own Windows session, for the one case where
 * Remote Desktop leaves it with no screen (RemoteDesktopReturnPolicy.h).
 *
 * Every two seconds it reads the session's state — no window, no process, one
 * WTS call. When the session was last seen active through Remote Desktop and
 * is now Disconnected, the native host's card says so (leftByRemoteDesktop()),
 * instead of a "No screen" that sends the owner looking for a cable.
 *
 * With the setting on (AppSettings::remoteDesktopReturn, on by default), the
 * native host enabled and the launcher service installed, the card also
 * offers "This PC's screen" (offersReturn()). Opening it (bringBack(), from
 * the stream's start) has the launcher put the session back on the console,
 * and this process locks it at once: the PC ends as if its user had signed in
 * there and pressed Win+L, which streams, and is unlocked from the stream with
 * the user's own PIN. Nothing moves on its own when mstsc is closed. Not
 * offered in this version (RemoteDesktopReturnPolicy::kReturnOffered): the
 * card says what happened, and that is all.
 *
 * Only where the server runs on the desktop itself (the logon task, a dev
 * instance). Inert everywhere else, other platforms included.
 */
class RemoteDesktopReturn : public QObject
{
    Q_OBJECT

public:
    static RemoteDesktopReturn& instance();

    /// Start watching (idempotent). Called once the server is up.
    void start();

    /// The session was left by Remote Desktop and is still away from the
    /// screen.
    bool leftByRemoteDesktop() const { return m_LeftByRemoteDesktop; }

    /// leftByRemoteDesktop(), and opening the card may bring the session back:
    /// the setting is on, the native host enabled, the launcher installed.
    bool offersReturn() const;

    /// leftByRemoteDesktop(), and the card offers "This PC's sign-in screen"
    /// instead: this version offers it, the native host is enabled and the
    /// launcher installed (RemoteDesktopReturnPolicy::signInOffered).
    bool offersSignIn() const;

    /// Why a stream of the sign-in screen must NOT start now, in English;
    /// empty when it may. @p booth: the request comes from a kiosk, which
    /// never gets it (RemoteDesktopReturnPolicy::signInRequestRefusal).
    QString signInRefusal(bool booth) const;

    /// Read the session's state now rather than at the next tick: a /start of
    /// the sign-in screen's card right after the user signed in through it
    /// streams the PC's screen, not the sign-in screen again. Main thread.
    void refreshNow() { onTick(); }

    /// This session is on the PC's screen at this moment (Active, and the
    /// console's): read now, not from the last tick. False on other platforms.
    static bool backOnScreenNow();

    /// How a stream of the sign-in screen ended, read from what Windows says
    /// now, in English for the audit line: "signed in" when this session is
    /// back on the screen; "stopped" when another account is signed in on
    /// the console; "closed" when the sign-in screen is still up.
    static QString signInOutcomeNow();

    /// Put the session back on this PC's screen, locked, then call @p done on
    /// this thread: true once the lock is confirmed, else false with the
    /// reason in English. A request made while one is under way waits for
    /// that one's result. Main thread only.
    void bringBack(std::function<void(bool ok, const QString& error)> done);

signals:
    /// leftByRemoteDesktop() changed, or the session is back on a screen:
    /// what the native host can capture has changed with it.
    void changed();

private:
    RemoteDesktopReturn() = default;

    /// How a return ended.
    enum class Outcome
    {
        Back,    ///< on the PC's screen, and the lock confirmed
        Refused, ///< the launcher said no (somebody is signed in on the PC)
        Hidden,  ///< moved, but not locked in time: disconnected again
        Exposed, ///< moved, not locked, and could not be disconnected again
    };

    /// The setting is on, the native host enabled, the launcher installed.
    bool allowed() const;
    /// The native host enabled and the launcher installed: what the sign-in
    /// screen needs. The return's setting is not looked at: it moves nothing.
    bool signInAllowed() const;

    void onTick();
    void askReturn();
    void returnFinished(Outcome outcome, const QString& detail);

    RemoteDesktopReturnPolicy::Tracker m_Tracker;
    QTimer m_Timer;
    bool m_Started = false;
    bool m_Watching = false;
    bool m_LeftByRemoteDesktop = false;
    int m_LastState = -1;
    std::atomic<bool> m_Asking{false};
    /// The requests waiting for the return under way (bringBack).
    QList<std::function<void(bool, const QString&)>> m_Waiting;
};
