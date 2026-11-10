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

#include "WaylandLayout.h"

#include "../../core/Log.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <dlfcn.h>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace mw::native::input {
namespace {

/// The soname every Wayland desktop has; the unversioned .so is the -dev
/// package's. The 0 is the ABI, unchanged since 1.0.
constexpr const char* kSoname = "libwayland-client.so.0";

// ── libwayland's ABI, spelled out ───────────────────────────────────────────
//
// wayland-util.h's two structs, in layout and nothing else: an interface is a
// name, a version and two message tables; a message is a name, a signature and
// the interfaces of its object arguments. Both are read by libwayland when a
// request is marshalled or an event demarshalled, so they must be exactly this.

struct WlInterface;

struct WlMessage
{
    const char* name;
    const char* signature;
    const WlInterface** types;
};

struct WlInterface
{
    const char* name;
    int version;
    int methodCount;
    const WlMessage* methods;
    int eventCount;
    const WlMessage* events;
};

/// Opaque on purpose: a proxy is only ever handed back to libwayland.
using WlProxy = void;

// The xdg-output extension (wayland-protocols, unstable/xdg-output), the two
// interfaces of it that matter here. wl_registry and wl_output are exported by
// libwayland-client itself and resolved at load; this pair is not, and would
// otherwise come from wayland-scanner.

const WlInterface* kNoTypes[8] = {nullptr, nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr, nullptr};

extern const WlInterface kXdgOutputInterface;
/// get_xdg_output(new_id zxdg_output_v1, object wl_output): the second slot is
/// libwayland's own wl_output_interface, known only once the library is open.
const WlInterface* g_getXdgOutputTypes[2] = {&kXdgOutputInterface, nullptr};

const WlMessage kXdgOutputManagerRequests[] = {
    {"destroy", "", kNoTypes},
    {"get_xdg_output", "no", g_getXdgOutputTypes},
};
const WlInterface kXdgOutputManagerInterface = {"zxdg_output_manager_v1",  3, 2,
                                                kXdgOutputManagerRequests, 0, nullptr};

const WlMessage kXdgOutputRequests[] = {{"destroy", "", kNoTypes}};
const WlMessage kXdgOutputEvents[] = {
    {"logical_position", "ii", kNoTypes},
    {"logical_size", "ii", kNoTypes},
    {"done", "", kNoTypes},
    {"name", "2s", kNoTypes},
    {"description", "2s", kNoTypes},
};
const WlInterface kXdgOutputInterface = {"zxdg_output_v1",   3, 1,
                                         kXdgOutputRequests, 5, kXdgOutputEvents};

/// Opcodes, in the order the protocol XML lists the requests.
constexpr uint32_t kDisplayGetRegistry = 1;
constexpr uint32_t kRegistryBind = 0;
constexpr uint32_t kOutputRelease = 0; // wl_output v3+
constexpr uint32_t kXdgManagerDestroy = 0;
constexpr uint32_t kXdgManagerGetXdgOutput = 1;
constexpr uint32_t kXdgOutputDestroy = 0;

constexpr uint32_t kWantOutputVersion = 4; // name/description arrive from 4
constexpr uint32_t kWantXdgVersion = 3;    // name from 2, done retired in 3
constexpr uint32_t kOutputModeCurrent = 1; // WL_OUTPUT_MODE_CURRENT

// Listener tables are arrays of function pointers, one per event in protocol
// order; libwayland indexes them by opcode. Every entry has to exist even when
// the compositor is too old to send it, because the table is sized by the
// interface, not by what arrives.

struct RegistryListener
{
    void (*global)(void*, WlProxy*, uint32_t, const char*, uint32_t);
    void (*globalRemove)(void*, WlProxy*, uint32_t);
};

struct OutputListener
{
    void (*geometry)(void*, WlProxy*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*,
                     const char*, int32_t);
    void (*mode)(void*, WlProxy*, uint32_t, int32_t, int32_t, int32_t);
    void (*done)(void*, WlProxy*);
    void (*scale)(void*, WlProxy*, int32_t);
    void (*name)(void*, WlProxy*, const char*);
    void (*description)(void*, WlProxy*, const char*);
};

struct XdgOutputListener
{
    void (*logicalPosition)(void*, WlProxy*, int32_t, int32_t);
    void (*logicalSize)(void*, WlProxy*, int32_t, int32_t);
    void (*done)(void*, WlProxy*);
    void (*name)(void*, WlProxy*, const char*);
    void (*description)(void*, WlProxy*, const char*);
};

/// libwayland-client, the seven entry points this needs and the two interface
/// tables it exports.
struct Lib
{
    void* handle = nullptr;

    WlProxy* (*displayConnect)(const char*) = nullptr;
    void (*displayDisconnect)(WlProxy*) = nullptr;
    int (*displayRoundtrip)(WlProxy*) = nullptr;
    WlProxy* (*marshalConstructor)(WlProxy*, uint32_t, const WlInterface*, ...) = nullptr;
    WlProxy* (*marshalConstructorVersioned)(WlProxy*, uint32_t, const WlInterface*, uint32_t,
                                            ...) = nullptr;
    void (*marshal)(WlProxy*, uint32_t, ...) = nullptr;
    int (*addListener)(WlProxy*, void (**)(void), void*) = nullptr;
    void (*proxyDestroy)(WlProxy*) = nullptr;

    const WlInterface* registryInterface = nullptr;
    const WlInterface* outputInterface = nullptr;

    ~Lib()
    {
        if (handle) ::dlclose(handle);
    }

    bool open(std::string& why)
    {
        handle = ::dlopen(kSoname, RTLD_NOW | RTLD_LOCAL);
        if (!handle) {
            why = std::string(kSoname) + " not available";
            return false;
        }
        // The casts are the documented dlsym idiom: it returns void*, and the
        // only way to a function pointer is through one.
        displayConnect =
            reinterpret_cast<decltype(displayConnect)>(::dlsym(handle, "wl_display_connect"));
        displayDisconnect =
            reinterpret_cast<decltype(displayDisconnect)>(::dlsym(handle, "wl_display_disconnect"));
        displayRoundtrip =
            reinterpret_cast<decltype(displayRoundtrip)>(::dlsym(handle, "wl_display_roundtrip"));
        marshalConstructor = reinterpret_cast<decltype(marshalConstructor)>(
            ::dlsym(handle, "wl_proxy_marshal_constructor"));
        marshalConstructorVersioned = reinterpret_cast<decltype(marshalConstructorVersioned)>(
            ::dlsym(handle, "wl_proxy_marshal_constructor_versioned"));
        marshal = reinterpret_cast<decltype(marshal)>(::dlsym(handle, "wl_proxy_marshal"));
        addListener =
            reinterpret_cast<decltype(addListener)>(::dlsym(handle, "wl_proxy_add_listener"));
        proxyDestroy =
            reinterpret_cast<decltype(proxyDestroy)>(::dlsym(handle, "wl_proxy_destroy"));
        registryInterface =
            static_cast<const WlInterface*>(::dlsym(handle, "wl_registry_interface"));
        outputInterface = static_cast<const WlInterface*>(::dlsym(handle, "wl_output_interface"));

        if (!displayConnect || !displayDisconnect || !displayRoundtrip || !marshalConstructor ||
            !marshalConstructorVersioned || !marshal || !addListener || !proxyDestroy ||
            !registryInterface || !outputInterface) {
            why = std::string(kSoname) + " loaded but does not export the client calls";
            return false;
        }
        g_getXdgOutputTypes[1] = outputInterface;
        return true;
    }
};

/// One wl_output as it reports itself: both the plain protocol's view (position
/// and mode in physical pixels, an integer scale) and xdg-output's logical one.
/// The logical one is the truth for an absolute device; the plain one is what a
/// compositor without xdg-output leaves us, and for integer scales it agrees.
struct OutputState
{
    WlProxy* output = nullptr;
    uint32_t outputVersion = 0;
    WlProxy* xdg = nullptr;
    std::string name;
    int geomX = 0;
    int geomY = 0;
    int modeW = 0;
    int modeH = 0;
    int scale = 1;
    int32_t transform = 0;
    bool haveLogical = false;
    int logicalX = 0;
    int logicalY = 0;
    int logicalW = 0;
    int logicalH = 0;
};

struct Session
{
    Lib* lib = nullptr;
    WlProxy* manager = nullptr;
    uint32_t managerVersion = 0;
    std::vector<std::unique_ptr<OutputState>> outputs;
};

// ── Event handlers ──────────────────────────────────────────────────────────

void onOutputGeometry(void* data, WlProxy*, int32_t x, int32_t y, int32_t, int32_t, int32_t,
                      const char*, const char*, int32_t transform)
{
    auto* out = static_cast<OutputState*>(data);
    out->geomX = x;
    out->geomY = y;
    out->transform = transform;
}

void onOutputMode(void* data, WlProxy*, uint32_t flags, int32_t width, int32_t height, int32_t)
{
    auto* out = static_cast<OutputState*>(data);
    if (flags & kOutputModeCurrent) {
        out->modeW = width;
        out->modeH = height;
    }
}

void onOutputDone(void*, WlProxy*) {}

void onOutputScale(void* data, WlProxy*, int32_t factor)
{
    auto* out = static_cast<OutputState*>(data);
    if (factor > 0) out->scale = factor;
}

void onOutputName(void* data, WlProxy*, const char* name)
{
    auto* out = static_cast<OutputState*>(data);
    if (name && out->name.empty()) out->name = name;
}

void onOutputDescription(void*, WlProxy*, const char*) {}

const OutputListener kOutputListener = {onOutputGeometry, onOutputMode, onOutputDone,
                                        onOutputScale,    onOutputName, onOutputDescription};

void onXdgLogicalPosition(void* data, WlProxy*, int32_t x, int32_t y)
{
    auto* out = static_cast<OutputState*>(data);
    out->logicalX = x;
    out->logicalY = y;
}

void onXdgLogicalSize(void* data, WlProxy*, int32_t width, int32_t height)
{
    auto* out = static_cast<OutputState*>(data);
    out->logicalW = width;
    out->logicalH = height;
    out->haveLogical = width > 0 && height > 0;
}

void onXdgDone(void*, WlProxy*) {}

void onXdgName(void* data, WlProxy*, const char* name)
{
    // xdg-output's name and wl_output v4's are the same string by
    // specification; whichever arrives first is kept.
    auto* out = static_cast<OutputState*>(data);
    if (name && out->name.empty()) out->name = name;
}

void onXdgDescription(void*, WlProxy*, const char*) {}

const XdgOutputListener kXdgOutputListener = {onXdgLogicalPosition, onXdgLogicalSize, onXdgDone,
                                              onXdgName, onXdgDescription};

void onGlobal(void* data, WlProxy* registry, uint32_t name, const char* interface, uint32_t version)
{
    auto* session = static_cast<Session*>(data);
    Lib* lib = session->lib;
    if (!interface) return;

    if (std::strcmp(interface, "wl_output") == 0) {
        auto out = std::make_unique<OutputState>();
        out->outputVersion = version < kWantOutputVersion ? version : kWantOutputVersion;
        // wl_registry.bind(name, interface, version, new_id): the new id is the
        // trailing NULL, filled in by libwayland.
        out->output = lib->marshalConstructorVersioned(
            registry, kRegistryBind, lib->outputInterface, out->outputVersion, name,
            lib->outputInterface->name, out->outputVersion, nullptr);
        if (!out->output) return;
        lib->addListener(
            out->output,
            reinterpret_cast<void (**)(void)>(const_cast<OutputListener*>(&kOutputListener)),
            out.get());
        session->outputs.push_back(std::move(out));
        return;
    }

    if (std::strcmp(interface, "zxdg_output_manager_v1") == 0 && !session->manager) {
        session->managerVersion = version < kWantXdgVersion ? version : kWantXdgVersion;
        session->manager = lib->marshalConstructorVersioned(
            registry, kRegistryBind, &kXdgOutputManagerInterface, session->managerVersion, name,
            kXdgOutputManagerInterface.name, session->managerVersion, nullptr);
    }
}

void onGlobalRemove(void*, WlProxy*, uint32_t) {}

const RegistryListener kRegistryListener = {onGlobal, onGlobalRemove};

// ── The socket ──────────────────────────────────────────────────────────────

/// Connect to the session's compositor: WAYLAND_DISPLAY when the host runs
/// inside the session, else every socket under /run/user — the host may be a
/// service with no environment of its own, and a user's runtime directory is
/// where every compositor listens.
WlProxy* connectAny(Lib& lib, std::string& socketUsed, std::string& why)
{
    const char* env = std::getenv("WAYLAND_DISPLAY");
    if (env && *env) {
        if (WlProxy* display = lib.displayConnect(nullptr)) {
            socketUsed = std::string("WAYLAND_DISPLAY=") + env;
            return display;
        }
        why = std::string("WAYLAND_DISPLAY=") + env + " refused the connection";
    }

    // No variable: the process was started outside any session. Absolute
    // paths are accepted by wl_display_connect since libwayland 1.20 (2021),
    // which every distribution this host runs on has.
    if (DIR* users = ::opendir("/run/user")) {
        while (dirent* user = ::readdir(users)) {
            if (user->d_name[0] < '0' || user->d_name[0] > '9') continue;
            const std::string dir = std::string("/run/user/") + user->d_name;
            DIR* entries = ::opendir(dir.c_str());
            if (!entries) continue;
            WlProxy* found = nullptr;
            while (dirent* e = ::readdir(entries)) {
                const std::string file = e->d_name;
                if (file.rfind("wayland-", 0) != 0) continue;
                if (file.size() >= 5 && file.compare(file.size() - 5, 5, ".lock") == 0) continue;
                const std::string path = dir + "/" + file;
                found = lib.displayConnect(path.c_str());
                if (found) {
                    socketUsed = path;
                    break;
                }
            }
            ::closedir(entries);
            if (found) {
                ::closedir(users);
                return found;
            }
        }
        ::closedir(users);
    }

    if (why.empty())
        why = "no Wayland socket (WAYLAND_DISPLAY unset, none answered under /run/user)";
    return nullptr;
}

} // namespace

bool WaylandLayout::read(std::vector<WaylandOutput>& outputs, std::string& socketUsed,
                         std::string& why)
{
    outputs.clear();
    socketUsed.clear();
    why.clear();

    Lib lib;
    if (!lib.open(why)) return false;

    WlProxy* display = connectAny(lib, socketUsed, why);
    if (!display) return false;

    Session session;
    session.lib = &lib;

    bool ok = false;
    WlProxy* registry =
        lib.marshalConstructor(display, kDisplayGetRegistry, lib.registryInterface, nullptr);
    if (registry) {
        lib.addListener(
            registry,
            reinterpret_cast<void (**)(void)>(const_cast<RegistryListener*>(&kRegistryListener)),
            &session);
        // First round trip: the globals, and with them the binds above. Second:
        // every output's geometry and mode, and the logical rectangles asked
        // for in between.
        if (lib.displayRoundtrip(display) >= 0) {
            if (session.manager) {
                for (auto& out : session.outputs) {
                    out->xdg = lib.marshalConstructor(session.manager, kXdgManagerGetXdgOutput,
                                                      &kXdgOutputInterface, nullptr, out->output);
                    if (out->xdg)
                        lib.addListener(out->xdg,
                                        reinterpret_cast<void (**)(void)>(
                                            const_cast<XdgOutputListener*>(&kXdgOutputListener)),
                                        out.get());
                }
            }
            ok = lib.displayRoundtrip(display) >= 0;
            if (!ok) why = "the compositor dropped the connection";
        } else {
            why = "the compositor dropped the connection";
        }
    } else {
        why = "could not reach the registry";
    }

    if (ok) {
        for (auto& out : session.outputs) {
            WaylandOutput o;
            o.name = out->name;
            o.transform = out->transform >= 0 && out->transform <= 7
                              ? static_cast<uint32_t>(out->transform)
                              : 0;
            if (out->haveLogical) {
                o.x = out->logicalX;
                o.y = out->logicalY;
                o.width = out->logicalW;
                o.height = out->logicalH;
            } else {
                // No xdg-output: the plain protocol's position is already in
                // the compositor's space, and the size follows from the mode
                // and the integer scale. A fractional scale has no expression
                // here, and comes out as the next integer's — off by the
                // difference, which is what xdg-output exists to fix. The mode
                // is the panel's: a quarter turn stands it the desktop's way
                // (odd transforms, 90 and 270, flipped or not).
                o.x = out->geomX;
                o.y = out->geomY;
                o.width = out->modeW / (out->scale > 0 ? out->scale : 1);
                o.height = out->modeH / (out->scale > 0 ? out->scale : 1);
                if (o.transform % 2 == 1) std::swap(o.width, o.height);
            }
            outputs.push_back(std::move(o));
        }
        if (outputs.empty()) {
            ok = false;
            why = "the compositor lists no output";
        } else if (!session.manager) {
            log::info("[native] input: the compositor has no xdg-output — the layout is read "
                      "from wl_output, exact for integer scales only");
        }
    }

    // Tear down in protocol order: every object told it is going, then the
    // proxies, then the socket. Nothing here outlives the call.
    for (auto& out : session.outputs) {
        if (out->xdg) {
            lib.marshal(out->xdg, kXdgOutputDestroy);
            lib.proxyDestroy(out->xdg);
        }
        if (out->output) {
            if (out->outputVersion >= 3) lib.marshal(out->output, kOutputRelease);
            lib.proxyDestroy(out->output);
        }
    }
    if (session.manager) {
        lib.marshal(session.manager, kXdgManagerDestroy);
        lib.proxyDestroy(session.manager);
    }
    if (registry) lib.proxyDestroy(registry);
    lib.displayDisconnect(display);
    return ok;
}

} // namespace mw::native::input
