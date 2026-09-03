/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "VplCapabilities.h"

#include "../../core/Log.h"
#include "VplSession.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <map>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace mw::native::encode {
namespace {

/// A D3D11 device on one specific adapter. D3D_DRIVER_TYPE_UNKNOWN is mandatory
/// when an adapter is supplied — asking for HARDWARE silently ignores it and
/// answers for the default GPU.
ComPtr<ID3D11Device> openDeviceOnAdapter(uint64_t adapterLuid)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         factory->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) continue;

        const uint64_t luid =
            (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
            static_cast<uint64_t>(desc.AdapterLuid.LowPart);
        if (luid != adapterLuid) continue;

        const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        ComPtr<ID3D11Device> device;
        if (SUCCEEDED(::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0,
                                          wanted, static_cast<UINT>(std::size(wanted)),
                                          D3D11_SDK_VERSION, device.GetAddressOf(), nullptr,
                                          nullptr)))
            return device;
        return nullptr;
    }
    return nullptr;
}

/// Whether this session can encode @p codec at a representative size.
///
/// MFXVideoENCODE_Query answers without allocating a thing, which is what makes
/// this cheap enough to run for three codecs at probe time.
///
/// MFX_WRN_* are accepted deliberately: a warning means the runtime adjusted
/// something and CAN encode, and treating that as a refusal would drop codecs
/// the hardware really has. Only a hard error is a no.
bool canEncode(const VplSession& session, Codec codec, bool tenBit)
{
    mfxVideoParam in = {};
    if (!fillEncodeParams(in, codec, 1920, 1080, 60, 20000)) return false;

    if (tenBit) {
        // 10-bit lives in P010 with a bit depth of 10 — what HDR needs.
        in.mfx.FrameInfo.FourCC = MFX_FOURCC_P010;
        in.mfx.FrameInfo.BitDepthLuma = 10;
        in.mfx.FrameInfo.BitDepthChroma = 10;
        in.mfx.FrameInfo.Shift = 1;
    }

    mfxVideoParam out = {};
    out.mfx.CodecId = in.mfx.CodecId;

    const mfxStatus status = session.api()->EncodeQuery(session.handle(), &in, &out);
    return status == MFX_ERR_NONE || status == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM ||
           status == MFX_WRN_VIDEO_PARAM_CHANGED;
}

VplCaps queryUncached(uint64_t adapterLuid)
{
    VplCaps caps;

    ComPtr<ID3D11Device> device = openDeviceOnAdapter(adapterLuid);
    if (!device) return caps;

    VplSession session;
    std::string error;
    if (!session.open(device.Get(), error)) {
        log::info("[native] oneVPL unavailable on this adapter: " + error);
        return caps;
    }

    // Walked in the engine's preference order, so the list handed to the
    // Selector is already sorted best-first.
    const Codec wanted[] = {Codec::Av1, Codec::Hevc, Codec::H264};
    for (Codec codec : wanted) {
        if (!canEncode(session, codec, false)) continue;
        caps.codecs.push_back(codec);
        // 10-bit is asked per codec and only credited where it answered: an
        // unclaimed capability costs a better codec, a wrongly claimed one
        // costs a failed session.
        if ((codec == Codec::Hevc || codec == Codec::Av1) && canEncode(session, codec, true))
            caps.supports10Bit = true;
    }

    // 4:4:4 is not claimed: the conversion pass produces AYUV, which oneVPL
    // does not take as an encoder input, so there is nothing to feed it with
    // even where the silicon could. Left for whoever adds a Y410/AYUV path.
    caps.codecs444.clear();

    caps.usable = !caps.codecs.empty();
    return caps;
}

} // namespace

VplCaps queryVplCapabilities(uint64_t adapterLuid)
{
    static std::mutex mutex;
    static std::map<uint64_t, VplCaps> cache;

    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = cache.find(adapterLuid);
        if (found != cache.end()) return found->second;
    }

    // Queried outside the lock: opening a session touches the driver and takes
    // milliseconds, and holding a global mutex across it would serialize every
    // concurrent probe on the slowest adapter. A duplicate query on a race is
    // harmless — it is a read-only question with a stable answer.
    VplCaps caps = queryUncached(adapterLuid);

    std::lock_guard<std::mutex> lock(mutex);
    return cache.insert({adapterLuid, caps}).first->second;
}

} // namespace mw::native::encode
