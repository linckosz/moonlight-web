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

#pragma once

#include <cstdint>
#include <string>
#include <vector>

// What this machine can actually do, established by asking the OS — never by
// asking the user. See NativeHost::probe().
//
// Everything here is plain C++17 with no Qt and no GPL dependency: this module
// is kept separable on purpose (see LICENSE.md next to this tree).

namespace mw::native {

/// Video codecs the engine can produce. Ordered by preference, best first —
/// Selector walks this enum in order, so the ordering is load-bearing.
enum class Codec
{
    Av1,  ///< royalty-free patent licence (AOMedia), best quality per bit
    Hevc, ///< today's default for MoonlightWeb streams
    H264, ///< universal floor
};

const char* toString(Codec c);

/// Which vendor API an encoder is reached through. Never shown to the user as a
/// choice (§11 of the mission: no NVIDIA/AMD/Intel/Software selector) — it is
/// surfaced read-only in the stats overlay so a user can report what ran.
enum class EncoderApi
{
    None,
    Nvenc,           ///< NVIDIA Video Codec SDK
    Amf,             ///< AMD Advanced Media Framework
    Vpl,             ///< Intel oneVPL / libvpl
    VaApi,           ///< Linux, AMD + Intel
    VideoToolbox,    ///< Apple
    MediaFoundation, ///< Windows ARM64 (planned)
    Software,        ///< OpenH264 — only when a probe proved it keeps up
};

const char* toString(EncoderApi a);

/// Which capture API a display is grabbed through.
enum class CaptureApi
{
    None,
    DxgiDuplication,        ///< Windows, primary
    WindowsGraphicsCapture, ///< Windows, automatic fallback
    PipeWire,               ///< Linux, via the ScreenCast portal
    ScreenCaptureKit,       ///< macOS 12.3+
};

const char* toString(CaptureApi a);

/// A GPU, as the OS enumerates it.
struct GpuInfo
{
    /// Stable within one process run; the index into Capabilities::gpus.
    int id = -1;
    std::string name;      ///< e.g. "NVIDIA GeForce RTX 4070"
    uint32_t vendorId = 0; ///< PCI vendor id (0x10DE NVIDIA, 0x1002 AMD, 0x8086 Intel)
    uint32_t deviceId = 0;

    /// Opaque, platform-specific handle to the adapter this describes: a
    /// DXGI LUID on Windows, a DRM render-node path hash on Linux, a Metal
    /// registry id on macOS. The engine uses it to re-open the exact same
    /// adapter the display is attached to (see DisplayInfo::gpuId); nothing
    /// outside the module should interpret it.
    uint64_t nativeHandle = 0;

    /// Which encoder APIs answered on this GPU, best first. Empty means the GPU
    /// is present but has no usable encoder — a real case (very old cards,
    /// virtualized adapters) and the reason software fallback exists at all.
    std::vector<EncoderApi> encoders;

    /// Codecs this GPU's best encoder can produce. The intersection with what
    /// the browser can decode is computed later, per session.
    std::vector<Codec> codecs;

    /// True when the encoder can produce 10-bit output (HEVC Main10 / AV1
    /// 10-bit), which is what HDR requires.
    bool supports10Bit = false;

    /// True when the encoder can do 4:4:4 chroma. MoonlightWeb already offers
    /// this choice for external hosts, so the native engine has to answer it
    /// honestly rather than silently downgrade: 4:2:0 throws away three
    /// quarters of the colour resolution, which is invisible on video and very
    /// visible on text and thin UI lines — exactly what a desktop stream is.
    bool supports444 = false;
};

/// A display, as the OS enumerates it. This is what the user picks from — and
/// the ONLY thing they are ever asked to pick (§13 of the mission).
struct DisplayInfo
{
    /// Stable within one process run. Carried in SessionConfig::displayId and,
    /// above this module, used as the app id the browser launches.
    int id = -1;

    /// What the user sees, already formatted by the platform layer: the OS's
    /// own number and nothing else, e.g. "Display 1".
    std::string label;

    /// The same display spelled out for the log — monitor name and mode, e.g.
    /// "M27Q — 2560×1440 · 165 Hz". Never shown to the user; it is what the
    /// label used to carry before it became a card title.
    std::string detail;

    int width = 0;
    int height = 0;
    /// Refresh rate in millihertz, so 143.98 Hz survives the trip. Divide by
    /// 1000 for display; the engine uses the exact value to pace capture.
    int refreshMilliHz = 0;

    /// The GPU that physically drives this output — GpuInfo::id. This is the
    /// whole point of §12: capturing on GPU A to encode on GPU B costs a
    /// VRAM→RAM→VRAM round trip that dwarfs everything else in the pipeline.
    int gpuId = -1;

    /// True when the OS reports this output in an HDR mode right now. It is a
    /// live property, not a capability: a display that CAN do HDR but is in SDR
    /// mode reports false, because that is what capture would actually yield.
    bool hdrActive = false;

    /// True when this is the OS's primary display. Used only to pick a default
    /// ordering, never to restrict the choice.
    bool primary = false;
};

/// Why the native engine cannot run here. The caller maps every one of these to
/// the SAME user-facing sentence (§23: "not available on this configuration —
/// install Sunshine"), so the distinction exists for logs and telemetry only.
enum class Unavailability
{
    None, ///< the engine is usable

    NoDisplay,            ///< headless: nothing attached (no virtual display in v1)
    NoCaptureApi,         ///< Windows: DDA and WGC both failed · Linux: no portal
    CapturePermission,    ///< macOS Screen Recording (TCC) not granted
    NoEncoder,            ///< no hardware encoder AND software probe insufficient
    NoInteractiveSession, ///< Windows service with nobody logged in (§13)
    OsTooOld,             ///< Win10 < 2004 · macOS < 12.3 · Linux without PipeWire
    ArchNotSupported,     ///< Windows ARM64 in v1
    ProbeFailed,          ///< the probe itself threw or timed out
};

const char* toString(Unavailability u);

/// Everything probe() found. `available` is the only field callers outside this
/// module need to branch on; the rest feeds the stats overlay and the logs.
struct Capabilities
{
    bool available = false;
    Unavailability reason = Unavailability::ProbeFailed;

    std::vector<GpuInfo> gpus;
    std::vector<DisplayInfo> displays;

    /// The capture API that answered. Recorded so a session that silently fell
    /// back from DDA to WGC says so somewhere.
    CaptureApi capture = CaptureApi::None;

    /// Free-form, English, for the log line only. Never shown to a user.
    std::string diagnostic;

    const GpuInfo* gpuFor(const DisplayInfo& display) const;
};

/// Whether a gamepad can be presented to the OS right now.
///
/// Deliberately NOT a field of Capabilities: a missing gamepad bus never makes
/// the engine unavailable — it is a clean degradation, keyboard and mouse
/// intact — and probe() runs on every host-list refresh, which is no place to
/// open and close a driver handle.
struct VirtualGamepad
{
    /// False on a platform with no virtual-pad backend at all. Nothing the user
    /// installs would change that, so nothing must be offered to them.
    bool supported = false;

    /// The driver answered. This is the only trustworthy form of the question:
    /// a registry key or a file on disk survives a partial uninstall and lies.
    bool present = false;

    /// English, for the log line. Never shown to a user.
    std::string diagnostic;
};

} // namespace mw::native
