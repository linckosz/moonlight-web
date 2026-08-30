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
#include "WindowsEncoderProbe.h"

#include <windows.h>

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
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

/// Exact refresh rate per GDI device name ("\\\\.\\DISPLAY1"), in millihertz.
///
/// EnumDisplaySettings would be one call, but it reports a rounded integer:
/// a 143.98 Hz panel comes back as 143, and capture paced at 143 against a
/// display running 143.98 beats visibly. QueryDisplayConfig carries the true
/// rational, so it is worth the extra work.
std::unordered_map<std::string, int> exactRefreshRates()
{
    std::unordered_map<std::string, int> byDevice;

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

    for (const DISPLAYCONFIG_PATH_INFO& path : paths) {
        const DISPLAYCONFIG_RATIONAL& rate = path.targetInfo.refreshRate;
        if (rate.Denominator == 0) continue;

        // The source's GDI name is what DXGI's DXGI_OUTPUT_DESC::DeviceName
        // also reports, which is what lets these two enumerations be joined.
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source = {};
        source.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
        source.header.size = sizeof(source);
        source.header.adapterId = path.sourceInfo.adapterId;
        source.header.id = path.sourceInfo.id;
        if (::DisplayConfigGetDeviceInfo(&source.header) != ERROR_SUCCESS) continue;

        const long long milliHz = (static_cast<long long>(rate.Numerator) * 1000LL) /
                                  static_cast<long long>(rate.Denominator);
        byDevice[narrow(source.viewGdiDeviceName)] = static_cast<int>(milliHz);
    }
    return byDevice;
}

/// Fallback when QueryDisplayConfig said nothing about this output.
int refreshFromGdi(const wchar_t* deviceName)
{
    DEVMODEW mode = {};
    mode.dmSize = sizeof(mode);
    if (!::EnumDisplaySettingsW(deviceName, ENUM_CURRENT_SETTINGS, &mode)) return 0;
    return static_cast<int>(mode.dmDisplayFrequency) * 1000;
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

/// "Display 1 — 2560×1440 · 144 Hz" — the only technical string a user sees.
std::string makeLabel(int index, int width, int height, int refreshMilliHz)
{
    std::string label = "Display " + std::to_string(index) + " — " + std::to_string(width) +
                        "\xC3\x97" + std::to_string(height); // U+00D7 MULTIPLICATION SIGN
    if (refreshMilliHz > 0) {
        label += " \xC2\xB7 " + std::to_string((refreshMilliHz + 500) / 1000) + " Hz";
    }
    return label;
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

    const std::unordered_map<std::string, int> refreshRates = exactRefreshRates();

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
            display.width =
                outputDesc.DesktopCoordinates.right - outputDesc.DesktopCoordinates.left;
            display.height =
                outputDesc.DesktopCoordinates.bottom - outputDesc.DesktopCoordinates.top;

            const std::string deviceName = narrow(outputDesc.DeviceName);
            const auto found = refreshRates.find(deviceName);
            display.refreshMilliHz =
                found != refreshRates.end() ? found->second : refreshFromGdi(outputDesc.DeviceName);

            display.hdrActive = isHdrActive(output.Get());
            display.primary = isPrimary(outputDesc.Monitor);
            display.label =
                makeLabel(display.id + 1, display.width, display.height, display.refreshMilliHz);

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
            log::info("[native] " + display.label + " on " + (gpu ? gpu->name : "unknown GPU") +
                      (display.hdrActive ? " [HDR]" : "") + (display.primary ? " [primary]" : ""));
        }
    }

    return Unavailability::None;
}

} // namespace mw::native::platform
