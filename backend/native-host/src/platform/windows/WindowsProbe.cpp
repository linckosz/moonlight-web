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

#include "../../core/Log.h"
#include "../../core/Probe.h"
#include "../../input/windows/VigemGamepad.h"
#include "WindowsEncoderProbe.h"

#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <cstdlib>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

// Windows capability detection.
//
// Answers three questions, in the order core/Probe.cpp asks them: is there a
// desktop to capture, is the OS recent enough, and what hardware is there.
//
// The third is where the value is: which GPU drives which display. Getting that
// association right is what decides whether the pipeline is zero-copy, because
// capturing on GPU A to encode on GPU B costs a VRAM→RAM→VRAM round trip that
// dwarfs everything else. DXGI answers it exactly — an output is enumerated by
// the adapter that scans it out — so there is no heuristic here.

using Microsoft::WRL::ComPtr;

namespace mw::native::platform {
namespace {

std::string narrow(const wchar_t* wide)
{
    if (!wide || !*wide) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), needed, nullptr, nullptr);
    return out;
}

/// A display's real mode: pixels and refresh, neither rounded nor scaled, plus
/// the monitor's own name when it has one.
struct DisplayMode
{
    int width = 0;
    int height = 0;
    int refreshMilliHz = 0;
    /// The name the monitor reports in its EDID — "M27Q", "LINDY32115_V3".
    /// Empty for a virtual display, which has no EDID to report one.
    std::string monitorName;
};

/// The true mode of each output, keyed by GDI device name ("\\\\.\\DISPLAY1").
///
/// Two things make this worth a second enumeration on top of DXGI's:
///
///  - **Size.** DXGI_OUTPUT_DESC::DesktopCoordinates is expressed in virtual
///    desktop coordinates, which Windows SCALES for a process that is not
///    per-monitor DPI aware. Measured here: a 2560×1440 panel at 125 % scaling
///    reports 2048×1152 — and MoonlightWeb must not declare itself DPI aware
///    from inside a library, because that setting is process-wide and belongs
///    to the host application's own UI. QueryDisplayConfig is unaffected.
///
///  - **Refresh.** EnumDisplaySettings rounds to an integer, so a 143.98 Hz
///    panel comes back as 143, and capture paced at 143 against a display
///    running 143.98 beats visibly. QueryDisplayConfig carries the rational.
std::unordered_map<std::string, DisplayMode> realDisplayModes()
{
    std::unordered_map<std::string, DisplayMode> byDevice;

    UINT32 pathCount = 0;
    UINT32 modeCount = 0;
    if (::GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) !=
        ERROR_SUCCESS)
        return byDevice;

    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modeCount);
    if (::QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data(), &modeCount,
                             modes.data(), nullptr) != ERROR_SUCCESS)
        return byDevice;
    paths.resize(pathCount);
    modes.resize(modeCount);

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        // The source's GDI name is what DXGI's DXGI_OUTPUT_DESC::DeviceName
        // also reports, which is what lets these two enumerations be joined.
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;

        DisplayMode mode;

        const DISPLAYCONFIG_RATIONAL& rate = path.targetInfo.refreshRate;
        if (rate.Denominator != 0) {
            mode.refreshMilliHz =
                static_cast<int>((static_cast<long long>(rate.Numerator) * 1000LL) /
                                 static_cast<long long>(rate.Denominator));
        }

        // The monitor's own name, straight from its EDID. This is what makes a
        // display identifiable without a number: "M27Q" is the label on the
        // bezel, and it does not change when Windows renumbers its list.
        DISPLAYCONFIG_TARGET_DEVICE_NAME targetName = {};
        targetName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
        targetName.header.size = sizeof(targetName);
        targetName.header.adapterId = path.targetInfo.adapterId;
        targetName.header.id = path.targetInfo.id;
        if (::DisplayConfigGetDeviceInfo(&targetName.header) == ERROR_SUCCESS &&
            targetName.flags.friendlyNameFromEdid) {
            mode.monitorName = narrow(targetName.monitorFriendlyDeviceName);
        }

        // The SOURCE mode is the desktop framebuffer — which is exactly what
        // Desktop Duplication hands back, so it is the size the pipeline will
        // really carry. The target's signal size can differ from it when the
        // display is scaling internally.
        const UINT32 sourceIdx = path.sourceInfo.modeInfoIdx;
        if (sourceIdx != DISPLAYCONFIG_PATH_MODE_IDX_INVALID && sourceIdx < modes.size() &&
            modes[sourceIdx].infoType == DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            mode.width = static_cast<int>(modes[sourceIdx].sourceMode.width);
            mode.height = static_cast<int>(modes[sourceIdx].sourceMode.height);
        }

        byDevice[narrow(source.viewGdiDeviceName)] = mode;
    }
    return byDevice;
}

/// Fallback when QueryDisplayConfig said nothing about this output.
///
/// EnumDisplaySettings reports the graphics mode rather than a DPI-virtualized
/// rectangle, so its pixel size is trustworthy even though its refresh is
/// rounded.
DisplayMode modeFromGdi(const wchar_t* deviceName)
{
    DEVMODEW dev = {};
    dev.dmSize = sizeof(dev);
    if (!::EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &dev)) return {};

    DisplayMode mode;
    mode.width = static_cast<int>(dev.dmPelsWidth);
    mode.height = static_cast<int>(dev.dmPelsHeight);
    mode.refreshMilliHz = static_cast<int>(dev.dmDisplayFrequency) * 1000;
    return mode;
}

/// The number Windows Settings puts on each display, keyed by GDI device name.
///
/// Not derivable from anything already at hand, which is why this needs its own
/// enumeration:
///
///  - our own index is just the order DXGI happened to enumerate adapters in;
///  - the GDI name is NOT the number — this machine reports `\\.\DISPLAY29`,
///    `\\.\DISPLAY27` and `\\.\DISPLAY2` for its three screens, because those
///    names persist across every monitor ever plugged in;
///  - `DISPLAYCONFIG_PATH_SOURCE_INFO::id` is 0 on all three (one source per
///    adapter), and `targetInfo.id` is a hardware id in the thousands.
///
/// What Settings shows is the RANK of the display's GDI index among the
/// attached ones, counting from 1. The names themselves are not the numbers —
/// they persist across every monitor ever plugged in, so this machine's three
/// screens are `\\.\DISPLAY2`, `\\.\DISPLAY27` and `\\.\DISPLAY29`, which
/// Settings presents as 1, 2 and 3 in that order.
///
/// Deliberately paired with the monitor's EDID name in the label rather than
/// trusted on its own: this rule matches what was observed on this machine, but
/// the numbering is not documented by Microsoft, and a name from the monitor
/// itself cannot be wrong.
std::unordered_map<std::string, int> windowsDisplayNumbers()
{
    // GDI index → device name, sorted by the map itself.
    std::map<unsigned long, std::string> attached;

    DISPLAY_DEVICEW device = {};
    device.cb = sizeof(device);

    for (DWORD index = 0; ::EnumDisplayDevicesW(nullptr, index, &device, 0); ++index) {
        device.cb = sizeof(device);
        // Mirrors are not displays anyone streams, and a device not on the
        // desktop has no number in Settings either.
        if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0) continue;
        if (device.StateFlags & DISPLAY_DEVICE_MIRRORING_DRIVER) continue;

        const std::string name = narrow(device.DeviceName);
        // "\\.\DISPLAY27" → 27. A name that does not parse keeps its place at
        // the end rather than colliding with a real index at 0.
        const size_t digits = name.find_last_not_of("0123456789");
        unsigned long gdiIndex = ULONG_MAX;
        if (digits != std::string::npos && digits + 1 < name.size())
            gdiIndex = std::strtoul(name.c_str() + digits + 1, nullptr, 10);
        attached[gdiIndex] = name;
    }

    std::unordered_map<std::string, int> byDevice;
    int number = 0;
    for (const auto& entry : attached)
        byDevice[entry.second] = ++number;
    return byDevice;
}

bool isPrimary(HMONITOR monitor)
{
    MONITORINFO info = {};
    info.cbSize = sizeof(info);
    if (!::GetMonitorInfoW(monitor, &info)) return false;
    return (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
}

/// Whether this output is scanning out an HDR signal RIGHT NOW.
///
/// A live property, not a capability: a display that can do HDR but sits in SDR
/// mode must report false, because SDR is what capture would actually yield and
/// promising HDR we cannot deliver is worse than not offering it.
bool isHdrActive(IDXGIOutput* output)
{
    ComPtr<IDXGIOutput6> output6;
    if (FAILED(output->QueryInterface(IID_PPV_ARGS(&output6)))) return false;

    DXGI_OUTPUT_DESC1 desc = {};
    if (FAILED(output6->GetDesc1(&desc))) return false;
    return desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020;
}

/// The one technical string a user sees: "Display 3", nothing more.
///
/// The NUMBER is what Windows' own display settings show, so the user can line
/// the two lists up. Windows renumbers it when monitors are plugged or
/// unplugged, and the EDID name would survive that — but a card title is read
/// at a glance, and the mode and the monitor's model spelled out in it only
/// made it unreadable. Both stay available: the mode in DisplayInfo's own
/// fields, and everything else in the probe log below.
std::string makeLabel(int number)
{
    return "Display " + std::to_string(number);
}

/// What the label no longer says, for the log: the monitor's own name straight
/// from its EDID (the one on the bezel, and the only thing that tells two
/// identical screens apart) and the mode. Falls back to the GDI device name
/// for a display with no EDID name, e.g. a virtual one.
std::string describe(const std::string& deviceName, const std::string& monitorName, int width,
                     int height, int refreshMilliHz)
{
    std::string detail = monitorName.empty() ? deviceName : monitorName;
    detail += " — " + std::to_string(width) + "\xC3\x97" +
              std::to_string(height); // U+00D7 MULTIPLICATION SIGN
    if (refreshMilliHz > 0) {
        detail += " \xC2\xB7 " + std::to_string((refreshMilliHz + 500) / 1000) + " Hz";
    }
    return detail;
}

} // namespace

bool hasInteractiveSession()
{
    // MoonlightWeb normally runs as a service in session 0, where neither
    // Desktop Duplication nor SendInput work at all. Saying so here, before any
    // capture API is touched, is what turns a confusing cascade of DXGI errors
    // into one accurate verdict — and it is why a native session has to be
    // launched into the console session instead.
    DWORD sessionId = 0;
    if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &sessionId)) return false;
    if (sessionId == 0) {
        log::info("[native] running in session 0 — no desktop to capture from here");
        return false;
    }

    // A session id is not proof on its own: the process must also be attached
    // to an interactive window station.
    const HWINSTA station = ::GetProcessWindowStation();
    if (!station) return false;

    USEROBJECTFLAGS flags = {};
    DWORD needed = 0;
    if (!::GetUserObjectInformationW(station, UOI_FLAGS, &flags, sizeof(flags), &needed))
        return false;
    return (flags.dwFlags & WSF_VISIBLE) != 0;
}

bool isOsSupported()
{
    // Windows 10 2004 (build 19041) is the floor: it is where the capture and
    // encoder paths this engine relies on are all present and maintained.
    // Read through RtlGetVersion, because GetVersionEx lies without a manifest.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);

    const HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;

    const auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")));
    if (!rtlGetVersion) return false;

    RTL_OSVERSIONINFOW info = {};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) return false;

    if (info.dwMajorVersion > 10) return true;
    if (info.dwMajorVersion < 10) return false;
    return info.dwBuildNumber >= 19041;
}

Unavailability enumerate(Capabilities& caps)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        caps.diagnostic = "DXGI is unavailable on this machine";
        return Unavailability::NoCaptureApi;
    }

    const std::unordered_map<std::string, DisplayMode> displayModes = realDisplayModes();
    const std::unordered_map<std::string, int> displayNumbers = windowsDisplayNumbers();

    int nextGpuId = 0;
    int nextDisplayId = 0;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 adapterDesc = {};
        if (FAILED(adapter->GetDesc1(&adapterDesc))) continue;

        // The Basic Render Driver has no encoder and no scanout; enumerating it
        // would only offer the user a GPU that cannot do the job.
        if (adapterDesc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) continue;

        GpuInfo gpu;
        gpu.id = nextGpuId;
        gpu.name = narrow(adapterDesc.Description);
        gpu.vendorId = adapterDesc.VendorId;
        gpu.deviceId = adapterDesc.DeviceId;
        // The LUID is how the encoder re-opens this exact adapter later. Packed
        // whole so no part of the identity is lost.
        gpu.nativeHandle =
            (static_cast<uint64_t>(static_cast<uint32_t>(adapterDesc.AdapterLuid.HighPart)) << 32) |
            static_cast<uint64_t>(adapterDesc.AdapterLuid.LowPart);

        bool adapterDrivesADisplay = false;

        ComPtr<IDXGIOutput> output;
        for (UINT j = 0;
             adapter->EnumOutputs(j, output.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
             ++j) {
            DXGI_OUTPUT_DESC outputDesc = {};
            if (FAILED(output->GetDesc(&outputDesc))) continue;
            // An output that is not on the desktop shows nothing and cannot be
            // duplicated.
            if (!outputDesc.AttachedToDesktop) continue;

            DisplayInfo display;
            display.id = nextDisplayId++;
            // This adapter enumerated the output, so this adapter scans it out.
            // That is the association §12 is about, stated by the OS rather
            // than inferred.
            display.gpuId = gpu.id;

            // Deliberately NOT from outputDesc.DesktopCoordinates: those are
            // DPI-scaled for a process that is not per-monitor DPI aware (a
            // 2560x1440 panel at 125 % reads as 2048x1152), and streaming the
            // scaled size would silently throw away a quarter of the pixels.
            const std::string deviceName = narrow(outputDesc.DeviceName);
            const auto found = displayModes.find(deviceName);
            DisplayMode mode = found != displayModes.end() ? found->second : DisplayMode{};
            if (mode.width <= 0 || mode.height <= 0 || mode.refreshMilliHz <= 0) {
                const DisplayMode fallback = modeFromGdi(outputDesc.DeviceName);
                if (mode.width <= 0 || mode.height <= 0) {
                    mode.width = fallback.width;
                    mode.height = fallback.height;
                }
                if (mode.refreshMilliHz <= 0) mode.refreshMilliHz = fallback.refreshMilliHz;
            }

            // Last resort only. The scaled rectangle is wrong whenever DPI
            // scaling is on, but a wrong size still beats a zero one.
            if (mode.width <= 0 || mode.height <= 0) {
                mode.width =
                    outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
                mode.height =
                    outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;
                log::warning("[native] " + deviceName +
                             ": falling back to DPI-scaled desktop coordinates");
            }

            display.width = mode.width;
            display.height = mode.height;
            display.refreshMilliHz = mode.refreshMilliHz;

            display.hdrActive = isHdrActive(output.Get());
            display.primary = isPrimary(outputDesc.Monitor);

            // The number Windows Settings shows, so the two lists agree. Falls
            // back to our own position only if the lookup fails, which would
            // leave the user with a number that matches nothing — better than
            // no number at all, and logged so it is not silent.
            const auto numberIt = displayNumbers.find(deviceName);
            int number = display.id + 1;
            if (numberIt != displayNumbers.end()) {
                number = numberIt->second;
            } else {
                log::warning("[native] " + deviceName +
                             ": no Windows display number — labelling by position");
            }
            display.label = makeLabel(number);
            display.detail = describe(deviceName, mode.monitorName, display.width, display.height,
                                      display.refreshMilliHz);

            caps.displays.push_back(std::move(display));
            adapterDrivesADisplay = true;
        }

        // A GPU that drives nothing is still worth keeping: it may be the only
        // one with an encoder, and borrowing it (at the cost of a cross-GPU
        // copy) beats refusing to stream. The Selector decides, not this.
        (void)adapterDrivesADisplay;

        probeEncoders(gpu);
        caps.gpus.push_back(std::move(gpu));
        ++nextGpuId;
    }

    if (caps.displays.empty()) {
        caps.diagnostic = "no display is attached to this machine";
        return Unavailability::NoDisplay;
    }

    // Which of DDA and WGC actually serves a session is settled when capture
    // starts; DDA is what is attempted first, so it is what is reported until
    // a fallback happens.
    caps.capture = CaptureApi::DxgiDuplication;

    if (log::enabled(log::Info)) {
        for (const DisplayInfo& display : caps.displays) {
            const GpuInfo* gpu = caps.gpuFor(display);
            log::info("[native] " + display.label + " \xC2\xB7 " + display.detail + " on " +
                      (gpu ? gpu->name : "unknown GPU") + (display.hdrActive ? " [HDR]" : "") +
                      (display.primary ? " [primary]" : ""));
        }
    }

    return Unavailability::None;
}

VirtualGamepad probeVirtualGamepad()
{
    VirtualGamepad result;
    // Windows has a backend, whatever the driver answers. That distinction is
    // the whole point: "supported but absent" is the one case where installing
    // something would help, and the only one worth telling a user about.
    result.supported = true;
    result.present = input::VigemGamepad::busPresent(result.diagnostic);
    return result;
}

} // namespace mw::native::platform
