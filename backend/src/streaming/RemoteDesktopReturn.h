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

#include <QObject>
#include <QTimer>

#include <atomic>

/**
 * The server's watch over its own Windows session, for the one case where
 * Remote Desktop leaves it with no screen (RemoteDesktopReturnPolicy.h).
 *
 * Every two seconds it reads the session's state — no window, no process, one
 * WTS call. When the session was last seen active through Remote Desktop and
 * is now Disconnected:
 *  - the native host's card says so (leftByRemoteDesktop()), instead of a
 *    "No screen" that sends the owner looking for a cable;
 *  - with the setting on (AppSettings::remoteDesktopReturn, on by default),
 *    the native host enabled and the launcher service installed, the launcher
 *    puts the session back on the console and this process locks it at once:
 *    the PC ends as if its user had signed in there and pressed Win+L, which
 *    streams, and is unlocked from the stream with the user's own PIN.
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

    void onTick();
    void askReturn();
    void returnFinished(Outcome outcome, const QString& detail);

    RemoteDesktopReturnPolicy::Tracker m_Tracker;
    QTimer m_Timer;
    bool m_Started = false;
    bool m_LeftByRemoteDesktop = false;
    int m_LastState = -1;
    std::atomic<bool> m_Asking{false};
};
