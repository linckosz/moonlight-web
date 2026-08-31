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

#include "NvencCapabilities.h"

#include "../../core/Log.h"
#include "NvencApi.h"

#include <windows.h>

#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <map>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace mw::native::encode {
namespace {

/// A D3D11 device on one specific adapter, for the length of the query.
///
/// D3D_DRIVER_TYPE_UNKNOWN is mandatory when an adapter is supplied: asking for
/// HARDWARE silently ignores the adapter and picks the default one, which on
/// this multi-GPU bench would answer for the wrong GPU entirely.
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

int queryCap(const NV_ENCODE_API_FUNCTION_LIST& fn, void* encoder, const GUID& codec,
             NV_ENC_CAPS which)
{
    NV_ENC_CAPS_PARAM param = {};
    param.version = NV_ENC_CAPS_PARAM_VER;
    param.capsToQuery = which;

    int value = 0;
    if (fn.nvEncGetEncodeCaps(encoder, codec, &param, &value) != NV_ENC_SUCCESS) return 0;
    return value;
}

bool guidsEqual(const GUID& a, const GUID& b)
{
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

NvencCaps queryUncached(uint64_t adapterLuid)
{
    NvencCaps caps;

    const NvencApi* api = NvencApi::instance();
    if (!api->available()) {
        log::info("[native] NVENC unavailable: " + api->unavailableReason());
        return caps;
    }

    ComPtr<ID3D11Device> device = openDeviceOnAdapter(adapterLuid);
    if (!device) return caps;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params = {};
    params.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    params.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    params.device = device.Get();
    params.apiVersion = NVENCAPI_VERSION;

    void* encoder = nullptr;
    const NVENCSTATUS status = api->fn().nvEncOpenEncodeSessionEx(&params, &encoder);
    if (status != NV_ENC_SUCCESS || !encoder) {
        // Not a warning: an adapter with no encoder is a normal thing to find
        // (an older card, a virtualized adapter, a GPU whose encoder is busy),
        // and it is handled by leaving the codec list empty rather than by
        // failing anything.
        log::info("[native] adapter cannot host an NVENC session (" +
                  std::string(NvencApi::statusToString(status)) + ")");
        if (encoder) api->fn().nvEncDestroyEncoder(encoder);
        return caps;
    }

    uint32_t guidCount = 0;
    if (api->fn().nvEncGetEncodeGUIDCount(encoder, &guidCount) == NV_ENC_SUCCESS && guidCount > 0) {
        std::vector<GUID> guids(guidCount);
        uint32_t returned = 0;
        if (api->fn().nvEncGetEncodeGUIDs(encoder, guids.data(), guidCount, &returned) ==
            NV_ENC_SUCCESS) {
            guids.resize(returned);

            // Walked in the engine's own preference order rather than the
            // driver's, so the list handed to the Selector is already sorted
            // best-first and the Selector never has to re-rank it.
            const struct
            {
                Codec codec;
                const GUID& guid;
            } wanted[] = {
                {Codec::Av1, NV_ENC_CODEC_AV1_GUID},
                {Codec::Hevc, NV_ENC_CODEC_HEVC_GUID},
                {Codec::H264, NV_ENC_CODEC_H264_GUID},
            };

            for (const auto& candidate : wanted) {
                for (const GUID& supported : guids) {
                    if (!guidsEqual(candidate.guid, supported)) continue;
                    caps.codecs.push_back(candidate.codec);

                    // Capabilities are per codec; report the union, because the
                    // session that matters is the one that gets picked and it
                    // will re-check for itself.
                    if (queryCap(api->fn(), encoder, supported, NV_ENC_CAPS_SUPPORT_10BIT_ENCODE))
                        caps.supports10Bit = true;
                    if (queryCap(api->fn(), encoder, supported, NV_ENC_CAPS_SUPPORT_INTRA_REFRESH))
                        caps.supportsIntraRefresh = true;
                    if (queryCap(api->fn(), encoder, supported,
                                 NV_ENC_CAPS_SUPPORT_REF_PIC_INVALIDATION))
                        caps.supportsReferenceInvalidation = true;
                    break;
                }
            }
        }
    }

    api->fn().nvEncDestroyEncoder(encoder);
    caps.usable = !caps.codecs.empty();
    return caps;
}

} // namespace

NvencCaps queryNvencCapabilities(uint64_t adapterLuid)
{
    // Cached for the life of the process: a GPU's encoder does not change under
    // a running driver, and probe() is called often enough (every listSeats,
    // every launch) that opening a session each time would be milliseconds
    // spent to learn nothing new.
    static std::mutex mutex;
    static std::map<uint64_t, NvencCaps> cache;

    {
        std::lock_guard<std::mutex> lock(mutex);
        const auto found = cache.find(adapterLuid);
        if (found != cache.end()) return found->second;
    }

    // Queried outside the lock: opening a session takes milliseconds and can
    // touch the driver, and holding a global mutex across it would serialize
    // every concurrent probe on the slowest adapter. A duplicate query on a
    // race is harmless — it is a read-only question with a stable answer.
    NvencCaps caps = queryUncached(adapterLuid);

    std::lock_guard<std::mutex> lock(mutex);
    return cache.insert({adapterLuid, caps}).first->second;
}

} // namespace mw::native::encode
