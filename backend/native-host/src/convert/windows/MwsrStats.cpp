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

#include "MwsrStats.h"
#include "ConvertShaders.h"

#include <cstring>

using Microsoft::WRL::ComPtr;

namespace mw::native::convert {

namespace mz = mwsrzones;

bool MwsrStats::init(ID3D11Device* device, int width, int height, bool light, std::string& error)
{
    // A new size, or a new device: nothing of the old pass is kept, and no
    // ticket of it reads (m_Last goes on counting).
    m_Device.Reset();
    m_Context.Reset();
    m_Shader.Reset();
    m_Cells.Reset();
    m_CellsView.Reset();
    for (int slot = 0; slot < kSlots; ++slot) {
        m_Readback[slot].Reset();
        m_Ticket[slot] = 0;
    }
    m_Width = m_Height = 0;
    m_Light = false;
    m_Timer.reset();
    if (!device) {
        error = "no D3D11 device";
        return false;
    }
    if (device->GetFeatureLevel() < D3D_FEATURE_LEVEL_11_0) {
        error = "no compute shaders below feature level 11_0";
        return false;
    }
    // A cell spans size / cells pixels, rounded down: below the grid's own
    // size some would have none, and their means would be 0 / 0.
    if (width < mz::kCellsX || height < mz::kCellsY) {
        error = "capture smaller than MWSR's statistics grid";
        return false;
    }

    ComPtr<ID3DBlob> blob;
    if (!compileMwsrStatsShader(width, height, light, blob, error)) return false;
    if (FAILED(device->CreateComputeShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr,
                                           m_Shader.GetAddressOf()))) {
        error = "could not create MWSR's statistics pass";
        return false;
    }

    // Two float4 per cell, the cells row by row: the layout MwsrZones.h reads.
    D3D11_BUFFER_DESC desc = {};
    desc.ByteWidth = static_cast<UINT>(sizeof(float) * kFloats);
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    desc.StructureByteStride = sizeof(float) * 4;
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav = {};
    uav.Format = DXGI_FORMAT_UNKNOWN;
    uav.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav.Buffer.NumElements = static_cast<UINT>(kFloats / 4);
    if (FAILED(device->CreateBuffer(&desc, nullptr, m_Cells.GetAddressOf())) ||
        FAILED(
            device->CreateUnorderedAccessView(m_Cells.Get(), &uav, m_CellsView.GetAddressOf()))) {
        error = "could not create MWSR's statistics buffer";
        return false;
    }

    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (ComPtr<ID3D11Buffer>& readback : m_Readback) {
        if (FAILED(device->CreateBuffer(&desc, nullptr, readback.GetAddressOf()))) {
            error = "could not create MWSR's statistics readback";
            return false;
        }
    }

    m_Device = device;
    m_Device->GetImmediateContext(m_Context.GetAddressOf());
    m_Width = width;
    m_Height = height;
    m_Light = light;
    return true;
}

uint64_t MwsrStats::dispatch(ID3D11ShaderResourceView* source)
{
    if (!m_Shader || !source) return 0;

    const int timing = m_Timer.begin(m_Device.Get(), m_Context.Get());
    ID3D11ShaderResourceView* views[] = {source};
    ID3D11UnorderedAccessView* cells[] = {m_CellsView.Get()};
    m_Context->CSSetShader(m_Shader.Get(), nullptr, 0);
    m_Context->CSSetShaderResources(0, 1, views);
    m_Context->CSSetUnorderedAccessViews(0, 1, cells, nullptr);
    // A group per cell, the light pass's too (kMwsrStatsSource).
    m_Context->Dispatch(mz::kCellsX, mz::kCellsY, 1);
    // Unbound at once: the capture is about to release its texture, and a view
    // left bound would keep it (see ColorConvert::convert).
    ID3D11ShaderResourceView* noView[] = {nullptr};
    ID3D11UnorderedAccessView* noCells[] = {nullptr};
    m_Context->CSSetShaderResources(0, 1, noView);
    m_Context->CSSetUnorderedAccessViews(0, 1, noCells, nullptr);
    m_Context->CSSetShader(nullptr, nullptr, 0);

    const uint64_t ticket = ++m_Last;
    const int slot = static_cast<int>(ticket % kSlots);
    m_Context->CopyResource(m_Readback[slot].Get(), m_Cells.Get());
    m_Ticket[slot] = ticket;
    m_Timer.end(m_Context.Get(), timing);
    return ticket;
}

bool MwsrStats::read(uint64_t ticket, float* cells, bool wait)
{
    if (!m_Context || ticket == 0 || !cells) return false;
    const int slot = static_cast<int>(ticket % kSlots);
    if (m_Ticket[slot] != ticket) return false; // gone to a later dispatch

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    // DO_NOT_WAIT: a copy the GPU has not reached answers WAS_STILL_DRAWING at
    // once, and the frame goes without its grid.
    if (FAILED(m_Context->Map(m_Readback[slot].Get(), 0, D3D11_MAP_READ,
                              wait ? 0 : D3D11_MAP_FLAG_DO_NOT_WAIT, &mapped)))
        return false;
    std::memcpy(cells, mapped.pData, sizeof(float) * kFloats);
    m_Context->Unmap(m_Readback[slot].Get(), 0);
    return true;
}

} // namespace mw::native::convert
