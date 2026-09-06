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

#include "MacDisplays.h"

#include "../../core/Log.h"
#include "../../core/Probe.h"

#import <AppKit/AppKit.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include <CoreGraphics/CoreGraphics.h>
#include <Security/AuthSession.h>
#include <VideoToolbox/VideoToolbox.h>

#include <cmath>
#include <string>
#include <vector>

// The macOS platform probe: displays from CoreGraphics (named by AppKit), the
// GPU behind each from Metal, encoders from VideoToolbox's own list. The same
// three answers WindowsProbe and LinuxProbe give, from three other places.
//
// ── What "interactive" means here ───────────────────────────────────────────
//
// A process on the console session: the one whose user is sitting at the Mac.
// ScreenCaptureKit and CGEventPost both work only there — a launchd daemon, or
// a shell arrived over SSH, is in another audit session and sees no windows —
// so the question is asked first and answered plainly, the way session 0 is on
// Windows. MoonlightWeb runs as a LaunchAgent (com.moonlightweb.agent), which
// IS the console session; a test binary started from SSH is not, and says so.
//
// ── Permission ──────────────────────────────────────────────────────────────
//
// Screen Recording is the one thing the OS will not let the program grant
// itself. The probe asks the system to show its prompt once per process and
// reports CapturePermission until the switch is on — which takes a restart of
// the program, as macOS applies the grant only to processes started after it.

namespace mw::native::platform {

std::vector<MacDisplay> listDisplays()
{
    std::vector<MacDisplay> out;
    CGDirectDisplayID ids[32];
    uint32_t count = 0;
    // ONLINE, not active: a display whose panel has gone to sleep drops out
    // of the active list, and a Mac left alone for ten minutes would vanish
    // from the host list (measured 05/09/2026: "no active display" with the
    // lid open and the screen dark). Online is "attached and usable"; the
    // session wakes the panel when it starts (MacSession).
    if (CGGetOnlineDisplayList(32, ids, &count) != kCGErrorSuccess) return out;

    @autoreleasepool {
        NSArray<NSScreen*>* screens = [NSScreen screens];
        for (uint32_t i = 0; i < count; ++i) {
            MacDisplay d;
            d.displayId = ids[i];
            d.isMain = CGDisplayIsMain(ids[i]);
            d.isAsleep = CGDisplayIsAsleep(ids[i]);
            const CGRect bounds = CGDisplayBounds(ids[i]);
            d.left = static_cast<int>(bounds.origin.x);
            d.top = static_cast<int>(bounds.origin.y);
            d.right = static_cast<int>(bounds.origin.x + bounds.size.width);
            d.bottom = static_cast<int>(bounds.origin.y + bounds.size.height);

            double refresh = 0;
            if (CGDisplayModeRef mode = CGDisplayCopyDisplayMode(ids[i])) {
                d.pixelWidth = static_cast<int>(CGDisplayModeGetPixelWidth(mode));
                d.pixelHeight = static_cast<int>(CGDisplayModeGetPixelHeight(mode));
                // Zero for the built-in panel — it is asked of AppKit below.
                refresh = CGDisplayModeGetRefreshRate(mode);
                CGDisplayModeRelease(mode);
            }
            for (NSScreen* screen in screens) {
                NSNumber* number = screen.deviceDescription[@"NSScreenNumber"];
                if (!number || number.unsignedIntValue != ids[i]) continue;
                if (const char* name = screen.localizedName.UTF8String) d.name = name;
                if (refresh <= 0) refresh = screen.maximumFramesPerSecond;
                // The POTENTIAL headroom: what the panel can do, not what it
                // is showing this instant (the current value sits at 1.0 on a
                // desktop with no HDR content and climbs when some appears).
                d.edrHeadroom = screen.maximumPotentialExtendedDynamicRangeColorComponentValue;
                d.hdr = d.edrHeadroom > 1.0;
                break;
            }
            if (refresh <= 0) refresh = 60;
            d.refreshMilliHz = static_cast<int>(std::llround(refresh * 1000.0));
            if (d.name.empty()) d.name = "Display";

            // The GPU that scans this display out, by Metal's account. On an
            // Apple Silicon Mac there is one; on an Intel Mac with a discrete
            // card the internal panel and an external monitor can differ.
            if (id<MTLDevice> device = CGDirectDisplayCopyCurrentMetalDevice(ids[i])) {
                d.gpuRegistryId = device.registryID;
                if (const char* name = device.name.UTF8String) d.gpuName = name;
            }
            out.push_back(d);
        }
    }
    return out;
}

namespace {

/// What VideoToolbox's hardware encoders produce, machine-wide.
void probeEncoders(bool& h264, bool& hevc, std::string& detail)
{
    h264 = false;
    hevc = false;
    CFArrayRef list = nullptr;
    if (VTCopyVideoEncoderList(nullptr, &list) != noErr || !list) {
        detail = "VideoToolbox listed no encoders";
        return;
    }
    const CFIndex count = CFArrayGetCount(list);
    for (CFIndex i = 0; i < count; ++i) {
        auto entry = static_cast<CFDictionaryRef>(CFArrayGetValueAtIndex(list, i));
        auto codecRef =
            static_cast<CFNumberRef>(CFDictionaryGetValue(entry, kVTVideoEncoderList_CodecType));
        int32_t codec = 0;
        if (!codecRef || !CFNumberGetValue(codecRef, kCFNumberSInt32Type, &codec)) continue;

        bool hardware = false;
        if (@available(macOS 13.0, *)) {
            auto flag = static_cast<CFBooleanRef>(
                CFDictionaryGetValue(entry, kVTVideoEncoderList_IsHardwareAccelerated));
            hardware = flag && CFBooleanGetValue(flag);
        } else {
            // Before the flag existed, the id said it: Apple's own hardware
            // block is "…ave…", Intel's Quick Sync on older Macs "…gva…".
            auto id = static_cast<CFStringRef>(
                CFDictionaryGetValue(entry, kVTVideoEncoderList_EncoderID));
            if (id) {
                hardware = CFStringFind(id, CFSTR(".ave."), 0).location != kCFNotFound ||
                           CFStringFind(id, CFSTR("gva"), 0).location != kCFNotFound;
            }
        }
        if (!hardware) continue;
        if (codec == kCMVideoCodecType_H264) h264 = true;
        if (codec == kCMVideoCodecType_HEVC) hevc = true;
    }
    CFRelease(list);
    detail = std::string("VideoToolbox hardware: ") + (hevc ? "HEVC" : "no HEVC") + ", " +
             (h264 ? "H.264" : "no H.264");
}

} // namespace

bool hasInteractiveSession()
{
    // See the file comment: the console session, or nothing.
    //
    // Two questions, because one is not enough. The Quartz session dictionary
    // answers for the USER — a shell arrived over SSH as the person sitting at
    // the Mac reads "on console" too — while the security session answers for
    // the PROCESS: whether this audit session has graphic access, which the
    // SSH one does not. Measured 05/09/2026: over SSH the first said yes and
    // CGGetActiveDisplayList then found no display, which would have reported
    // a headless Mac instead of the truth.
    CFDictionaryRef session = CGSessionCopyCurrentDictionary();
    if (!session) return false;
    auto onConsole =
        static_cast<CFBooleanRef>(CFDictionaryGetValue(session, kCGSessionOnConsoleKey));
    const bool interactive = onConsole && CFBooleanGetValue(onConsole);
    CFRelease(session);
    if (!interactive) return false;

    // Deprecated since 10.7 with nothing put in its place that a plain
    // process may call; it still answers, and answers this exact question.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    SecuritySessionId id = 0;
    SessionAttributeBits attributes = 0;
    if (SessionGetInfo(callerSecuritySession, &id, &attributes) == errSessionSuccess)
        return (attributes & sessionHasGraphicAccess) != 0;
#pragma clang diagnostic pop
    return true;
}

bool isOsSupported()
{
    // ScreenCaptureKit arrived in macOS 12.3.
    const NSOperatingSystemVersion v = [NSProcessInfo processInfo].operatingSystemVersion;
    return v.majorVersion > 12 || (v.majorVersion == 12 && v.minorVersion >= 3);
}

Unavailability enumerate(Capabilities& caps)
{
    caps.gpus.clear();
    caps.displays.clear();

    const std::vector<MacDisplay> displays = listDisplays();
    if (displays.empty()) {
        caps.diagnostic = "no active display";
        return Unavailability::NoDisplay;
    }

    bool h264 = false;
    bool hevc = false;
    std::string encoderDetail;
    probeEncoders(h264, hevc, encoderDetail);

    // HDR needs the capture to hand over 10-bit PQ, which ScreenCaptureKit
    // does from macOS 15 (SCStreamConfiguration.captureDynamicRange). The
    // encoder side — HEVC Main10 — every Apple Silicon Mac has had since the
    // first; it is the capture that gates the platform.
    bool hdrCapture = false;
    if (@available(macOS 15.0, *)) hdrCapture = true;

    // One GpuInfo per Metal device, in the order the displays name them.
    for (const MacDisplay& d : displays) {
        int gpuId = -1;
        for (const GpuInfo& gpu : caps.gpus)
            if (gpu.nativeHandle == d.gpuRegistryId) gpuId = gpu.id;
        if (gpuId < 0) {
            GpuInfo gpu;
            gpu.id = static_cast<int>(caps.gpus.size());
            gpu.name = d.gpuName.empty() ? "Apple GPU" : d.gpuName;
            gpu.vendorId = 0x106B; // Apple
            gpu.nativeHandle = d.gpuRegistryId;
            // Best first, as the Selector expects (the Codec enum's order).
            if (hevc || h264) gpu.encoders.push_back(EncoderApi::VideoToolbox);
            if (hevc) gpu.codecs.push_back(Codec::Hevc);
            if (h264) gpu.codecs.push_back(Codec::H264);
            // 10-bit HEVC out of the Apple Video Encoder, fed by the 10-bit
            // capture: the HDR path of this platform (design §20.10). Claimed
            // only where the whole path exists — a capability says "this
            // pipeline can carry it", never "this chip could" (§16.3).
            gpu.supports10Bit = hevc && hdrCapture;
            caps.gpus.push_back(gpu);
            gpuId = gpu.id;
            log::info("[native] " + gpu.name + ": " + encoderDetail);
        }

        DisplayInfo display;
        display.id = static_cast<int>(caps.displays.size());
        display.gpuId = gpuId;
        display.width = d.pixelWidth;
        display.height = d.pixelHeight;
        display.refreshMilliHz = d.refreshMilliHz;
        // "In an HDR mode right now" has no switch to read on macOS: a panel
        // with EDR headroom is always in it. So this is the panel's headroom,
        // gated on a capture that can deliver 10-bit PQ (macOS 15).
        display.hdrActive = d.hdr && hdrCapture;
        display.primary = d.isMain;
        display.label = "Display " + std::to_string(display.id + 1);
        display.detail = d.name + " \xE2\x80\x94 " + std::to_string(d.pixelWidth) + "\xC3\x97" +
                         std::to_string(d.pixelHeight) + " \xC2\xB7 " +
                         std::to_string((d.refreshMilliHz + 500) / 1000) + " Hz" +
                         (d.hdr ? " \xC2\xB7 HDR (EDR \xC3\x97" +
                                      std::to_string(static_cast<int>(d.edrHeadroom + 0.5)) + ")"
                                : std::string()) +
                         (d.isAsleep ? " (asleep)" : "");
        caps.displays.push_back(display);
    }

    caps.capture = CaptureApi::ScreenCaptureKit;

    // The permission, checked once here so the host list can say why rather
    // than a session failing at the click. The OS prompt is requested once
    // per process; the grant applies to the NEXT start of the program.
    if (!CGPreflightScreenCaptureAccess()) {
        static bool asked = false;
        if (!asked) {
            asked = true;
            CGRequestScreenCaptureAccess();
        }
        caps.diagnostic = "Screen Recording is not granted to this program — allow it in System "
                          "Settings › Privacy & Security › Screen & System Audio Recording, then "
                          "restart MoonlightWeb";
        return Unavailability::CapturePermission;
    }

    for (const DisplayInfo& display : caps.displays) {
        const GpuInfo* gpu = caps.gpuFor(display);
        log::info("[native] " + display.label + " \xC2\xB7 " + display.detail + " on " +
                  (gpu ? gpu->name : "unknown GPU") + (display.primary ? " [primary]" : ""));
    }
    return Unavailability::None;
}

VirtualGamepad probeVirtualGamepad()
{
    // supported = false: presenting a gamepad to macOS takes a DriverKit
    // extension signed with an Apple entitlement (§8 of the plan, tranché le
    // 02/09/2026). Nothing a user installs would change that, so nothing is
    // offered.
    VirtualGamepad result;
    result.diagnostic = "macOS has no virtual gamepad without a signed DriverKit extension";
    return result;
}

} // namespace mw::native::platform
