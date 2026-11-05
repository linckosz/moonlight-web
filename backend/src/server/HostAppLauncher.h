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

#include <QList>
#include <QRect>
#include <QRegularExpression>
#include <QSet>
#include <QString>
#include <QStringList>

#include <array>

/**
 * @brief Opens one of the native host's apps (AppSettings::hostApps) on this
 * PC's desktop, as the user who sits at it — never as anything the server or
 * its worker may have been raised to.
 *
 * The server opens it once the stream is up (main.cpp), so the program lands
 * on the screen that stream shows: "MoonlightWeb Virtual Display", made the
 * primary one, or the primary display itself. Linux apps that run in their
 * own gamescope are not opened here: the session starts them inside it.
 *
 * The program is left running when the stream ends, as gamescope's linger
 * leaves it — a reconnect finds the game where it was. Quitting the card
 * runs the app's quit command, when it has one (Steam: close Big Picture).
 *
 *  - Windows: through the desktop's own shell (Explorer's ShellExecute, asked
 *    over COM), so the program is Explorer's child — the user's ordinary
 *    token even when this server runs elevated, out of our process tree, and
 *    any URL, .exe, .lnk or App Paths name works as it does in the Run box.
 *    From the session-0 service of older installs, this executable is
 *    started on the console desktop as its user (--open-on-desktop) and does
 *    the same from there.
 *  - macOS: /usr/bin/open for a URL or an .app, /bin/sh -c otherwise. An app
 *    already running opens its new window on the screen of its old ones —
 *    Steam puts Big Picture next to its own window, not on the main display
 *    — so for the virtual display the windows it opens next are moved there
 *    (openOnVirtualDisplay).
 *  - Linux: xdg-open for a URL, /bin/sh -c otherwise, each in a transient
 *    systemd user unit when there is one, out of the server's own cgroup.
 *    As on macOS, Steam already running puts Big Picture on a screen of its
 *    choosing, so the X11 windows opened next are moved to the virtual display
 *    (openOnVirtualDisplay, HostAppLauncherX11.cpp).
 */
namespace HostAppLauncher {

/// Whether @p command is a URL ("steam://open/bigpicture") rather than a
/// program: a scheme of two characters or more, so "C:\Games\x.exe" is not,
/// and nothing after it.
inline bool isUrl(const QString& command)
{
    // RFC 3986's scheme, two characters at least so a drive letter is not one.
    static const QRegularExpression scheme(QStringLiteral("^[A-Za-z][A-Za-z0-9+.-]+:"));
    const QString line = command.trimmed();
    return scheme.match(line).hasMatch() && !line.contains(QLatin1Char(' '));
}

/// Splits a Windows command line into its program and the arguments that
/// follow, the way the Run box reads it: a quoted program, or the first word.
inline void splitWindowsCommand(const QString& command, QString* program, QString* arguments)
{
    const QString line = command.trimmed();
    if (line.startsWith(QLatin1Char('"'))) {
        const int close = line.indexOf(QLatin1Char('"'), 1);
        *program = close > 0 ? line.mid(1, close - 1) : line.mid(1);
        *arguments = close > 0 ? line.mid(close + 1).trimmed() : QString();
        return;
    }
    const int space = line.indexOf(QLatin1Char(' '));
    *program = space < 0 ? line : line.left(space);
    *arguments = space < 0 ? QString() : line.mid(space + 1).trimmed();
}

/// systemd-run's arguments for @p run in the transient user unit @p unit, with
/// @p environment ("NAME=value"). xdg-open returns as soon as the link's
/// handler is started, and that ends the unit: KillMode=process leaves the
/// handler running, where the default would kill it along with the unit.
inline QStringList systemdRunArguments(const QString& unit, const QStringList& environment,
                                       const QStringList& run)
{
    QStringList arguments{QStringLiteral("--user"), QStringLiteral("--collect"),
                          QStringLiteral("--quiet"), QStringLiteral("--unit=%1").arg(unit),
                          QStringLiteral("--property=KillMode=process")};
    for (const QString& variable : environment)
        arguments << QStringLiteral("--setenv=%1").arg(variable);
    arguments << QStringLiteral("--") << run;
    return arguments;
}

/// Opens @p command on the desktop, without waiting for it. False with
/// @p error when it could not be handed over.
bool open(const QString& command, QString* error);

/// open() for an app shown on "MoonlightWeb Virtual Display", which is the
/// main display by then. macOS: the windows the app opens in the next few
/// seconds are moved onto it, those it had before left where they are; one
/// in full screen (Big Picture) leaves it, moves, and goes back to it there.
/// Linux, the same for X11 windows, Xwayland's included: which program opened
/// a window cannot be told there, so it is every new one in those seconds.
/// Windows: the same as open(), a new window lands on the primary display.
bool openOnVirtualDisplay(const QString& command, QString* error);

/// One window of the app being opened, as the window server lists it.
struct SeenWindow
{
    int number = 0; ///< the window server's id, stable for the window's life
    QRect bounds;   ///< global coordinates, top-left origin
};

/// Which of @p now to move onto @p target: windows the app opened since
/// @p before was taken, not moved already, not on @p target yet, and big
/// enough to be a window rather than a tooltip or a menu's shadow.
inline QList<SeenWindow> windowsToMove(const QSet<int>& before, const QSet<int>& moved,
                                       const QList<SeenWindow>& now, const QRect& target)
{
    QList<SeenWindow> out;
    for (const SeenWindow& w : now) {
        if (before.contains(w.number) || moved.contains(w.number)) continue;
        if (w.bounds.width() < 100 || w.bounds.height() < 100) continue;
        if (target.contains(w.bounds.center())) continue;
        out.append(w);
    }
    return out;
}

/// Where a window of @p size goes on @p target: its top-left corner, no larger
/// than the display.
inline QRect placedOn(const QSize& size, const QRect& target)
{
    return QRect(target.topLeft(), size.boundedTo(target.size()));
}

/// The values of EWMH's _NET_MOVERESIZE_WINDOW that put a window at @p to, asked
/// as a pager asks.
inline std::array<long, 5> moveResizeValues(const QRect& to)
{
    const long northWestGravity = 1;
    const long positionAndSize = 0xF << 8;
    const long fromPager = 2 << 12;
    return {northWestGravity | positionAndSize | fromPager, to.x(), to.y(), to.width(),
            to.height()};
}

/// What moving one X11 window has done so far.
struct WindowMove
{
    bool wasFullScreen = false;
    int tries = 0;
};

enum class WindowStep
{
    LeaveFullScreen, ///< a window manager does not move one in full screen
    Move,
    EnterFullScreen, ///< on the target now, as it was before
    Done,
    GiveUp,
};

/// The next step for a window. A request may be lost while the window manager
/// is busy with the one before, so each is repeated until the window shows it,
/// @p maxTries times at most.
inline WindowStep nextWindowStep(const WindowMove& move, bool fullScreen, bool onTarget,
                                 int maxTries)
{
    if (onTarget)
        return move.wasFullScreen && !fullScreen ? WindowStep::EnterFullScreen : WindowStep::Done;
    if (move.tries >= maxTries) return WindowStep::GiveUp;
    return fullScreen ? WindowStep::LeaveFullScreen : WindowStep::Move;
}

#ifdef Q_OS_WIN
/// --open-on-desktop <base64url>: the console-session half of open() for the
/// service of older installs. Returns the process exit code.
int runOpenOnDesktop(const QString& encoded);
#endif

} // namespace HostAppLauncher
