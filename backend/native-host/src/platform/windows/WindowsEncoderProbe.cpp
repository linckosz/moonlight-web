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
#include "../../encode/windows/AmfCapabilities.h"
#include "../../encode/windows/NvencCapabilities.h"
#include "../../encode/windows/VplCapabilities.h"

#include <windows.h>

namespace mw::native::platform {
namespace {

// PCI vendor ids. Used only to decide WHICH encoder API is worth asking about —
// never to conclude that one is present. The conclusion always comes from the
// hardware answering.
constexpr uint32_t kVendorNvidia = 0x10DE;
constexpr uint32_t kVendorAmd = 0x1002;
constexpr uint32_t kVendorIntel = 0x8086;

/// Whether a vendor runtime is installed and exposes its entry point.
///
/// A necessary condition, never a sufficient one: the DLL is machine-wide, so
/// it says nothing about a particular adapter. Kept for the vendors whose real
/// query is not implemented yet, where it is honestly labelled as unverified.
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
    case kVendorNvidia: {
        // The real question, asked of the hardware: open an encode session on
        // THIS adapter and read back what it supports. It is what separates a
        // genuine RTX from a virtual-display adapter wearing its name and
        // device id — see NvencCapabilities.h.
        const encode::NvencCaps caps = encode::queryNvencCapabilities(gpu.nativeHandle);
        if (caps.usable) {
            gpu.encoders.push_back(EncoderApi::Nvenc);
            gpu.codecs = caps.codecs;
            gpu.supports10Bit = caps.supports10Bit;
            gpu.supports444 = caps.supports444;
        }
        break;
    }

    case kVendorAmd: {
        // Same real question as NVIDIA's: create an AMF context on THIS adapter
        // and try each encoder component. That is what separates an RX 7600
        // (which does AV1) from an older Radeon on the same driver.
        const encode::AmfCaps caps = encode::queryAmfCapabilities(gpu.nativeHandle);
        if (caps.usable) {
            gpu.encoders.push_back(EncoderApi::Amf);
            gpu.codecs = caps.codecs;
            gpu.supports10Bit = caps.supports10Bit;
            gpu.supports444 = caps.supports444;
        }
        break;
    }

    case kVendorIntel: {
        // Same real question as the other two: open a oneVPL session on THIS
        // adapter and ask the encoder itself. That is what separates an Arc
        // card from a UHD iGPU on the same dispatcher — they differ on AV1.
        const encode::VplCaps caps = encode::queryVplCapabilities(gpu.nativeHandle);
        if (caps.usable) {
            gpu.encoders.push_back(EncoderApi::Vpl);
            gpu.codecs = caps.codecs;
            gpu.supports10Bit = caps.supports10Bit;
            gpu.supports444 = caps.supports444;
        }
        break;
    }

    default: break;
    }

    if (!log::enabled(log::Info)) return;

    if (gpu.encoders.empty()) {
        log::info("[native] " + gpu.name + ": no usable encoder");
        return;
    }

    // The distinction the log must preserve: a codec list means the hardware
    // answered, an empty one means only a runtime was found.
    if (gpu.codecs.empty()) {
        log::info("[native] " + gpu.name + ": " + toString(gpu.encoders.front()) +
                  " runtime present (unverified — no session opened yet)");
        return;
    }

    std::string codecs;
    for (Codec c : gpu.codecs) {
        if (!codecs.empty()) codecs += ", ";
        codecs += toString(c);
    }
    log::info("[native] " + gpu.name + ": " + toString(gpu.encoders.front()) + " confirmed — " +
              codecs + (gpu.supports10Bit ? " (10-bit)" : "") +
              (gpu.supports444 ? " (4:4:4)" : ""));
}

} // namespace mw::native::platform
