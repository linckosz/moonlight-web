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

#include "CrossGpuBridge.h"

#include "../../core/Log.h"

#include <dxgi1_6.h>

#include <chrono>
#include <cstdio>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace mw::native {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string hresultToString(HRESULT hr)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

/// Bytes per pixel of the formats Desktop Duplication hands out. Anything else
/// is refused rather than guessed: a wrong stride here is a sheared picture.
size_t bytesPerPixel(DXGI_FORMAT format)
{
    switch (format) {
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM: return 4;
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
    default: return 0;
    }
}

} // namespace

CrossGpuBridge::CrossGpuBridge(uint64_t encodeAdapterLuid)
    : m_AdapterLuid(encodeAdapterLuid)
{}

CrossGpuBridge::~CrossGpuBridge() = default;

bool CrossGpuBridge::open(std::string& error)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        error = "DXGI is unavailable";
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIAdapter1> candidate;
    for (UINT i = 0;
         factory->EnumAdapters1(i, candidate.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(candidate->GetDesc1(&desc))) continue;
        const uint64_t luid =
            (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
            static_cast<uint64_t>(desc.AdapterLuid.LowPart);
        if (luid == m_AdapterLuid) {
            adapter = candidate;
            break;
        }
    }
    if (!adapter) {
        error = "the GPU chosen to encode is no longer present";
        return false;
    }

    // UNKNOWN is required when an adapter is named — see DxgiDuplication.
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained = {};
    const HRESULT hr = ::D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, wanted,
        static_cast<UINT>(std::size(wanted)), D3D11_SDK_VERSION, m_Device.ReleaseAndGetAddressOf(),
        &obtained, m_Context.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error = "could not create a D3D11 device on the encoding GPU (" + hresultToString(hr) + ")";
        return false;
    }
    return true;
}

bool CrossGpuBridge::ensureBuffers(ID3D11Device* sourceDevice, const D3D11_TEXTURE2D_DESC& desc,
                                   std::string& error)
{
    const bool same = m_Staging && m_Upload && m_StagingDevice == sourceDevice &&
                      m_Width == desc.Width && m_Height == desc.Height && m_Format == desc.Format;
    if (same) return true;

    const size_t bpp = bytesPerPixel(desc.Format);
    if (bpp == 0) {
        error = "cross-GPU copy: unsupported capture format " +
                std::to_string(static_cast<int>(desc.Format));
        return false;
    }

    // Readback on the source: nothing bound, CPU-readable, the GPU writes it
    // with CopyResource and Map waits for that write.
    D3D11_TEXTURE2D_DESC staging = desc;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.BindFlags = 0;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    staging.MiscFlags = 0;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    if (FAILED(
            sourceDevice->CreateTexture2D(&staging, nullptr, m_Staging.ReleaseAndGetAddressOf()))) {
        error = "cross-GPU copy: the display's GPU refused a readback texture";
        m_Staging.Reset();
        return false;
    }

    // Upload on the destination: dynamic, so Map(WRITE_DISCARD) hands out a
    // fresh allocation each frame and the converter's read of the previous one
    // never has to be waited for.
    D3D11_TEXTURE2D_DESC upload = desc;
    upload.Usage = D3D11_USAGE_DYNAMIC;
    upload.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    upload.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    upload.MiscFlags = 0;
    upload.MipLevels = 1;
    upload.ArraySize = 1;
    if (FAILED(m_Device->CreateTexture2D(&upload, nullptr, m_Upload.ReleaseAndGetAddressOf()))) {
        error = "cross-GPU copy: the encoding GPU refused an upload texture";
        m_Staging.Reset();
        m_Upload.Reset();
        return false;
    }

    m_StagingDevice = sourceDevice;
    m_Width = desc.Width;
    m_Height = desc.Height;
    m_Format = desc.Format;
    m_BytesPerPixel = bpp;
    m_BytesPerFrame = static_cast<size_t>(desc.Width) * desc.Height * bpp;
    log::info("[native] cross-GPU copy: " + std::to_string(desc.Width) + "x" +
              std::to_string(desc.Height) + ", " + std::to_string(m_BytesPerFrame / (1024 * 1024)) +
              " MB per frame through system memory");
    return true;
}

ID3D11Texture2D* CrossGpuBridge::transfer(ID3D11Device* sourceDevice,
                                          ID3D11DeviceContext* sourceContext,
                                          ID3D11Texture2D* source, std::string& error)
{
    if (!m_Device || !sourceDevice || !sourceContext || !source) {
        error = "cross-GPU copy: nothing to copy from";
        return nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    if (!ensureBuffers(sourceDevice, desc, error)) return nullptr;

    const int64_t startUs = steadyNowUs();

    sourceContext->CopyResource(m_Staging.Get(), source);

    // Map blocks until the copy above has landed: this is where the wait for
    // the source GPU happens, and it is the first of the two real costs.
    D3D11_MAPPED_SUBRESOURCE in = {};
    HRESULT hr = sourceContext->Map(m_Staging.Get(), 0, D3D11_MAP_READ, 0, &in);
    if (FAILED(hr)) {
        error = "cross-GPU copy: could not read the frame back (" + hresultToString(hr) + ")";
        return nullptr;
    }

    D3D11_MAPPED_SUBRESOURCE out = {};
    hr = m_Context->Map(m_Upload.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &out);
    if (FAILED(hr)) {
        sourceContext->Unmap(m_Staging.Get(), 0);
        error = "cross-GPU copy: could not write the frame to the encoding GPU (" +
                hresultToString(hr) + ")";
        return nullptr;
    }

    // Row by row: the two pitches are each GPU's own and rarely agree. The
    // second real cost — a memcpy of the whole frame.
    const size_t rowBytes = static_cast<size_t>(m_Width) * m_BytesPerPixel;
    const uint8_t* src = static_cast<const uint8_t*>(in.pData);
    uint8_t* dst = static_cast<uint8_t*>(out.pData);
    if (in.RowPitch == out.RowPitch && in.RowPitch == rowBytes) {
        std::memcpy(dst, src, rowBytes * m_Height);
    } else {
        for (UINT y = 0; y < m_Height; ++y)
            std::memcpy(dst + static_cast<size_t>(y) * out.RowPitch,
                        src + static_cast<size_t>(y) * in.RowPitch, rowBytes);
    }

    m_Context->Unmap(m_Upload.Get(), 0);
    sourceContext->Unmap(m_Staging.Get(), 0);

    const int64_t tookUs = steadyNowUs() - startUs;
    m_Transfers++;
    m_TotalUs += tookUs;
    if (tookUs > m_MaxUs) m_MaxUs = tookUs;
    return m_Upload.Get();
}

} // namespace mw::native
