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

#include "HostAppLauncher.h"

#include "common/Logger.h"

#include <QElapsedTimer>
#include <QHash>

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>

#include <chrono>
#include <mutex>
#include <optional>
#include <thread>

/*
 * Steam already running opens Big Picture on a screen of its own choosing, not
 * on the primary one the stream shows, and a Wayland compositor offers no way
 * to move another program's window. Steam's windows are X11 ones under
 * Xwayland, though, and there a pager's requests to the window manager are
 * still honoured. A native Wayland window is not listed here and stays where
 * it opens.
 */

namespace HostAppLauncher {

namespace {

// As on macOS: Steam already running shows Big Picture within a second or two,
// and a cold start lands on the primary display by itself.
constexpr int kWatchMs = 15000;
constexpr int kTickMs = 250;
constexpr int kMaxTries = 8;

/// Xlib's default handler ends the process, and any window may close between
/// two requests here. Never restored: two watches can overlap, and nothing else
/// in this process wants an X error to end it either.
int ignoreXError(Display*, XErrorEvent*)
{
    return 0;
}

struct Atoms
{
    explicit Atoms(Display* display)
        : clientList(XInternAtom(display, "_NET_CLIENT_LIST", False))
        , windowType(XInternAtom(display, "_NET_WM_WINDOW_TYPE", False))
        , typeNormal(XInternAtom(display, "_NET_WM_WINDOW_TYPE_NORMAL", False))
        , typeDialog(XInternAtom(display, "_NET_WM_WINDOW_TYPE_DIALOG", False))
        , state(XInternAtom(display, "_NET_WM_STATE", False))
        , stateFullScreen(XInternAtom(display, "_NET_WM_STATE_FULLSCREEN", False))
        , moveResize(XInternAtom(display, "_NET_MOVERESIZE_WINDOW", False))
    {}
    Atom clientList, windowType, typeNormal, typeDialog, state, stateFullScreen, moveResize;
};

QList<unsigned long> valuesOf(Display* display, Window window, Atom property, Atom type)
{
    Atom actualType = 0;
    int actualFormat = 0;
    unsigned long count = 0, remaining = 0;
    unsigned char* data = nullptr;
    QList<unsigned long> out;
    if (XGetWindowProperty(display, window, property, 0, 4096, False, type, &actualType,
                           &actualFormat, &count, &remaining, &data) == Success &&
        data && actualFormat == 32) {
        // Format 32 comes back as an array of long, whatever long's size.
        const auto* values = reinterpret_cast<const unsigned long*>(data);
        for (unsigned long i = 0; i < count; ++i)
            out.append(values[i]);
    }
    if (data) XFree(data);
    return out;
}

/// The windows the window manager handles that a person would call a window,
/// shown now, in root coordinates.
QList<SeenWindow> windowsOn(Display* display, const Atoms& atoms)
{
    QList<SeenWindow> out;
    const Window root = DefaultRootWindow(display);
    for (unsigned long id : valuesOf(display, root, atoms.clientList, XA_WINDOW)) {
        const auto window = static_cast<Window>(id);
        XWindowAttributes attributes{};
        if (!XGetWindowAttributes(display, window, &attributes) ||
            attributes.map_state != IsViewable)
            continue;
        const QList<unsigned long> types = valuesOf(display, window, atoms.windowType, XA_ATOM);
        if (!types.isEmpty() && !types.contains(atoms.typeNormal) &&
            !types.contains(atoms.typeDialog))
            continue;
        int x = 0, y = 0;
        Window child = 0;
        if (!XTranslateCoordinates(display, window, root, 0, 0, &x, &y, &child)) continue;
        out.append({static_cast<int>(id), QRect(x, y, attributes.width, attributes.height)});
    }
    return out;
}

/// The primary monitor: the virtual display while it shows a stream.
std::optional<QRect> primaryMonitor(Display* display)
{
    std::optional<QRect> out;
    int count = 0;
    XRRMonitorInfo* monitors = XRRGetMonitors(display, DefaultRootWindow(display), True, &count);
    if (!monitors) return out;
    for (int i = 0; i < count; ++i)
        if (monitors[i].primary)
            out = QRect(monitors[i].x, monitors[i].y, monitors[i].width, monitors[i].height);
    XRRFreeMonitors(monitors);
    return out;
}

/// A request to the window manager, as a pager sends it (EWMH).
void ask(Display* display, Window window, Atom message, const std::array<long, 5>& values)
{
    XEvent event{};
    event.xclient.type = ClientMessage;
    event.xclient.window = window;
    event.xclient.message_type = message;
    event.xclient.format = 32;
    for (size_t i = 0; i < values.size(); ++i)
        event.xclient.data.l[i] = values[i];
    XSendEvent(display, DefaultRootWindow(display), False,
               SubstructureRedirectMask | SubstructureNotifyMask, &event);
}

void setFullScreen(Display* display, const Atoms& atoms, Window window, bool on)
{
    const long fromPager = 2;
    ask(display, window, atoms.state,
        {on ? 1L : 0L, static_cast<long>(atoms.stateFullScreen), 0L, fromPager, 0L});
}

QString nameOf(Display* display, Window window)
{
    XClassHint hint{};
    QString out = QStringLiteral("0x%1").arg(static_cast<qulonglong>(window), 0, 16);
    if (XGetClassHint(display, window, &hint)) {
        if (hint.res_class) out += QStringLiteral(" (%1)").arg(QString::fromUtf8(hint.res_class));
        if (hint.res_name) XFree(hint.res_name);
        if (hint.res_class) XFree(hint.res_class);
    }
    return out;
}

void pullOnce(Display* display, const Atoms& atoms, const QRect& target, const QSet<int>& before,
              QSet<int>* moved, QHash<int, WindowMove>* moving)
{
    const QList<SeenWindow> now = windowsOn(display, atoms);
    QList<SeenWindow> todo = windowsToMove(before, *moved, now, target);
    // Those already there that still have a step left: full screen again.
    for (const SeenWindow& w : now)
        if (moving->contains(w.number) && !moved->contains(w.number) &&
            target.contains(w.bounds.center()))
            todo.append(w);

    for (const SeenWindow& w : todo) {
        const auto window = static_cast<Window>(static_cast<unsigned int>(w.number));
        WindowMove& move = (*moving)[w.number];
        const bool fullScreen = valuesOf(display, window, atoms.state, XA_ATOM)
                                    .contains(static_cast<unsigned long>(atoms.stateFullScreen));
        const WindowStep step =
            nextWindowStep(move, fullScreen, target.contains(w.bounds.center()), kMaxTries);
        switch (step) {
        case WindowStep::LeaveFullScreen:
            move.wasFullScreen = true;
            ++move.tries;
            setFullScreen(display, atoms, window, false);
            break;
        case WindowStep::Move:
            ++move.tries;
            ask(display, window, atoms.moveResize,
                moveResizeValues(placedOn(w.bounds.size(), target)));
            break;
        case WindowStep::EnterFullScreen:
            setFullScreen(display, atoms, window, true);
            [[fallthrough]];
        case WindowStep::Done:
            moved->insert(w.number);
            Logger::info(
                QStringLiteral("[HostApp] moved window %1 onto the virtual display%2")
                    .arg(nameOf(display, window), step == WindowStep::EnterFullScreen
                                                      ? QStringLiteral(", in full screen again")
                                                      : QString()));
            break;
        case WindowStep::GiveUp:
            moved->insert(w.number);
            Logger::warning(QStringLiteral("[HostApp] could not move window %1 onto the virtual "
                                           "display")
                                .arg(nameOf(display, window)));
            break;
        }
    }
    XFlush(display);
}

/// Its own thread: a program holding the X server must not hold the event loop.
void watch(Display* display, const QSet<int>& before)
{
    const Atoms atoms(display);
    const std::optional<QRect> target = primaryMonitor(display);
    if (target) {
        QSet<int> moved;
        QHash<int, WindowMove> moving;
        QElapsedTimer age;
        age.start();
        // Another primary monitor means the stream ended, and its display with it.
        while (age.elapsed() < kWatchMs && primaryMonitor(display) == target) {
            pullOnce(display, atoms, *target, before, &moved, &moving);
            std::this_thread::sleep_for(std::chrono::milliseconds(kTickMs));
        }
    } else {
        Logger::warning(QStringLiteral("[HostApp] no primary monitor: the app stays where it "
                                       "opens, which may not be the virtual display"));
    }
    XCloseDisplay(display);
}

} // namespace

bool openOnVirtualDisplay(const QString& command, QString* error)
{
    Display* display = nullptr;
    QSet<int> before;
    if (!qEnvironmentVariableIsEmpty("DISPLAY")) {
        static std::once_flag once;
        std::call_once(once, [] { XSetErrorHandler(ignoreXError); });
        display = XOpenDisplay(nullptr);
        if (display) {
            for (const SeenWindow& w : windowsOn(display, Atoms(display)))
                before.insert(w.number);
        } else {
            Logger::warning(QStringLiteral("[HostApp] the X server refused the connection "
                                           "(XAUTHORITY?): the app stays where it opens, which "
                                           "may not be the virtual display"));
        }
    }
    if (!open(command, error)) {
        if (display) XCloseDisplay(display);
        return false;
    }
    if (display) std::thread(watch, display, before).detach();
    return true;
}

} // namespace HostAppLauncher
