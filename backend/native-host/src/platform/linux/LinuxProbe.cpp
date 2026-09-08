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
#include "../../core/Log.h"
#include "../../core/Probe.h"
#include "../../encode/OpenH264Encoder.h"
#include "../../input/linux/UinputGamepad.h"

#include <fcntl.h>
#include <glob.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <xf86drm.h>

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
// framebuffer. A machine whose screen is off, or headless, answers no and is
// reported as such rather than as a capture failure.

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
        // Best first, as the Selector expects. Only what VaapiEncoder has a
        // path for: AV1 is refused there until it has been driven, and a
        // capability the encoder does not honour is bug B7 — so it is noted for
        // the log and never claimed.
        const bool h264 = encodes(VAProfileH264High) || encodes(VAProfileH264Main) ||
                          encodes(VAProfileH264ConstrainedBaseline);
        const bool hevc = encodes(VAProfileHEVCMain);
        const bool av1 = encodes(VAProfileAV1Profile0);
        if (hevc || h264) gpu.encoders.push_back(EncoderApi::VaApi);
        if (hevc) gpu.codecs.push_back(Codec::Hevc);
        if (h264) gpu.codecs.push_back(Codec::H264);
        log::info("[native] " + gpu.name + ": VA-API " +
                  (hevc ? (h264 ? "HEVC, H.264" : "HEVC") : (h264 ? "H.264" : "no encoder")) +
                  (av1 ? ", AV1 (silicon, not yet driven)" : ""));
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
            display.primary = !anyPrimary && out.x == 0 && out.y == 0 && out.active;
            if (display.primary) anyPrimary = true;
            display.label = "Display " + std::to_string(display.id + 1);
            display.detail = out.name + " \xE2\x80\x94 " + std::to_string(out.width) + "\xC3\x97" +
                             std::to_string(out.height) + " \xC2\xB7 " +
                             std::to_string((out.refreshMilliHz + 500) / 1000) + " Hz" +
                             (out.active ? "" : " (off)");
            caps.displays.push_back(display);
            ++indexOnCard;
        }
        if (!error.empty()) log::warning("[native] " + error);

        caps.gpus.push_back(gpu);
        ++nextGpuId;
    }

    if (caps.displays.empty()) {
        caps.diagnostic = "no display is connected";
        return Unavailability::NoDisplay;
    }
    if (!anyPrimary) caps.displays.front().primary = true;

    // The privilege check, once, so the host list can say why rather than a
    // session failing at the click. Reported as a capture-API failure: KMS is
    // the API, and it is unusable without the capability.
    for (const std::string& card : cards) {
        std::string why;
        if (capture::KmsCapture::canReadFramebuffers(card, why)) {
            caps.capture = CaptureApi::Kms;
            break;
        }
        caps.diagnostic = why;
    }
    if (caps.capture != CaptureApi::Kms) return Unavailability::NoCaptureApi;

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

VirtualGamepad probeVirtualGamepad()
{
    VirtualGamepad result;
    result.supported = true;
    result.present = input::UinputGamepad::devicePresent(result.diagnostic);
    return result;
}

} // namespace mw::native::platform
