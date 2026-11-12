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

#include "X11Layout.h"

#include "../../platform/linux/XlibLock.h"

#include <atomic>
#include <cstdlib>
#include <dlfcn.h>
#include <mutex>

namespace mw::native::input {
namespace {

// The sonames, not the -dev symlinks (X11Pointer.cpp says why).
constexpr const char* kX11 = "libX11.so.6";
constexpr const char* kXrandr = "libXrandr.so.2";

// Xlib's and Xrandr.h's types, spelled out so no X header is needed: the same
// members in the same order, laid out by the same ABI rules. XIDs, atoms and
// times are unsigned long; Connection and Rotation are unsigned short.
using Display = void;
using XID = unsigned long;
using Atom = unsigned long;
using Time = unsigned long;

struct ScreenResources
{
    Time timestamp;
    Time configTimestamp;
    int ncrtc;
    XID* crtcs;
    int noutput;
    XID* outputs;
    int nmode;
    void* modes;
};

struct OutputInfo
{
    Time timestamp;
    XID crtc;
    char* name;
    int nameLen;
    unsigned long mmWidth;
    unsigned long mmHeight;
    unsigned short connection;
    unsigned short subpixelOrder;
    int ncrtc;
    XID* crtcs;
    int nclone;
    XID* clones;
    int nmode;
    int npreferred;
    XID* modes;
};

struct CrtcInfo
{
    Time timestamp;
    int x;
    int y;
    unsigned int width;
    unsigned int height;
    XID mode;
    unsigned short rotation;
    int noutput;
    XID* outputs;
    unsigned short rotations;
    int npossible;
    XID* possible;
};

constexpr unsigned short kConnected = 0; // RR_Connected
constexpr Atom kAnyPropertyType = 0;
constexpr int kSuccess = 0;

/// Xlib's error handler is the process's, and its default exits: an output
/// unplugged between listing it and asking for it would take the worker along.
/// Counted instead, for the length of one read.
std::atomic<int> g_Errors{0};
int countError(Display* /*display*/, void* /*event*/)
{
    g_Errors.fetch_add(1);
    return 0;
}

/// What Xlib calls when the connection dies (libX11 1.7): returning leaves the
/// display dead rather than exit() the process; the flag stops the read.
void onConnectionLost(Display* /*display*/, void* userData)
{
    if (auto* dead = static_cast<std::atomic<bool>*>(userData)) dead->store(true);
}

struct Lib
{
    void* x11 = nullptr;
    void* xrandr = nullptr;

    Display* (*openDisplay)(const char*) = nullptr;
    int (*closeDisplay)(Display*) = nullptr;
    XID (*defaultRootWindow)(Display*) = nullptr;
    int (*getGeometry)(Display*, XID, XID*, int*, int*, unsigned int*, unsigned int*, unsigned int*,
                       unsigned int*) = nullptr;
    Atom (*internAtom)(Display*, const char*, int) = nullptr;
    int (*xFree)(void*) = nullptr;
    int (*xSync)(Display*, int) = nullptr;
    using ErrorHandler = int (*)(Display*, void*);
    ErrorHandler (*setErrorHandler)(ErrorHandler) = nullptr;
    using ExitHandler = void (*)(Display*, void*);
    void (*setIOErrorExitHandler)(Display*, ExitHandler, void*) = nullptr;

    int (*queryExtension)(Display*, int*, int*) = nullptr;
    int (*queryVersion)(Display*, int*, int*) = nullptr;
    ScreenResources* (*getScreenResourcesCurrent)(Display*, XID) = nullptr;
    void (*freeScreenResources)(ScreenResources*) = nullptr;
    OutputInfo* (*getOutputInfo)(Display*, ScreenResources*, XID) = nullptr;
    void (*freeOutputInfo)(OutputInfo*) = nullptr;
    CrtcInfo* (*getCrtcInfo)(Display*, ScreenResources*, XID) = nullptr;
    void (*freeCrtcInfo)(CrtcInfo*) = nullptr;
    int (*getOutputProperty)(Display*, XID, Atom, long, long, int, int, Atom, Atom*, int*,
                             unsigned long*, unsigned long*, unsigned char**) = nullptr;

    ~Lib()
    {
        if (xrandr) ::dlclose(xrandr);
        if (x11) ::dlclose(x11);
    }

    bool open(std::string& why)
    {
        x11 = ::dlopen(kX11, RTLD_NOW | RTLD_LOCAL);
        xrandr = x11 ? ::dlopen(kXrandr, RTLD_NOW | RTLD_LOCAL) : nullptr;
        if (!x11 || !xrandr) {
            why = std::string(x11 ? kXrandr : kX11) + " not available";
            return false;
        }
        const auto sym = [](void* lib, const char* name) { return ::dlsym(lib, name); };
        openDisplay = reinterpret_cast<decltype(openDisplay)>(sym(x11, "XOpenDisplay"));
        closeDisplay = reinterpret_cast<decltype(closeDisplay)>(sym(x11, "XCloseDisplay"));
        defaultRootWindow =
            reinterpret_cast<decltype(defaultRootWindow)>(sym(x11, "XDefaultRootWindow"));
        getGeometry = reinterpret_cast<decltype(getGeometry)>(sym(x11, "XGetGeometry"));
        internAtom = reinterpret_cast<decltype(internAtom)>(sym(x11, "XInternAtom"));
        xFree = reinterpret_cast<decltype(xFree)>(sym(x11, "XFree"));
        xSync = reinterpret_cast<decltype(xSync)>(sym(x11, "XSync"));
        setErrorHandler = reinterpret_cast<decltype(setErrorHandler)>(sym(x11, "XSetErrorHandler"));
        setIOErrorExitHandler =
            reinterpret_cast<decltype(setIOErrorExitHandler)>(sym(x11, "XSetIOErrorExitHandler"));
        queryExtension =
            reinterpret_cast<decltype(queryExtension)>(sym(xrandr, "XRRQueryExtension"));
        queryVersion = reinterpret_cast<decltype(queryVersion)>(sym(xrandr, "XRRQueryVersion"));
        getScreenResourcesCurrent = reinterpret_cast<decltype(getScreenResourcesCurrent)>(
            sym(xrandr, "XRRGetScreenResourcesCurrent"));
        freeScreenResources =
            reinterpret_cast<decltype(freeScreenResources)>(sym(xrandr, "XRRFreeScreenResources"));
        getOutputInfo = reinterpret_cast<decltype(getOutputInfo)>(sym(xrandr, "XRRGetOutputInfo"));
        freeOutputInfo =
            reinterpret_cast<decltype(freeOutputInfo)>(sym(xrandr, "XRRFreeOutputInfo"));
        getCrtcInfo = reinterpret_cast<decltype(getCrtcInfo)>(sym(xrandr, "XRRGetCrtcInfo"));
        freeCrtcInfo = reinterpret_cast<decltype(freeCrtcInfo)>(sym(xrandr, "XRRFreeCrtcInfo"));
        getOutputProperty =
            reinterpret_cast<decltype(getOutputProperty)>(sym(xrandr, "XRRGetOutputProperty"));
        // XSetIOErrorExitHandler is libX11 1.7's: the one call the read goes
        // on without, as the server dying in the middle of it is a narrow
        // window X11Damage, which lives for the whole session, cannot afford.
        if (!openDisplay || !closeDisplay || !defaultRootWindow || !getGeometry || !internAtom ||
            !xFree || !xSync || !setErrorHandler || !queryExtension || !queryVersion ||
            !getScreenResourcesCurrent || !freeScreenResources || !getOutputInfo ||
            !freeOutputInfo || !getCrtcInfo || !freeCrtcInfo || !getOutputProperty) {
            why = std::string(kX11) + " or " + kXrandr + " lacks a call this needs";
            return false;
        }
        return true;
    }

    /// One output property, whole: its bytes for format 8, and for format 32
    /// Xlib's longs — one per item, whatever their width on the wire.
    bool property(Display* display, XID output, Atom atom, int& format,
                  std::vector<unsigned char>& bytes, std::vector<long>& items)
    {
        if (atom == 0) return false;
        Atom type = 0;
        unsigned long count = 0;
        unsigned long after = 0;
        unsigned char* data = nullptr;
        format = 0;
        // 8192 words is 32 KiB: every EDID, extension blocks included.
        if (getOutputProperty(display, output, atom, 0, 8192, 0, 0, kAnyPropertyType, &type,
                              &format, &count, &after, &data) != kSuccess)
            return false;
        bool ok = false;
        if (data && type != 0 && count > 0) {
            if (format == 8) {
                bytes.assign(data, data + count);
                ok = true;
            } else if (format == 32) {
                const long* longs = reinterpret_cast<const long*>(data);
                items.assign(longs, longs + count);
                ok = true;
            }
        }
        if (data) xFree(data);
        return ok;
    }
};

} // namespace

bool X11Layout::read(std::vector<X11Output>& outputs, int& rootWidth, int& rootHeight,
                     std::string& displayUsed, std::string& why)
{
    outputs.clear();
    rootWidth = 0;
    rootHeight = 0;
    displayUsed.clear();
    why.clear();

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
    displayUsed = display;

    // Under the process's Xlib lock, for the whole read (XlibLock.h): the
    // error handler set below is the process's, and a session's capture loop
    // and its turn watch (TurnWatch.h) both read, from two threads.
    std::lock_guard<std::mutex> xlib(platform::xlibMutex());

    Lib lib;
    if (!lib.open(why)) return false;

    Display* dpy = lib.openDisplay(display);
    if (!dpy) {
        why = std::string("the X server \"") + display + "\" refused the connection";
        return false;
    }
    std::atomic<bool> dead{false};
    if (lib.setIOErrorExitHandler) lib.setIOErrorExitHandler(dpy, &onConnectionLost, &dead);
    g_Errors.store(0);
    const Lib::ErrorHandler previous = lib.setErrorHandler(&countError);

    bool ok = false;
    int eventBase = 0;
    int errorBase = 0;
    int major = 0;
    int minor = 0;
    const XID root = lib.defaultRootWindow(dpy);
    XID rootReturn = 0;
    int gx = 0;
    int gy = 0;
    unsigned int gw = 0;
    unsigned int gh = 0;
    unsigned int border = 0;
    unsigned int depth = 0;
    if (!lib.queryExtension(dpy, &eventBase, &errorBase) ||
        !lib.queryVersion(dpy, &major, &minor) || (major == 1 && minor < 3) || major < 1) {
        why = "the X server has no RandR 1.3";
    } else if (!lib.getGeometry(dpy, root, &rootReturn, &gx, &gy, &gw, &gh, &border, &depth) ||
               dead.load()) {
        why = "the X server did not give its root's size";
    } else if (ScreenResources* res = lib.getScreenResourcesCurrent(dpy, root)) {
        rootWidth = static_cast<int>(gw);
        rootHeight = static_cast<int>(gh);
        // Only if they exist: an atom created here would outlive the read in
        // the server for nothing.
        const Atom edidAtom = lib.internAtom(dpy, "EDID", 1);
        const Atom connectorAtom = lib.internAtom(dpy, "CONNECTOR_ID", 1);
        for (int i = 0; i < res->noutput && !dead.load(); ++i) {
            OutputInfo* info = lib.getOutputInfo(dpy, res, res->outputs[i]);
            if (!info) continue;
            if (info->connection == kConnected && info->crtc != 0) {
                CrtcInfo* crtc = lib.getCrtcInfo(dpy, res, info->crtc);
                if (crtc && crtc->width > 0 && crtc->height > 0) {
                    X11Output out;
                    if (info->name && info->nameLen > 0)
                        out.name.assign(info->name, static_cast<size_t>(info->nameLen));
                    out.x = crtc->x;
                    out.y = crtc->y;
                    out.width = static_cast<int>(crtc->width);
                    out.height = static_cast<int>(crtc->height);
                    out.rotation = crtc->rotation;
                    int format = 0;
                    std::vector<unsigned char> bytes;
                    std::vector<long> items;
                    if (lib.property(dpy, res->outputs[i], edidAtom, format, bytes, items) &&
                        format == 8)
                        out.edid.assign(bytes.begin(), bytes.end());
                    bytes.clear();
                    items.clear();
                    if (lib.property(dpy, res->outputs[i], connectorAtom, format, bytes, items) &&
                        format == 32 && !items.empty())
                        out.connectorId = static_cast<uint32_t>(items.front());
                    outputs.push_back(std::move(out));
                }
                if (crtc) lib.freeCrtcInfo(crtc);
            }
            lib.freeOutputInfo(info);
        }
        lib.freeScreenResources(res);
        ok = !dead.load();
        if (!ok) why = "the X server went away during the read";
    } else {
        why = "the X server did not list its outputs";
    }

    // Every reply is in, so any error is too — before the handler goes back.
    if (!dead.load()) lib.xSync(dpy, 0);
    lib.setErrorHandler(previous);
    // A dead connection is left alone: nothing more may be done with it.
    if (!dead.load()) lib.closeDisplay(dpy);
    if (ok && outputs.empty()) {
        why = "the X server shows nothing";
        ok = false;
    }
    return ok;
}

} // namespace mw::native::input
