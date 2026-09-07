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

#include "X11Pointer.h"

#include "../../core/Log.h"

#include <cstdlib>
#include <dlfcn.h>
#include <string>

namespace mw::native::input {
namespace {

/// The soname, not the -dev symlink. libX11.so exists only where the developer
/// package is installed; libX11.so.6 is what every X desktop actually has, and
/// the 6 has not moved since 1996 — it is the ABI version, and X11 is done.
constexpr const char* kSoname = "libX11.so.6";

} // namespace

X11Pointer::~X11Pointer()
{
    close();
}

bool X11Pointer::open()
{
    if (m_Display) return true;

    // No DISPLAY means no X server to talk to: on a Wayland session or a
    // headless KMS host that is the normal state of the world, and asking
    // libX11 anyway would only load a library to be told the same thing.
    const char* display = std::getenv("DISPLAY");
    if (!display || *display == '\0') {
        log::debug("[native] input: no DISPLAY — the pointer cannot be read, relative motion "
                   "will be applied wherever the compositor has it");
        return false;
    }

    // RTLD_LOCAL so nothing else in this process picks up X symbols by accident;
    // RTLD_NOW so a truncated or mismatched library fails here, once, rather
    // than at the first pointer move.
    m_Lib = ::dlopen(kSoname, RTLD_NOW | RTLD_LOCAL);
    if (!m_Lib) {
        log::debug(std::string("[native] input: ") + kSoname +
                   " not available — relative motion will not be brought back onto the captured "
                   "display");
        return false;
    }

    // The casts are the documented dlsym idiom: it returns void*, and the only
    // way to a function pointer is through one. Every symbol is required — a
    // libX11 missing any of these is not a libX11.
    m_XOpenDisplay = reinterpret_cast<decltype(m_XOpenDisplay)>(::dlsym(m_Lib, "XOpenDisplay"));
    m_XCloseDisplay = reinterpret_cast<decltype(m_XCloseDisplay)>(::dlsym(m_Lib, "XCloseDisplay"));
    m_XDefaultRootWindow =
        reinterpret_cast<decltype(m_XDefaultRootWindow)>(::dlsym(m_Lib, "XDefaultRootWindow"));
    m_XQueryPointer = reinterpret_cast<decltype(m_XQueryPointer)>(::dlsym(m_Lib, "XQueryPointer"));
    m_XWarpPointer = reinterpret_cast<decltype(m_XWarpPointer)>(::dlsym(m_Lib, "XWarpPointer"));
    m_XFlush = reinterpret_cast<decltype(m_XFlush)>(::dlsym(m_Lib, "XFlush"));

    if (!m_XOpenDisplay || !m_XCloseDisplay || !m_XDefaultRootWindow || !m_XQueryPointer ||
        !m_XWarpPointer || !m_XFlush) {
        log::warning(std::string("[native] input: ") + kSoname +
                     " loaded but does not export the pointer calls — ignoring it");
        close();
        return false;
    }

    m_Display = m_XOpenDisplay(display);
    if (!m_Display) {
        // A server that is there and will not have us: no XAUTHORITY, or a
        // different user's session. Worth a line, because unlike the cases
        // above this one is usually a mistake somebody can fix.
        log::warning(std::string("[native] input: X server \"") + display +
                     "\" refused the connection — the pointer cannot be brought back onto the "
                     "captured display (XAUTHORITY?)");
        close();
        return false;
    }

    m_Root = m_XDefaultRootWindow(m_Display);
    log::info(std::string("[native] input: attached to X display \"") + display +
              "\" — a pointer left on another screen will be brought back");
    return true;
}

void X11Pointer::close()
{
    if (m_Display && m_XCloseDisplay) m_XCloseDisplay(m_Display);
    m_Display = nullptr;
    m_Root = 0;
    // The library itself stays out of the process: dlclose on libX11 is safe
    // here because nothing else in this binary links it, and unloading returns
    // the memory of a library a headless session will never use again.
    if (m_Lib) ::dlclose(m_Lib);
    m_Lib = nullptr;
    m_XOpenDisplay = nullptr;
    m_XCloseDisplay = nullptr;
    m_XDefaultRootWindow = nullptr;
    m_XQueryPointer = nullptr;
    m_XWarpPointer = nullptr;
    m_XFlush = nullptr;
}

bool X11Pointer::position(int& x, int& y)
{
    if (!m_Display) return false;

    XWindow root = 0;
    XWindow child = 0;
    int rootX = 0;
    int rootY = 0;
    int winX = 0;
    int winY = 0;
    unsigned int mask = 0;
    // False means the pointer is not on the screen this root belongs to. On a
    // single-screen display — which is every modern desktop, multi-monitor
    // included, since they are one screen under RandR — that does not happen.
    if (!m_XQueryPointer(m_Display, m_Root, &root, &child, &rootX, &rootY, &winX, &winY, &mask))
        return false;

    x = rootX;
    y = rootY;
    return true;
}

bool X11Pointer::warp(int x, int y)
{
    if (!m_Display) return false;
    // Source window None: move it from wherever it is, with no condition on
    // where that was. The zeroes are the source rectangle, unused in that case.
    m_XWarpPointer(m_Display, 0, m_Root, 0, 0, 0, 0, x, y);
    m_XFlush(m_Display);
    return true;
}

} // namespace mw::native::input
