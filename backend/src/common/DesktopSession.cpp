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

#include "DesktopSession.h"

#include <QtGlobal>

#include <atomic>

#if defined(Q_OS_LINUX)
#include <QByteArray>

#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace mw {
namespace {

// -1 not probed yet, 0 no display, 1 display. Cached because the QPA platform
// plugin is chosen once, at QApplication construction: every later caller must
// get the same answer the plugin choice was made on. Atomic because the API
// exposes it (SystemRoutes) from the HTTP threads.
std::atomic<int> g_displayServer{-1};

#if defined(Q_OS_LINUX)
// Connect to a Unix socket and drop it. @p abstract selects the Linux abstract
// namespace (leading NUL), where X servers also listen and where no filesystem
// entry exists to stat.
bool unixSocketAnswers(const QByteArray& path, bool abstract = false)
{
    if (path.isEmpty()) return false;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    // One byte is the NUL terminator (or, in the abstract namespace, the leading
    // NUL); a path that does not fit cannot be the socket we are looking for.
    const size_t room = sizeof(addr.sun_path) - 1;
    if (static_cast<size_t>(path.size()) > room) return false;
    memcpy(addr.sun_path + (abstract ? 1 : 0), path.constData(), path.size());
    const socklen_t len =
        abstract ? static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + 1 + path.size())
                 : static_cast<socklen_t>(sizeof(addr));

    const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return false;
    const bool ok = ::connect(fd, reinterpret_cast<sockaddr*>(&addr), len) == 0;
    ::close(fd);
    return ok;
}

// DISPLAY is "[protocol/][host]:number[.screen]". A local display (empty host,
// or the literal "unix") lives on /tmp/.X11-unix/X<number>; anything else is a
// TCP display (ssh -X forwarding, a remote X server) which we do not probe —
// that would cost a DNS lookup and a possibly long connect on the startup path.
// Those are taken at their word; the QT_QPA_PLATFORM fallback list in main.cpp
// is what keeps a wrong guess from aborting the process.
bool x11DisplayAnswers()
{
    const QByteArray display = qgetenv("DISPLAY");
    if (display.isEmpty()) return false;

    const int colon = display.lastIndexOf(':');
    if (colon < 0) return false; // malformed — no display number at all
    QByteArray host = display.left(colon);
    if (const int slash = host.indexOf('/'); slash >= 0) host = host.mid(slash + 1);
    if (!host.isEmpty() && host != "unix") return true; // remote/TCP: assume reachable

    QByteArray number = display.mid(colon + 1);
    if (const int dot = number.indexOf('.'); dot >= 0) number = number.left(dot);
    if (number.isEmpty()) return false;

    const QByteArray path = "/tmp/.X11-unix/X" + number;
    // Xwayland can be started with only the abstract socket bound, so a missing
    // file is not proof of absence — try both namespaces before giving up.
    return unixSocketAnswers(path) || unixSocketAnswers(path, /*abstract=*/true);
}

// WAYLAND_DISPLAY is a socket name inside XDG_RUNTIME_DIR, or an absolute path.
// Without XDG_RUNTIME_DIR a relative name cannot be resolved — libwayland would
// fail the same way, so that counts as no display.
bool waylandDisplayAnswers()
{
    const QByteArray display = qgetenv("WAYLAND_DISPLAY");
    if (display.isEmpty()) return false;
    if (display.startsWith('/')) return unixSocketAnswers(display);

    const QByteArray runtimeDir = qgetenv("XDG_RUNTIME_DIR");
    if (runtimeDir.isEmpty()) return false;
    return unixSocketAnswers(runtimeDir + '/' + display);
}
#endif // Q_OS_LINUX

bool probeDisplayServer()
{
#if defined(Q_OS_LINUX)
    return waylandDisplayAnswers() || x11DisplayAnswers();
#else
    return true;
#endif
}

} // namespace

bool hasDisplayServer()
{
    int cached = g_displayServer.load(std::memory_order_relaxed);
    if (cached < 0) {
        cached = probeDisplayServer() ? 1 : 0;
        // A racing probe reaches the same verdict, so whoever stores last wins
        // and both callers agree.
        g_displayServer.store(cached, std::memory_order_relaxed);
    }
    return cached == 1;
}

void confirmDisplayServer(bool present)
{
    g_displayServer.store(present ? 1 : 0, std::memory_order_relaxed);
}

bool hasDesktopSession()
{
    if (!qEnvironmentVariableIsEmpty("MW_SERVICE")) return false;
    return hasDisplayServer();
}

} // namespace mw
