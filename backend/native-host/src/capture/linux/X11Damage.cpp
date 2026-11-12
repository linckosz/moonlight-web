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

#include "X11Damage.h"

#include "../../core/Log.h"
#include "../../platform/linux/XlibLock.h"

#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>
#include <poll.h>

namespace mw::native::capture {
namespace {

// The sonames, not the -dev symlinks (X11Pointer.cpp says why).
constexpr const char* kX11 = "libX11.so.6";
constexpr const char* kXdamage = "libXdamage.so.1";

/// XDamageReportRawRectangles: an event per rendering operation, each with its
/// rectangle — nothing to subtract, and the area to test against the display.
constexpr int kReportRawRectangles = 0;

/// Xlib's XRectangle and XDamageNotifyEvent (X11/extensions/Xdamage.h), spelled
/// out so no X header is needed: the same members in the same order, laid out
/// by the same ABI rules. `area` is what was drawn; `geometry` is the whole
/// drawable's — the root — and must not be read for it.
struct Rectangle
{
    short x;
    short y;
    unsigned short width;
    unsigned short height;
};

struct DamageNotifyEvent
{
    int type;
    unsigned long serial;
    int sendEvent;
    void* display;
    unsigned long drawable;
    unsigned long damage;
    int level;
    int more;
    unsigned long timestamp;
    Rectangle area;
    Rectangle geometry;
};

/// An XEvent is a union of 24 longs.
constexpr size_t kEventLongs = 24;
static_assert(sizeof(DamageNotifyEvent) <= kEventLongs * sizeof(long), "fits an XEvent");

/// What Xlib calls when the connection dies: by default exit() — a whole
/// stream worker gone because the X server went away under a watcher. With
/// this handler returning, Xlib leaves the display dead instead, and the flag
/// stops the thread before it calls into it again.
void onConnectionLost(void* /*display*/, void* userData)
{
    if (auto* dead = static_cast<std::atomic<bool>*>(userData)) dead->store(true);
}

} // namespace

X11Damage::~X11Damage()
{
    stop();
}

bool X11Damage::watch(int left, int top, int right, int bottom, std::string& why)
{
    m_Left.store(left);
    m_Top.store(top);
    m_Right.store(right);
    m_Bottom.store(bottom);
    if (m_Thread.joinable()) return true;

    // X11Pointer's rules: no DISPLAY, no X server; a Wayland session's DISPLAY
    // is Xwayland's, which does not see the compositor's picture.
    const char* display = std::getenv("DISPLAY");
    if (!display || *display == '\0') {
        why = "no DISPLAY";
        return false;
    }
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    const char* sessionType = std::getenv("XDG_SESSION_TYPE");
    if ((wayland && *wayland != '\0') || (sessionType && std::string(sessionType) == "wayland")) {
        why = "a Wayland session (its DISPLAY is Xwayland's)";
        return false;
    }

    m_X11 = ::dlopen(kX11, RTLD_NOW | RTLD_LOCAL);
    m_Xdamage = m_X11 ? ::dlopen(kXdamage, RTLD_NOW | RTLD_LOCAL) : nullptr;
    if (!m_X11 || !m_Xdamage) {
        why = std::string(m_X11 ? kXdamage : kX11) + " not available";
        unload();
        return false;
    }
    const auto x11 = [this](const char* name) { return ::dlsym(m_X11, name); };
    m_XOpenDisplay = reinterpret_cast<decltype(m_XOpenDisplay)>(x11("XOpenDisplay"));
    m_XCloseDisplay = reinterpret_cast<decltype(m_XCloseDisplay)>(x11("XCloseDisplay"));
    m_XDefaultRootWindow =
        reinterpret_cast<decltype(m_XDefaultRootWindow)>(x11("XDefaultRootWindow"));
    m_XConnectionNumber = reinterpret_cast<decltype(m_XConnectionNumber)>(x11("XConnectionNumber"));
    m_XPending = reinterpret_cast<decltype(m_XPending)>(x11("XPending"));
    m_XNextEvent = reinterpret_cast<decltype(m_XNextEvent)>(x11("XNextEvent"));
    m_XFlush = reinterpret_cast<decltype(m_XFlush)>(x11("XFlush"));
    // libX11 1.7 and later: without it a server that goes away takes the
    // process with it, so no watch at all rather than that.
    using ExitHandler = void (*)(void*, void*);
    auto setExitHandler =
        reinterpret_cast<void (*)(Display*, ExitHandler, void*)>(x11("XSetIOErrorExitHandler"));
    m_XDamageQueryExtension = reinterpret_cast<decltype(m_XDamageQueryExtension)>(
        ::dlsym(m_Xdamage, "XDamageQueryExtension"));
    m_XDamageCreate =
        reinterpret_cast<decltype(m_XDamageCreate)>(::dlsym(m_Xdamage, "XDamageCreate"));
    if (!m_XOpenDisplay || !m_XCloseDisplay || !m_XDefaultRootWindow || !m_XConnectionNumber ||
        !m_XPending || !m_XNextEvent || !m_XFlush || !m_XDamageQueryExtension || !m_XDamageCreate ||
        !setExitHandler) {
        why = std::string(kX11) + " or " + kXdamage +
              " lacks a call this needs (XSetIOErrorExitHandler: libX11 1.7)";
        unload();
        return false;
    }

    // Every Xlib call under the process's lock (XlibLock.h), here to the
    // flush; the early returns let it go.
    std::unique_lock<std::mutex> xlib(platform::xlibMutex());
    m_Display = m_XOpenDisplay(display);
    if (!m_Display) {
        why = std::string("the X server \"") + display + "\" refused the connection";
        unload();
        return false;
    }
    int errorBase = 0;
    if (!m_XDamageQueryExtension(m_Display, &m_EventBase, &errorBase)) {
        why = "the X server has no DAMAGE extension";
        m_XCloseDisplay(m_Display);
        m_Display = nullptr;
        unload();
        return false;
    }
    m_Stop.store(false);
    m_Dead.store(false);
    setExitHandler(m_Display, &onConnectionLost, &m_Dead);
    // Creating it reports what is already on screen: one extra read of the
    // picture at the start, which costs nothing worth a special case.
    m_XDamageCreate(m_Display, m_XDefaultRootWindow(m_Display), kReportRawRectangles);
    m_XFlush(m_Display);
    xlib.unlock();
    m_Thread = std::thread([this] { run(); });
    log::info(std::string("[native] KMS: watching X damage on \"") + display +
              "\" — a picture the X server draws into the scanned-out buffer, without a new "
              "buffer, is read again");
    return true;
}

void X11Damage::run()
{
    int fd = -1;
    {
        std::lock_guard<std::mutex> xlib(platform::xlibMutex());
        fd = m_XConnectionNumber(m_Display);
    }
    long event[kEventLongs];
    while (!m_Stop.load() && !m_Dead.load()) {
        pollfd p = {fd, POLLIN, 0};
        ::poll(&p, 1, 100);
        std::lock_guard<std::mutex> xlib(platform::xlibMutex());
        while (!m_Stop.load() && !m_Dead.load() && m_XPending(m_Display) > 0) {
            m_XNextEvent(m_Display, event);
            if (m_Dead.load()) break;
            int type = 0;
            std::memcpy(&type, event, sizeof(type));
            // DamageNotify is the extension's first event: its event base.
            if (type != m_EventBase) continue;
            DamageNotifyEvent notify;
            std::memcpy(&notify, event, sizeof(notify));
            const int x = notify.area.x;
            const int y = notify.area.y;
            const int right = x + static_cast<int>(notify.area.width);
            const int bottom = y + static_cast<int>(notify.area.height);
            if (x < m_Right.load() && right > m_Left.load() && y < m_Bottom.load() &&
                bottom > m_Top.load()) {
                m_Events.fetch_add(1, std::memory_order_relaxed);
                m_Changed.store(true, std::memory_order_release);
            }
        }
    }
    if (m_Dead.load()) log::info("[native] KMS: the X server went away — no more damage to watch");
}

void X11Damage::stop()
{
    m_Stop.store(true);
    if (m_Thread.joinable()) m_Thread.join();
    // A dead connection is left alone: Xlib says nothing more may be done with
    // it, closing included. The process's memory is all it costs.
    if (m_Display && !m_Dead.load() && m_XCloseDisplay) {
        std::lock_guard<std::mutex> xlib(platform::xlibMutex());
        m_XCloseDisplay(m_Display);
    }
    m_Display = nullptr;
    unload();
}

void X11Damage::unload()
{
    if (m_Xdamage) ::dlclose(m_Xdamage);
    if (m_X11) ::dlclose(m_X11);
    m_Xdamage = nullptr;
    m_X11 = nullptr;
    m_XOpenDisplay = nullptr;
    m_XCloseDisplay = nullptr;
    m_XDefaultRootWindow = nullptr;
    m_XConnectionNumber = nullptr;
    m_XPending = nullptr;
    m_XNextEvent = nullptr;
    m_XFlush = nullptr;
    m_XDamageQueryExtension = nullptr;
    m_XDamageCreate = nullptr;
}

} // namespace mw::native::capture
