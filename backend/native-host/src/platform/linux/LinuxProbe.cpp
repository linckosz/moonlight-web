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

#include "../../capture/linux/KmsCapture.h"
#if defined(MW_NATIVE_LINUX_PORTAL)
#include "../../capture/linux/GamescopeSession.h"
#include "../../capture/linux/KwinVirtualOutput.h"
#include "../../capture/linux/MutterScreenCast.h"
#include "../../capture/linux/PortalScreenCast.h"
#endif
#include "../../core/KmsVerdictCache.h"
#include "../../core/LinuxRouteChoice.h"
#include "../../core/Log.h"
#include "../../core/Probe.h"
#include "../../encode/OpenH264Encoder.h"
#if defined(MW_NATIVE_LINUX_VULKAN)
#include "../../encode/linux/Dav1dDecoder.h"
#include "vulkan/VulkanDevice.h"
#endif
#include "../../input/linux/UinputGamepad.h"

#include <fcntl.h>
#include <glob.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <xf86drm.h>

#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

// The Linux platform probe: GPUs from /dev/dri, displays from KMS, encoders from
// VA-API. The same three answers WindowsProbe gives, from three other places.
//
// ── What "interactive" means here ───────────────────────────────────────────
//
// Nothing. KMS captures the scanout buffer whether or not a display server is
// running and whoever is logged in — that is the point of choosing it (see
// KmsCapture.h). So the question Windows has to ask ("can this process reach a
// desktop?") becomes "is anything being shown at all?", which is a CRTC with a
// framebuffer. A machine whose screen is off answers no and is reported as
// such rather than as a capture failure.
//
// One exception: a Wayland session this process can reach, on a build with the
// portal's route. A machine with no monitor at all still runs one — autologin
// on a mini PC under the TV's shelf — and its compositor can make the virtual
// display for a stream (enumerate() lists it then, or says why it cannot).

namespace mw::native::platform {
namespace {

/// Every /dev/dri/cardN, in order. Render nodes are not cards.
std::vector<std::string> cardPaths()
{
    std::vector<std::string> paths;
    glob_t found = {};
    if (glob("/dev/dri/card*", 0, nullptr, &found) == 0) {
        for (size_t i = 0; i < found.gl_pathc; ++i)
            paths.emplace_back(found.gl_pathv[i]);
    }
    globfree(&found);
    return paths;
}

/// What names a card across boots: its bus address ("0000:03:00.0"), which the
/// minor number is not when two GPUs race to register. Falls back to the path.
std::string cardStableId(const std::string& cardPath)
{
    const std::string node = cardPath.substr(cardPath.rfind('/') + 1);
    char resolved[PATH_MAX];
    if (::realpath(("/sys/class/drm/" + node + "/device").c_str(), resolved)) {
        const std::string device(resolved);
        return device.substr(device.rfind('/') + 1);
    }
    return cardPath;
}

/// An X11 session, as the session says: XDG_SESSION_TYPE, or — when it names
/// neither (unset; "tty" for a process started over ssh) — a DISPLAY with no
/// WAYLAND_DISPLAY beside it.
[[maybe_unused]] bool isX11Session()
{
    const char* type = std::getenv("XDG_SESSION_TYPE");
    if (type && std::strcmp(type, "wayland") == 0) return false;
    if (type && std::strcmp(type, "x11") == 0) return true;
    const char* display = std::getenv("DISPLAY");
    const char* wayland = std::getenv("WAYLAND_DISPLAY");
    return display && *display && !(wayland && *wayland);
}

/// Whether the machine has a battery of its own. A mouse or a UPS reports one
/// too, with a "Device" scope; a laptop's has none or "System".
bool hasSystemBattery()
{
    bool found = false;
    glob_t supplies = {};
    if (glob("/sys/class/power_supply/*/type", 0, nullptr, &supplies) == 0) {
        for (size_t i = 0; i < supplies.gl_pathc && !found; ++i) {
            const std::string typePath(supplies.gl_pathv[i]);
            const std::string dir = typePath.substr(0, typePath.rfind('/'));
            auto read = [](const std::string& path) {
                std::string text;
                if (FILE* f = std::fopen(path.c_str(), "r")) {
                    char buf[64] = {};
                    if (std::fgets(buf, sizeof(buf), f)) text = buf;
                    std::fclose(f);
                }
                while (!text.empty() && (text.back() == '\n' || text.back() == ' '))
                    text.pop_back();
                return text;
            };
            if (read(typePath) != "Battery") continue;
            const std::string scope = read(dir + "/scope");
            found = scope.empty() || scope == "System";
        }
    }
    globfree(&supplies);
    return found;
}

/// The card's minor number — /dev/dri/card1 → 1. Stable within a boot, which
/// is all GpuInfo::nativeHandle promises.
uint64_t cardHandle(const std::string& cardPath)
{
    const size_t digits = cardPath.find_first_of("0123456789", cardPath.rfind("card"));
    return digits == std::string::npos ? 0 : std::strtoull(cardPath.c_str() + digits, nullptr, 10);
}

/// PCI vendor/device of the card, and the driver's name, from libdrm.
void identifyCard(const std::string& cardPath, GpuInfo& gpu)
{
    const int fd = ::open(cardPath.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    drmDevicePtr device = nullptr;
    if (drmGetDevice2(fd, 0, &device) == 0 && device) {
        if (device->bustype == DRM_BUS_PCI && device->deviceinfo.pci) {
            gpu.vendorId = device->deviceinfo.pci->vendor_id;
            gpu.deviceId = device->deviceinfo.pci->device_id;
        }
        drmFreeDevice(&device);
    }
    drmVersionPtr version = drmGetVersion(fd);
    if (version) {
        // "amdgpu", "i915", "nouveau" — the fallback name when VA-API has no
        // friendlier one to offer.
        gpu.name = version->name ? version->name : "GPU";
        drmFreeVersion(version);
    }
    ::close(fd);
}

/// Ask VA-API what this card encodes, on its render node. Fills the encoder,
/// codec and name fields; leaves them empty when the driver has nothing.
void probeEncoders(const std::string& renderNode, GpuInfo& gpu)
{
    const int fd = ::open(renderNode.c_str(), O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    VADisplay display = vaGetDisplayDRM(fd);
    int major = 0, minor = 0;
    if (!display || vaInitialize(display, &major, &minor) != VA_STATUS_SUCCESS) {
        ::close(fd);
        return;
    }

    // "Mesa Gallium driver 23.2.1 for AMD Radeon Graphics (gfx1103_r1, LLVM
    // 15.0.7, ...)" — the part after "for", up to the LLVM version, is the
    // name a user would recognise.
    if (const char* vendor = vaQueryVendorString(display)) {
        std::string text(vendor);
        const size_t forPos = text.find(" for ");
        if (forPos != std::string::npos) {
            std::string name = text.substr(forPos + 5);
            const size_t llvm = name.find(", LLVM");
            if (llvm != std::string::npos) name = name.substr(0, llvm) + ")";
            if (!name.empty()) gpu.name = name;
        }
    }

    const int count = vaMaxNumProfiles(display);
    std::vector<VAProfile> profiles(static_cast<size_t>(count));
    int have = 0;
    if (vaQueryConfigProfiles(display, profiles.data(), &have) == VA_STATUS_SUCCESS) {
        const auto encodes = [&](VAProfile wanted) {
            for (int i = 0; i < have; ++i) {
                if (profiles[static_cast<size_t>(i)] != wanted) continue;
                const int n = vaMaxNumEntrypoints(display);
                std::vector<VAEntrypoint> entrypoints(static_cast<size_t>(n));
                int got = 0;
                if (vaQueryConfigEntrypoints(display, wanted, entrypoints.data(), &got) !=
                    VA_STATUS_SUCCESS)
                    return false;
                for (int k = 0; k < got; ++k)
                    if (entrypoints[static_cast<size_t>(k)] == VAEntrypointEncSlice) return true;
            }
            return false;
        };
        // Best first, as the Selector expects. AV1 is the exception, and the
        // note below the list says why.
        const bool h264 = encodes(VAProfileH264High) || encodes(VAProfileH264Main) ||
                          encodes(VAProfileH264ConstrainedBaseline);
        const bool hevc = encodes(VAProfileHEVCMain);
        const bool av1 = encodes(VAProfileAV1Profile0);
        if (hevc || h264) gpu.encoders.push_back(EncoderApi::VaApi);
        if (hevc) gpu.codecs.push_back(Codec::Hevc);
        if (h264) gpu.codecs.push_back(Codec::H264);
        std::string list;
        for (Codec c : gpu.codecs)
            list += (list.empty() ? "" : ", ") + std::string(toString(c));

        // ⚠️ AV1 is NOT claimed, and the log says which of the two reasons.
        //
        // A profile with an encode entrypoint is not an encoder that can be
        // configured: the AV1 encoder is described by attributes of its own
        // (VAConfigAttribEncAV1 and its two extensions), and Mesa 23.2.1 on
        // gfx1103 answers "not supported" to them while advertising the
        // profile. Measured 08/09: our own encoder gets as far as
        // vaEndPicture, and FFmpeg 7.1.1 — a complete AV1 VA-API
        // implementation, OBU writer and all — fails on the same driver with
        // "Attribute type:52 is not supported". So the gap is the driver's,
        // and claiming the codec would be bug B7 (design §19.13).
        std::string av1Note;
        if (av1) {
            VAConfigAttrib av1Attribs[1] = {};
            av1Attribs[0].type = VAConfigAttribEncAV1;
            const bool described =
                vaGetConfigAttributes(display, VAProfileAV1Profile0, VAEntrypointEncSlice,
                                      av1Attribs, 1) == VA_STATUS_SUCCESS &&
                av1Attribs[0].value != VA_ATTRIB_NOT_SUPPORTED;
            av1Note = described ? ", AV1 (silicon and attributes, never driven here)"
                                : ", AV1 (profile advertised, but this driver has no AV1 encode "
                                  "attributes — unusable)";
        }
        log::info("[native] " + gpu.name + ": VA-API " + (list.empty() ? "no encoder" : list) +
                  av1Note);
    }
    vaTerminate(display);
    ::close(fd);
}

} // namespace

bool hasInteractiveSession()
{
    // See the file comment: not a session at all, a display being shown.
    for (const std::string& card : cardPaths()) {
        std::string error;
        for (const capture::KmsOutput& out : capture::KmsCapture::listOutputs(card, error))
            if (out.active) return true;
    }
#if defined(MW_NATIVE_LINUX_PORTAL)
    // ...or, with nothing shown, a Wayland session to make a display in. Before
    // this, a machine with no monitor was "no interactive desktop session", and
    // its native host card was gone — measured on the GNOME 48 VM with its only
    // connector off (video=Virtual-1:d), gnome-shell running headless, the
    // portal offering VIRTUAL (01/10/2026).
    if (!isX11Session()) {
        const char* wayland = std::getenv("WAYLAND_DISPLAY");
        if (wayland && *wayland) {
            std::string socket = wayland;
            if (socket.front() != '/') {
                const char* runtime = std::getenv("XDG_RUNTIME_DIR");
                socket = std::string(runtime ? runtime : "") + "/" + socket;
            }
            struct stat st = {};
            if (::stat(socket.c_str(), &st) == 0 && S_ISSOCK(st.st_mode)) return true;
        }
    }
#endif
    return false;
}

bool isOsSupported()
{
    // GETFB2 — the ioctl that hands framebuffers over with their modifiers —
    // arrived in Linux 5.4. Anything older cannot capture a tiled buffer
    // correctly, and every desktop GPU tiles.
    utsname name = {};
    if (uname(&name) != 0) return false;
    int major = 0, minor = 0;
    if (std::sscanf(name.release, "%d.%d", &major, &minor) != 2) return false;
    if (major < 5 || (major == 5 && minor < 4)) return false;
    return ::access("/dev/dri", F_OK) == 0;
}

Unavailability enumerate(Capabilities& caps)
{
    caps.gpus.clear();
    caps.displays.clear();

    const std::vector<std::string> cards = cardPaths();
    if (cards.empty()) {
        caps.diagnostic = "no DRM device under /dev/dri";
        return Unavailability::NoCaptureApi;
    }

    int nextGpuId = 0;
    int nextDisplayId = 0;
    bool anyPrimary = false;
    for (const std::string& card : cards) {
        GpuInfo gpu;
        gpu.id = nextGpuId;
        gpu.nativeHandle = cardHandle(card);
        identifyCard(card, gpu);

        // The render node is where EGL and VA-API live; a card without one is
        // a display-only device (a USB adapter, a virtual console) and encodes
        // nothing.
        std::string renderNode;
        {
            const int fd = ::open(card.c_str(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                char* node = drmGetRenderDeviceNameFromFd(fd);
                if (node) {
                    renderNode = node;
                    std::free(node);
                }
                ::close(fd);
            }
        }
        if (!renderNode.empty()) probeEncoders(renderNode, gpu);

        std::string error;
        int indexOnCard = 0;
        for (const capture::KmsOutput& out : capture::KmsCapture::listOutputs(card, error)) {
            if (!out.connected) continue;
            // Connected, but with nothing KMS shows on it: no card for it — a
            // launch could only fail ("the display is disconnected", seen on
            // the UM790Pro's GTX 1050 under X11, 30/09/2026). LinuxSession
            // counts displays by this same rule, so the index it is handed
            // still names the card that was picked.
            if (!out.active) {
                log::info("[native] " + out.name + " on " + card +
                          " is connected but KMS shows nothing on it — not offered: " +
                          (gpu.vendorId == 0x10DE
                               ? "NVIDIA's own X driver sets its modes without KMS, and its "
                                 "picture cannot be captured here"
                               : "a screen switched off in the display settings, or one being "
                                 "reconfigured"));
                continue;
            }
            DisplayInfo display;
            display.id = nextDisplayId++;
            display.gpuId = gpu.id;
            display.width = out.width;
            display.height = out.height;
            display.refreshMilliHz = out.refreshMilliHz;
            // No HDR on this path yet: the capture reads 8-bit XRGB and the
            // converter has no PQ shader here. Said here so the Selector never
            // grants it.
            display.hdrActive = false;
            // KMS has no notion of a primary; the one at the desktop's origin
            // is what every compositor treats as such.
            display.primary = !anyPrimary && out.x == 0 && out.y == 0;
            if (display.primary) anyPrimary = true;
            display.label = "Display " + std::to_string(display.id + 1);
            display.detail = out.name + " \xE2\x80\x94 " + std::to_string(out.width) + "\xC3\x97" +
                             std::to_string(out.height) + " \xC2\xB7 " +
                             std::to_string((out.refreshMilliHz + 500) / 1000) + " Hz";
            display.kind = classifyConnectorName(out.name);
            display.key = cardStableId(card) + "/" + out.name;
            caps.displays.push_back(display);
            ++indexOnCard;
        }
        if (!error.empty()) log::warning("[native] " + error);

        caps.gpus.push_back(gpu);
        ++nextGpuId;
    }

#if defined(MW_NATIVE_LINUX_PORTAL)
    // The portal's VIRTUAL source: a monitor the compositor creates for a
    // stream, at the client's size, and removes after it (GNOME 46+ says so
    // in AvailableSourceTypes; GNOME 42 and KDE 5 do not). Listed whatever
    // route the real monitors take — it has only the one — and even with no
    // monitor at all, which is the headless machine it exists for.
    //
    // Not under X11: GNOME's portal says VIRTUAL there too (7 on the UM790Pro,
    // 30/09/2026), but only its Wayland compositor can make such a monitor —
    // under X11 the card would lead to a session that fails.
    const uint32_t sourceTypes = capture::PortalScreenCast::sourceTypes();
    bool portalVirtual = (sourceTypes & capture::PortalScreenCast::kSourceVirtual) != 0;
    if (portalVirtual && isX11Session()) {
        portalVirtual = false;
        log::info("[native] the portal offers a virtual display, but this is an X11 session, "
                  "where GNOME cannot make one — not offered");
    }
    // GNOME's own screen cast (MutterScreenCast.h, plan Idées Punktfunk C2),
    // which the session asks before the portal: no dialog, and GNOME 42 to 45
    // too, whose portal makes no virtual monitor. Wayland only, as above.
    int mutterVersion = 0;
    if (!isX11Session()) {
        std::string why;
        mutterVersion = capture::MutterScreenCast::version(why);
    }
    const bool mutterVirtual = mutterVersion >= capture::MutterScreenCast::kVirtualVersion;
    portalVirtual = portalVirtual || mutterVirtual;
    // KDE Plasma 6: its portal offers no VIRTUAL source (6.3: types 3), KWin
    // makes the monitor itself (KwinVirtualOutput.h). Offered on KWin's word
    // alone: whether it trusts the program is asked by the session, from the
    // helper without capabilities — this process, the server's, may hold one.
    bool kwinVirtual = false;
    std::string kwinWhy;
    if (!portalVirtual && sourceTypes != 0 && !isX11Session()) {
        bool kwin = false;
        bool granted = false;
        if (capture::KwinVirtualOutput::probe(kwin, granted, kwinWhy) && kwin) {
            kwinVirtual = true;
            kwinWhy = granted ? "KWin grants its screencast protocol"
                              : "KWin's grant is checked when the stream starts";
        }
    }
    portalVirtual = portalVirtual || kwinVirtual;

    // Steam's Big Picture in its own gamescope (GamescopeSession.h, plan Idées
    // Punktfunk, chapter G): a gamescope recent enough and a Steam someone
    // uses. No desktop needed — an X11 session, or none, has this card too.
    // The user's own apps (G5) need gamescope alone: one hidden entry for them
    // all, which the server turns into a card per app of its settings.
    std::string gamescopeWhy;
    std::string gamescopeDetail;
    std::string gamescopeVersion;
    bool gamescopeSteam = false;
    bool gamescopeApps = false;
    {
        capture::GamescopeBinary gamescope;
        if (capture::findGamescope(gamescope, gamescopeWhy)) {
            gamescopeApps = true;
            gamescopeVersion = gamescope.version.text();
            const std::vector<capture::SteamInstall> installs = capture::findSteamInstalls();
            int pid = 0;
            const int chosen = capture::chooseSteam(installs, capture::runningDesktopSteam(pid));
            if (chosen >= 0) {
                gamescopeSteam = true;
                gamescopeDetail = "Steam (" +
                                  std::string(capture::toString(
                                      installs[static_cast<size_t>(chosen)].packaging)) +
                                  ") in its own gamescope " + gamescope.version.text() +
                                  ", made at the client's size when the stream starts";
            } else {
                gamescopeWhy = installs.empty() ? "Steam is not installed"
                                                : "no Steam install was ever signed in to";
            }
        }
        if (!gamescopeSteam) log::debug("[native] no Steam Big Picture card: " + gamescopeWhy);
    }
#else
    const bool portalVirtual = false;
    const bool gamescopeSteam = false;
    const bool gamescopeApps = false;
#endif
    if (caps.displays.empty() && !portalVirtual && !gamescopeSteam && !gamescopeApps) {
        caps.diagnostic = "no display is connected";
        return Unavailability::NoDisplay;
    }
    if (!anyPrimary && !caps.displays.empty()) caps.displays.front().primary = true;
    caps.hasBattery = hasSystemBattery();

    // The privilege check, once, so the host list can say why rather than a
    // session failing at the click. Kept a few seconds per card: the status
    // route asks the probe at each request (KmsVerdictCache.h). A session's
    // creation clears them first and gets the answer of now.
    for (const std::string& card : cards) {
        const KmsVerdictCache::Verdict verdict = kmsVerdicts().get(card, [&card](std::string& why) {
            return capture::KmsCapture::canReadFramebuffers(card, why);
        });
        if (verdict.readable) {
            caps.capture = CaptureApi::Kms;
            break;
        }
        caps.diagnostic = verdict.why;
    }

#if defined(MW_NATIVE_LINUX_PORTAL)
    // No capability to read the scanout — the AppImage's situation, since FUSE
    // mounts it nosuid and no capability can travel with it (§19.8). The
    // ScreenCast portal asks the compositor instead, and needs nothing.
    //
    // ⚠️ The display list collapses to ONE entry here, and that is not a
    // simplification: the portal does not let the application choose a monitor,
    // the USER does, in the dialog it raises. Offering the three we enumerated
    // would be offering three buttons that all do the same thing and none of
    // them what they say. The size shown is the primary output's, as a
    // reasonable guess — the real one arrives when the compositor negotiates.
    if (caps.capture != CaptureApi::Kms && !caps.displays.empty()) {
        std::string reason;
        if (capture::PortalScreenCast::available(reason)) {
            const DisplayInfo* hint = nullptr;
            for (const DisplayInfo& d : caps.displays)
                if (d.primary) hint = &d;
            if (!hint && !caps.displays.empty()) hint = &caps.displays.front();

            DisplayInfo portal;
            portal.id = 0;
            portal.gpuId = hint ? hint->gpuId : (caps.gpus.empty() ? -1 : caps.gpus.front().id);
            portal.width = hint ? hint->width : 0;
            portal.height = hint ? hint->height : 0;
            portal.refreshMilliHz = hint ? hint->refreshMilliHz : 60000;
            portal.hdrActive = false;
            portal.primary = true;
            portal.label = "Screen";
            // Which screen it will be is the user's choice, made later: the
            // card cannot say what it stands for, so it draws no machine.
            portal.kind = DisplayKind::Unknown;
            portal.key = "portal";
            portal.detail = "chosen when the stream starts \xE2\x80\x94 " + reason;
            caps.displays.clear();
            caps.displays.push_back(portal);
            caps.capture = CaptureApi::PipeWire;
            log::info("[native] no capability to read the scanout (" + caps.diagnostic +
                      ") \xE2\x80\x94 falling back to the ScreenCast portal, which asks the user "
                      "once");
            caps.diagnostic.clear();
        }
    }

    if (portalVirtual) {
        // Encoded where an encoder is: the compositor renders it on whichever
        // GPU it likes, and the portal hands it over like any of its streams.
        const GpuInfo* gpu = nullptr;
        for (const GpuInfo& g : caps.gpus)
            if (!gpu && !g.encoders.empty() && !g.codecs.empty()) gpu = &g;
        if (!gpu && !caps.gpus.empty()) gpu = &caps.gpus.front();
        int nextId = 0;
        for (const DisplayInfo& d : caps.displays)
            nextId = std::max(nextId, d.id + 1);
        DisplayInfo virt;
        virt.id = nextId;
        virt.gpuId = gpu ? gpu->id : -1;
        // Nominal: the real size is the client's, set when the stream starts.
        virt.width = 1920;
        virt.height = 1080;
        virt.refreshMilliHz = 60000;
        virt.primary = caps.displays.empty();
        virt.label = "Virtual display";
        virt.model = "Virtual display";
        virt.kind = DisplayKind::Virtual;
        virt.key = kPortalVirtualDisplayKey;
        virt.capture = CaptureApi::PipeWire;
        virt.detail = mutterVirtual ? "created by GNOME at the client's size when the stream "
                                      "starts, and shared by the streams on it (Mutter's screen "
                                      "cast, version " +
                                          std::to_string(mutterVersion) + ")"
                      : kwinVirtual ? "created by KWin at the client's size when the stream "
                                      "starts (" +
                                          kwinWhy + ")"
                                    : "created by the ScreenCast portal at the client's size "
                                      "when the stream starts (source types " +
                                          std::to_string(sourceTypes) + ")";
        caps.displays.push_back(virt);
        // Nothing else to capture: this display's route is the machine's.
        if (caps.capture != CaptureApi::Kms && caps.capture != CaptureApi::PipeWire &&
            caps.displays.size() == 1) {
            caps.capture = CaptureApi::PipeWire;
            caps.diagnostic.clear();
        }
    }

    if (gamescopeSteam) {
        // Encoded where an encoder is, like the virtual display: gamescope
        // renders on the GPU Vulkan picks, and its node hands the picture over.
        const GpuInfo* gpu = nullptr;
        for (const GpuInfo& g : caps.gpus)
            if (!gpu && !g.encoders.empty() && !g.codecs.empty()) gpu = &g;
        if (!gpu && !caps.gpus.empty()) gpu = &caps.gpus.front();
        int nextId = 0;
        for (const DisplayInfo& d : caps.displays)
            nextId = std::max(nextId, d.id + 1);
        DisplayInfo steam;
        steam.id = nextId;
        steam.gpuId = gpu ? gpu->id : -1;
        // Nominal, as the virtual display's: the size is the client's.
        steam.width = 1920;
        steam.height = 1080;
        steam.refreshMilliHz = 60000;
        steam.primary = caps.displays.empty();
        steam.label = "Steam Big Picture";
        steam.model = "Steam Big Picture";
        steam.kind = DisplayKind::Virtual;
        steam.key = kGamescopeSteamDisplayKey;
        steam.capture = CaptureApi::PipeWire;
        steam.detail = gamescopeDetail;
        caps.displays.push_back(steam);
        if (caps.capture != CaptureApi::Kms && caps.capture != CaptureApi::PipeWire &&
            caps.displays.size() == 1) {
            caps.capture = CaptureApi::PipeWire;
            caps.diagnostic.clear();
        }
    }

    if (gamescopeApps) {
        const GpuInfo* gpu = nullptr;
        for (const GpuInfo& g : caps.gpus)
            if (!gpu && !g.encoders.empty() && !g.codecs.empty()) gpu = &g;
        if (!gpu && !caps.gpus.empty()) gpu = &caps.gpus.front();
        int nextId = 0;
        for (const DisplayInfo& d : caps.displays)
            nextId = std::max(nextId, d.id + 1);
        DisplayInfo apps;
        apps.id = nextId;
        apps.gpuId = gpu ? gpu->id : -1;
        apps.width = 1920;
        apps.height = 1080;
        apps.refreshMilliHz = 60000;
        apps.label = "Apps in gamescope";
        apps.kind = DisplayKind::Virtual;
        apps.key = kGamescopeAppDisplayKey;
        apps.capture = CaptureApi::PipeWire;
        apps.detail = "the user's own apps, each in its own gamescope " + gamescopeVersion +
                      " at the client's size (a card per app of the settings)";
        caps.displays.push_back(apps);
        if (caps.capture != CaptureApi::Kms && caps.capture != CaptureApi::PipeWire &&
            caps.displays.size() == 1) {
            caps.capture = CaptureApi::PipeWire;
            caps.diagnostic.clear();
        }
    }
#endif

    if (caps.capture != CaptureApi::Kms && caps.capture != CaptureApi::PipeWire)
        return Unavailability::NoCaptureApi;

    for (const DisplayInfo& display : caps.displays) {
        const GpuInfo* gpu = caps.gpuFor(display);
        log::info("[native] " + display.label + " \xC2\xB7 " + display.detail + " on " +
                  (gpu ? gpu->name : "unknown GPU") + (display.primary ? " [primary]" : ""));
    }
    return Unavailability::None;
}

void probeFallbackEncoders(Capabilities& caps)
{
    // Linux has exactly one fallback and it is OpenH264 on the CPU: there is no
    // OS-level encoder API here the way Media Foundation is one on Windows —
    // VA-API *is* the vendor path, and reaching this function means it answered
    // nothing.
    //
    // The machine this is for (bench-vm, hyperv_drm) has no render node at
    // all, so it lacks more than an encoder: EGL has no device either. The
    // session then reads the scanout buffer through a DMA-BUF mmap and converts
    // on the CPU (CpuConvert) — which needs the buffer to be linear, a fact only
    // the first frame can prove. Offered here regardless: the alternative is to
    // refuse the machine outright, and a tiled buffer fails the session with a
    // sentence that says so.
    FallbackEncoder cpu;
    cpu.api = EncoderApi::Software;
    cpu.codecs = {Codec::H264};
    cpu.hardware = false;
    cpu.name = encode::OpenH264Encoder::version();
    caps.fallbacks.push_back(std::move(cpu));
}

std::vector<Codec> offerSessionCodecs(Capabilities& caps, const SessionConfig& config)
{
    // AV1, the one codec only Vulkan Video encodes here (C13.12): added to a
    // GPU that encodes through VA-API already — the chain falls back to it for
    // everything else — only for a session whose bench key, setting or vendor
    // table (AMD's since 05/10/2026) asks for the Vulkan Video chain on that
    // GPU, and only where the GPU's Vulkan driver shows an AV1 encoder and
    // this build can prove one at the pixel.
    // A session that would have encoded through VA-API is never handed AV1;
    // the proof itself runs in the session, which falls back to the client's
    // next codec if it does not pass (LinuxSession::buildPipeline).
    std::vector<Codec> added;
#if defined(MW_NATIVE_LINUX_VULKAN)
    if (!encode::Dav1dDecoder::built()) return added;
    for (GpuInfo& gpu : caps.gpus) {
        if (std::find(gpu.encoders.begin(), gpu.encoders.end(), EncoderApi::VaApi) ==
                gpu.encoders.end() ||
            std::find(gpu.codecs.begin(), gpu.codecs.end(), Codec::Av1) != gpu.codecs.end())
            continue;
        if (linuxWantedPipeline(config.tuning.pipeline, config.videoPipeline, gpu.vendorId) !=
            VideoPipeline::Vulkan)
            continue;
        std::string renderNode;
        for (const std::string& card : cardPaths()) {
            if (cardHandle(card) != gpu.nativeHandle) continue;
            const int fd = ::open(card.c_str(), O_RDWR | O_CLOEXEC);
            if (fd < 0) break;
            if (char* node = drmGetRenderDeviceNameFromFd(fd)) {
                renderNode = node;
                std::free(node);
            }
            ::close(fd);
            break;
        }
        vulkan::DeviceIdentity id;
        std::string error;
        if (renderNode.empty() || !vulkan::VulkanDevice::identify(renderNode, id, error) ||
            !id.encodesAv1)
            continue;
        gpu.codecs.push_back(Codec::Av1);
        if (added.empty()) added.push_back(Codec::Av1);
        log::info("[native] " + gpu.name +
                  ": AV1 offered to this session, through Vulkan Video (the chain it asks for), "
                  "once the pixel proof passes");
    }
#else
    (void)caps;
    (void)config;
#endif
    return added;
}

VirtualGamepad probeVirtualGamepad()
{
    VirtualGamepad result;
    result.supported = true;
    result.present = input::UinputGamepad::devicePresent(result.diagnostic);
    return result;
}

} // namespace mw::native::platform
