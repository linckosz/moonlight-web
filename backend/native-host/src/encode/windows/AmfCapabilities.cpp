/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "AmfCapabilities.h"

#include "../../core/Log.h"
#include "AmfApi.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <components/VideoEncoderAV1.h>
#include <components/VideoEncoderHEVC.h>
#include <components/VideoEncoderVCE.h>

#include <map>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace mw::native::encode {
namespace {

/// A D3D11 device on one specific adapter. D3D_DRIVER_TYPE_UNKNOWN is mandatory
/// when an adapter is supplied — asking for HARDWARE silently ignores it and
/// answers for the default GPU, which on a two-dGPU machine is the wrong one.
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

AmfCaps queryUncached(uint64_t adapterLuid)
{
    AmfCaps caps;

    const AmfApi* api = AmfApi::instance();
    if (!api->available()) {
        log::info("[native] AMF unavailable: " + api->unavailableReason());
        return caps;
    }

    ComPtr<ID3D11Device> device = openDeviceOnAdapter(adapterLuid);
    if (!device) return caps;

    amf::AMFContextPtr context;
    AMF_RESULT result = api->factory()->CreateContext(&context);
    if (result != AMF_OK || !context) {
        log::info(std::string("[native] AMF context refused: ") + AmfApi::resultToString(result));
        return caps;
    }

    // Bind the context to THIS adapter's device. Without this AMF would pick a
    // device of its own and answer for the wrong GPU on a multi-GPU machine.
    result = context->InitDX11(device.Get());
    if (result != AMF_OK) {
        log::info(std::string("[native] AMF could not bind this adapter: ") +
                  AmfApi::resultToString(result));
        return caps;
    }

    // Walked in the engine's preference order, so the list handed to the
    // Selector is already sorted best-first.
    const struct
    {
        Codec codec;
        const wchar_t* component;
    } wanted[] = {
        {Codec::Av1, AMFVideoEncoder_AV1},
        {Codec::Hevc, AMFVideoEncoder_HEVC},
        {Codec::H264, AMFVideoEncoderVCE_AVC},
    };

    for (const auto& candidate : wanted) {
        amf::AMFComponentPtr encoder;
        // Creating the component IS the question: an encoder the silicon does
        // not have refuses here, which is how an RDNA2 card declines AV1 while
        // an RX 7600 accepts it — with no device-id table to maintain.
        if (api->factory()->CreateComponent(context, candidate.component, &encoder) != AMF_OK ||
            !encoder)
            continue;

        caps.codecs.push_back(candidate.codec);

        // 10-bit is asked of HEVC only, and only through the capability the
        // header actually exposes: whether the encoder reaches the Main10
        // profile. AV1's 10-bit and 4:4:4 have no equally plain query here, so
        // they are not claimed — an unclaimed capability costs a better codec,
        // a wrongly claimed one costs a failed session.
        if (candidate.codec == Codec::Hevc) {
            amf::AMFCapsPtr encoderCaps;
            if (encoder->GetCaps(&encoderCaps) == AMF_OK && encoderCaps) {
                amf_int64 maxProfile = 0;
                if (encoderCaps->GetProperty(AMF_VIDEO_ENCODER_HEVC_CAP_MAX_PROFILE, &maxProfile) ==
                        AMF_OK &&
                    maxProfile >= AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10)
                    caps.supports10Bit = true;
            }
        }

        encoder->Terminate();
    }

    context->Terminate();
    caps.usable = !caps.codecs.empty();
    return caps;
}

} // namespace

AmfCaps queryAmfCapabilities(uint64_t adapterLuid)
{
    static std::mutex mutex;
    static std::map<uint64_t, AmfCaps> cache;

    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = cache.find(adapterLuid);
        if (found != cache.end()) return found->second;
    }

    // Queried outside the lock: creating a context touches the driver and takes
    // milliseconds, and holding a global mutex across it would serialize every
    // concurrent probe on the slowest adapter. A duplicate query on a race is
    // harmless — it is a read-only question with a stable answer.
    AmfCaps caps = queryUncached(adapterLuid);

    std::lock_guard<std::mutex> lock(mutex);
    return cache.insert({adapterLuid, caps}).first->second;
}

} // namespace mw::native::encode
