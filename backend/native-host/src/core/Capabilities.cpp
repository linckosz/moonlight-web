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

#include "mw/native/Capabilities.h"

namespace mw::native {

const char* toString(Codec c)
{
    switch (c) {
    case Codec::Av1: return "AV1";
    case Codec::Hevc: return "HEVC";
    case Codec::H264: return "H.264";
    }
    return "unknown";
}

const char* toString(EncoderApi a)
{
    switch (a) {
    case EncoderApi::None: return "none";
    case EncoderApi::Nvenc: return "NVENC";
    case EncoderApi::Amf: return "AMF";
    case EncoderApi::Vpl: return "oneVPL";
    case EncoderApi::VaApi: return "VA-API";
    case EncoderApi::VideoToolbox: return "VideoToolbox";
    case EncoderApi::MediaFoundation: return "Media Foundation";
    case EncoderApi::Software: return "software";
    }
    return "unknown";
}

const char* toString(CaptureApi a)
{
    switch (a) {
    case CaptureApi::None: return "none";
    case CaptureApi::DxgiDuplication: return "DXGI Desktop Duplication";
    case CaptureApi::WindowsGraphicsCapture: return "Windows.Graphics.Capture";
    case CaptureApi::PipeWire: return "PipeWire";
    case CaptureApi::ScreenCaptureKit: return "ScreenCaptureKit";
    }
    return "unknown";
}

const char* toString(Unavailability u)
{
    switch (u) {
    case Unavailability::None: return "available";
    case Unavailability::NoDisplay: return "no display attached";
    case Unavailability::NoCaptureApi: return "no usable capture API";
    case Unavailability::CapturePermission: return "screen capture permission not granted";
    case Unavailability::NoEncoder: return "no usable encoder";
    case Unavailability::NoInteractiveSession: return "no interactive desktop session";
    case Unavailability::OsTooOld: return "operating system too old";
    case Unavailability::ArchNotSupported: return "architecture not supported";
    case Unavailability::ProbeFailed: return "probe failed";
    }
    return "unknown";
}

const GpuInfo* Capabilities::gpuFor(const DisplayInfo& display) const
{
    for (const GpuInfo& gpu : gpus) {
        if (gpu.id == display.gpuId) return &gpu;
    }
    // A display whose GPU we could not identify is not a fatal error on its own
    // — the caller falls back to the first GPU that has an encoder, paying one
    // cross-GPU copy (§6) rather than refusing to stream.
    return nullptr;
}

} // namespace mw::native
