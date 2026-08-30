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

#include "WindowsEncoderProbe.h"

#include "../../core/Log.h"

#include <windows.h>

namespace mw::native::platform {
namespace {

// PCI vendor ids. An encoder runtime is vendor-wide, so pairing the DLL with
// the adapter's vendor is what keeps the answer per-GPU rather than per-machine
// — otherwise an NVIDIA driver on a box with an Intel iGPU would credit NVENC
// to the iGPU.
constexpr uint32_t kVendorNvidia = 0x10DE;
constexpr uint32_t kVendorAmd = 0x1002;
constexpr uint32_t kVendorIntel = 0x8086;

/// Whether a vendor runtime is installed and exposes its entry point.
///
/// GetModuleHandle first so an already-loaded runtime is not loaded twice, then
/// LoadLibrary. The handle is deliberately not freed: these are driver DLLs the
/// process will use again within seconds, and unloading a graphics runtime has
/// a habit of taking worker threads with it.
bool runtimePresent(const wchar_t* dll, const char* entryPoint)
{
    HMODULE module = ::GetModuleHandleW(dll);
    if (!module) module = ::LoadLibraryW(dll);
    if (!module) return false;
    return ::GetProcAddress(module, entryPoint) != nullptr;
}

} // namespace

void probeEncoders(GpuInfo& gpu)
{
    switch (gpu.vendorId) {
    case kVendorNvidia:
        // Ships with every NVIDIA display driver.
        if (runtimePresent(L"nvEncodeAPI64.dll", "NvEncodeAPICreateInstance"))
            gpu.encoders.push_back(EncoderApi::Nvenc);
        break;

    case kVendorAmd:
        if (runtimePresent(L"amfrt64.dll", "AMFInit")) gpu.encoders.push_back(EncoderApi::Amf);
        break;

    case kVendorIntel:
        // The oneVPL dispatcher, else the legacy Media SDK runtime that older
        // drivers still install.
        if (runtimePresent(L"libvpl.dll", "MFXLoad") ||
            runtimePresent(L"libmfxhw64.dll", "MFXInit"))
            gpu.encoders.push_back(EncoderApi::Vpl);
        break;

    default: break;
    }

    // Codecs are deliberately left empty — see the contract in the header. The
    // engine therefore still reports itself unavailable, which is the honest
    // answer while no encoder backend exists to ask the hardware.
    //
    // The wording of the log line matters: this is a runtime that COULD serve
    // this vendor, not a verdict on this adapter. An indirect-display adapter
    // reaches this line looking exactly like the GPU it borrows its name from.
    if (log::enabled(log::Info)) {
        log::info("[native] " + gpu.name + ": " +
                  (gpu.encoders.empty()
                       ? std::string("no encoder runtime found")
                       : std::string(toString(gpu.encoders.front())) +
                             " runtime present (unverified — no session opened yet)"));
    }
}

} // namespace mw::native::platform
