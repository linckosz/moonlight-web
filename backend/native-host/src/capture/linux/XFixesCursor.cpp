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

#include "XFixesCursor.h"

#include "../../core/Log.h"
#include "../../platform/linux/XlibLock.h"

#include <dlfcn.h>
#include <poll.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <mutex>
#include <set>
#include <type_traits>

namespace mw::native::capture {
namespace {

using XDisplay = void;
using XWindow = unsigned long;
using XAtom = unsigned long;

/// XEvent is a union of 24 longs; only its type is read here.
struct XEventBuffer
{
    int type;
    long pad[24];
};

/// Xfixes.h's XFixesCursorImage, version 2 and later.
struct XFixesCursorImage
{
    short x, y;
    unsigned short width, height;
    unsigned short xhot, yhot;
    unsigned long cursorSerial;
    unsigned long* pixels;
    XAtom atom;
    const char* name;
};

constexpr int kXFixesCursorNotify = 1;
constexpr unsigned long kXFixesDisplayCursorNotifyMask = 1;

// Xlib's IO error handler is the process's, and the one in place ends it: Qt's
// says "The X11 connection broke" and calls Xlib's default, which exits — the
// worker went with gamescope's Xwayland (bench, G1). This one is put in front
// of it: a connection of a reader here is let go (the reader's exit handler
// then stops it), any other gets what it always got.
using IoErrorHandler = int (*)(XDisplay*);
std::mutex g_ioMutex;
std::set<XDisplay*> g_readers;
IoErrorHandler g_previousIoHandler = nullptr;

int onIoError(XDisplay* display)
{
    {
        std::lock_guard<std::mutex> lock(g_ioMutex);
        if (g_readers.count(display)) return 0;
    }
    return g_previousIoHandler ? g_previousIoHandler(display) : 0;
}

} // namespace

struct XFixesCursor::Api
{
    XDisplay* (*openDisplay)(const char*) = nullptr;
    int (*closeDisplay)(XDisplay*) = nullptr;
    XWindow (*defaultRootWindow)(XDisplay*) = nullptr;
    int (*connectionNumber)(XDisplay*) = nullptr;
    int (*pending)(XDisplay*) = nullptr;
    int (*nextEvent)(XDisplay*, XEventBuffer*) = nullptr;
    int (*queryPointer)(XDisplay*, XWindow, XWindow*, XWindow*, int*, int*, int*, int*,
                        unsigned int*) = nullptr;
    int (*free)(void*) = nullptr;
    int (*getGeometry)(XDisplay*, XWindow, XWindow*, int*, int*, unsigned int*, unsigned int*,
                       unsigned int*, unsigned int*) = nullptr;
    IoErrorHandler (*setIOErrorHandler)(IoErrorHandler) = nullptr;
    void (*setIOErrorExitHandler)(XDisplay*, void (*)(XDisplay*, void*), void*) = nullptr;
    int (*fixesQueryExtension)(XDisplay*, int*, int*) = nullptr;
    void (*fixesSelectCursorInput)(XDisplay*, XWindow, unsigned long) = nullptr;
    XFixesCursorImage* (*fixesGetCursorImage)(XDisplay*) = nullptr;

    static const Api* get()
    {
        static Api api;
        static const bool loaded = [] {
            void* x11 = ::dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
            void* fixes = ::dlopen("libXfixes.so.3", RTLD_NOW | RTLD_LOCAL);
            if (!x11 || !fixes) return false;
            const auto load = [](void* lib, auto& fn, const char* name) {
                fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(::dlsym(lib, name));
                return fn != nullptr;
            };
            return load(x11, api.openDisplay, "XOpenDisplay") &&
                   load(x11, api.closeDisplay, "XCloseDisplay") &&
                   load(x11, api.defaultRootWindow, "XDefaultRootWindow") &&
                   load(x11, api.connectionNumber, "XConnectionNumber") &&
                   load(x11, api.pending, "XPending") && load(x11, api.nextEvent, "XNextEvent") &&
                   load(x11, api.queryPointer, "XQueryPointer") && load(x11, api.free, "XFree") &&
                   load(x11, api.getGeometry, "XGetGeometry") &&
                   load(x11, api.setIOErrorHandler, "XSetIOErrorHandler") &&
                   load(x11, api.setIOErrorExitHandler, "XSetIOErrorExitHandler") &&
                   load(fixes, api.fixesQueryExtension, "XFixesQueryExtension") &&
                   load(fixes, api.fixesSelectCursorInput, "XFixesSelectCursorInput") &&
                   load(fixes, api.fixesGetCursorImage, "XFixesGetCursorImage");
        }();
        return loaded ? &api : nullptr;
    }
};

XFixesCursor::~XFixesCursor()
{
    stop();
}

bool XFixesCursor::start(const std::string& display,
                         std::function<void(const CursorState&)> changed, std::string& error)
{
    stop();
    const Api* api = Api::get();
    if (!api) {
        error = "libX11 1.7 or later and libXfixes are needed";
        return false;
    }
    // Every Xlib call under the process's lock (XlibLock.h), to the end of the
    // set-up. g_ioMutex is taken under it: onIoError takes it from inside a
    // call into Xlib.
    std::unique_lock<std::mutex> xlib(platform::xlibMutex());
    m_Display = api->openDisplay(display.c_str());
    if (!m_Display) {
        error = "cannot open the X display " + display;
        return false;
    }
    // gamescope's Xwayland goes with gamescope, and a connection that breaks
    // makes Xlib end the whole process — the worker, mid-stream, before the
    // session can say why (bench, G1). Its own handler instead: the reader
    // stops, the session goes on to notice gamescope went. Xlib 1.7 and later
    // have it; without it there is no reader at all, never an exit.
    m_Broken.store(false);
    {
        static std::once_flag installed;
        std::call_once(installed,
                       [api] { g_previousIoHandler = api->setIOErrorHandler(&onIoError); });
        std::lock_guard<std::mutex> lock(g_ioMutex);
        g_readers.insert(m_Display);
    }
    api->setIOErrorExitHandler(
        m_Display,
        [](XDisplay*, void* self) { static_cast<XFixesCursor*>(self)->m_Broken.store(true); },
        this);
    int errorBase = 0;
    if (!api->fixesQueryExtension(m_Display, &m_EventBase, &errorBase)) {
        error = "the X display " + display + " has no XFixes";
        api->closeDisplay(m_Display);
        m_Display = nullptr;
        return false;
    }
    m_Root = api->defaultRootWindow(m_Display);
    api->fixesSelectCursorInput(m_Display, m_Root, kXFixesDisplayCursorNotifyMask);
    xlib.unlock();
    m_Changed = std::move(changed);
    m_State = CursorState{};
    m_Stopping.store(false);
    m_Thread = std::thread([this] { run(); });
    return true;
}

void XFixesCursor::stop()
{
    m_Stopping.store(true);
    if (m_Thread.joinable()) m_Thread.join();
    if (m_Display) {
        // A broken connection is left as it is: closing it would talk to a
        // server that is gone. Still one of ours until then, should it break
        // while closing.
        if (!m_Broken.load()) {
            std::lock_guard<std::mutex> xlib(platform::xlibMutex());
            Api::get()->closeDisplay(m_Display);
        }
        {
            std::lock_guard<std::mutex> lock(g_ioMutex);
            g_readers.erase(m_Display);
        }
        m_Display = nullptr;
    }
}

void XFixesCursor::run()
{
    const Api* api = Api::get();
    std::unique_lock<std::mutex> xlib(platform::xlibMutex());
    const int fd = api->connectionNumber(m_Display);
    bool shapeDue = true;

    // gamescope shows the focused window scaled to fill its output, aspect kept
    // and centred — a game at 1366x768 fills a 1920x1080 picture — while X
    // reports the pointer in that window's own pixels. Drawn as read, the
    // pointer sat up and left of where the game saw it, by the ratio (bench:
    // ACC at 768p in a 1080p stream). The root is the output: gamescope keeps
    // it at its own size whatever mode a game asks for.
    unsigned int rootW = 0, rootH = 0;
    {
        XWindow r = 0;
        int rx = 0, ry = 0;
        unsigned int border = 0, depth = 0;
        api->getGeometry(m_Display, m_Root, &r, &rx, &ry, &rootW, &rootH, &border, &depth);
    }
    xlib.unlock();
    XWindow scaledWindow = 0;
    int winX = 0, winY = 0;
    unsigned int winW = 0, winH = 0;
    auto lastGeometry = std::chrono::steady_clock::time_point{};
    while (!m_Stopping.load() && !m_Broken.load()) {
        pollfd p{fd, POLLIN, 0};
        ::poll(&p, 1, 8);
        // Xlib under the process's lock from here to the pointer read; the
        // owner's callback below is made without it.
        xlib.lock();
        while (!m_Broken.load() && api->pending(m_Display) > 0) {
            XEventBuffer event{};
            api->nextEvent(m_Display, &event);
            if (event.type == m_EventBase + kXFixesCursorNotify) shapeDue = true;
        }
        if (m_Broken.load()) break; // xlib lets the lock go as run() returns
        bool changed = false;

        if (shapeDue) {
            shapeDue = false;
            if (XFixesCursorImage* image = api->fixesGetCursorImage(m_Display)) {
                const int w = image->width;
                const int h = image->height;
                m_State.width = w;
                m_State.height = h;
                m_State.hotspotX = image->xhot;
                m_State.hotspotY = image->yhot;
                m_State.pixels.assign(static_cast<size_t>(w) * h * 4, 0);
                int inkW = 0, inkH = 0;
                for (int i = 0; i < w * h; ++i) {
                    // ARGB in the low 32 bits of a long — BGRA once laid out
                    // little-endian, the order CursorState keeps.
                    const auto argb = static_cast<uint32_t>(image->pixels[i]);
                    std::memcpy(&m_State.pixels[static_cast<size_t>(i) * 4], &argb, 4);
                    if ((argb >> 24) != 0) {
                        if (i % w + 1 > inkW) inkW = i % w + 1;
                        if (i / w + 1 > inkH) inkH = i / w + 1;
                    }
                }
                m_State.inkWidth = inkW;
                m_State.inkHeight = inkH;
                m_State.invert.assign(static_cast<size_t>(w) * h, 0);
                ++m_State.shapeVersion;
                api->free(image);
                changed = true;
            }
        }

        XWindow rootReturn = 0, child = 0;
        int rootX = 0, rootY = 0, childX = 0, childY = 0;
        unsigned int mask = 0;
        if (api->queryPointer(m_Display, m_Root, &rootReturn, &child, &rootX, &rootY, &childX,
                              &childY, &mask)) {
            // The window under the pointer is the one gamescope shows. Its
            // size is read again when it changes and every quarter second: a
            // game switches resolution without a new window.
            const auto now = std::chrono::steady_clock::now();
            if (child != scaledWindow || now - lastGeometry > std::chrono::milliseconds(250)) {
                scaledWindow = child;
                lastGeometry = now;
                winW = winH = 0;
                XWindow r = 0;
                unsigned int border = 0, depth = 0;
                if (child == 0 || !api->getGeometry(m_Display, child, &r, &winX, &winY, &winW,
                                                    &winH, &border, &depth))
                    winW = winH = 0;
            }
            double px = rootX, py = rootY;
            if (winW > 0 && winH > 0 && rootW > 0 && rootH > 0 &&
                (winW != rootW || winH != rootH)) {
                const double scale =
                    std::min(static_cast<double>(rootW) / winW, static_cast<double>(rootH) / winH);
                px = (rootW - winW * scale) / 2 + (rootX - winX) * scale;
                py = (rootH - winH * scale) / 2 + (rootY - winY) * scale;
            }
            // The image's top-left, the hotspot taken off (CursorState).
            const int x = static_cast<int>(std::lround(px)) - m_State.hotspotX;
            const int y = static_cast<int>(std::lround(py)) - m_State.hotspotY;
            if (x != m_State.x || y != m_State.y) {
                m_State.x = x;
                m_State.y = y;
                changed = true;
            }
        }
        xlib.unlock();
        // Shown when the app shows one: a game that hides the pointer gives X
        // an empty one, which has no ink. gamescope's own word on it
        // (GAMESCOPE_CURSOR_VISIBLE_FEEDBACK) is about the pointer on ITS
        // screen, never in the picture, and it says hidden after an absolute
        // move — every move of a viewer in desktop mode (bench, G0).
        const bool visibleNow = m_State.inkWidth > 0 && m_State.inkHeight > 0;
        if (visibleNow != m_State.visible) {
            m_State.visible = visibleNow;
            changed = true;
        }
        if (changed && m_Changed && !m_Broken.load()) m_Changed(m_State);
    }
}

} // namespace mw::native::capture
