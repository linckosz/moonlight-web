/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "VplFrameAllocator.h"

#include "../../core/Log.h"

namespace mw::native::encode {
namespace {

DXGI_FORMAT formatOf(mfxU32 fourCC)
{
    switch (fourCC) {
    case MFX_FOURCC_NV12: return DXGI_FORMAT_NV12;
    case MFX_FOURCC_P010: return DXGI_FORMAT_P010;
    case MFX_FOURCC_AYUV: return DXGI_FORMAT_AYUV;
    case MFX_FOURCC_RGB4: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace

VplFrameAllocator::VplFrameAllocator(ID3D11Device* device)
    : m_Device(device)
{
    m_Callbacks.pthis = this;
    m_Callbacks.Alloc = &VplFrameAllocator::allocCb;
    m_Callbacks.Lock = &VplFrameAllocator::lockCb;
    m_Callbacks.Unlock = &VplFrameAllocator::unlockCb;
    m_Callbacks.GetHDL = &VplFrameAllocator::getHdlCb;
    m_Callbacks.Free = &VplFrameAllocator::freeCb;
}

mfxStatus VplFrameAllocator::alloc(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response)
{
    if (!request || !response) return MFX_ERR_NULL_PTR;
    // System memory is not this pipeline's business: everything it hands over
    // lives on the GPU, and quietly serving a system-memory request would put a
    // copy back in the middle of the frame path.
    if ((request->Type & MFX_MEMTYPE_SYSTEM_MEMORY) != 0) return MFX_ERR_UNSUPPORTED;

    const DXGI_FORMAT format = formatOf(request->Info.FourCC);
    if (format == DXGI_FORMAT_UNKNOWN) return MFX_ERR_UNSUPPORTED;

    const mfxU16 count = request->NumFrameSuggested > 0 ? request->NumFrameSuggested : 1;

    auto pool = std::make_unique<Pool>();
    pool->textures.reserve(count);
    pool->handles.reserve(count);
    pool->mids.reserve(count);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = request->Info.Width;
    desc.Height = request->Info.Height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // What the Media SDK's own allocator asks for. A driver that refuses it
    // still has to take the pair below, which is what our conversion pass
    // already writes into every session.
    desc.BindFlags = D3D11_BIND_DECODER;

    for (mfxU16 i = 0; i < count; ++i) {
        Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
        HRESULT hr = m_Device->CreateTexture2D(&desc, nullptr, texture.GetAddressOf());
        if (FAILED(hr)) {
            D3D11_TEXTURE2D_DESC fallback = desc;
            fallback.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
            hr = m_Device->CreateTexture2D(&fallback, nullptr, texture.ReleaseAndGetAddressOf());
        }
        if (FAILED(hr)) {
            log::warning("[native] oneVPL: could not allocate " + std::to_string(desc.Width) + "x" +
                         std::to_string(desc.Height) + " surface " + std::to_string(i + 1) +
                         " of " + std::to_string(count));
            return MFX_ERR_MEMORY_ALLOC;
        }
        pool->textures.push_back(texture);
    }

    // Filled in a second pass: the vector must have stopped growing before any
    // pointer into it is handed to the runtime.
    for (mfxU16 i = 0; i < count; ++i) {
        mfxHDLPair pair = {};
        pair.first = static_cast<mfxHDL>(pool->textures[i].Get());
        pair.second = nullptr; // subresource 0 — one texture per surface
        pool->handles.push_back(pair);
    }
    for (mfxU16 i = 0; i < count; ++i)
        pool->mids.push_back(static_cast<mfxMemId>(&pool->handles[i]));

    response->mids = pool->mids.data();
    response->NumFrameActual = count;
    m_Pools[response->mids] = std::move(pool);
    return MFX_ERR_NONE;
}

mfxStatus VplFrameAllocator::free(mfxFrameAllocResponse* response)
{
    if (!response || !response->mids) return MFX_ERR_NONE;
    m_Pools.erase(response->mids);
    response->mids = nullptr;
    response->NumFrameActual = 0;
    return MFX_ERR_NONE;
}

mfxStatus MFX_CDECL VplFrameAllocator::allocCb(mfxHDL pthis, mfxFrameAllocRequest* request,
                                               mfxFrameAllocResponse* response)
{
    if (!pthis) return MFX_ERR_NULL_PTR;
    return static_cast<VplFrameAllocator*>(pthis)->alloc(request, response);
}

mfxStatus MFX_CDECL VplFrameAllocator::freeCb(mfxHDL pthis, mfxFrameAllocResponse* response)
{
    if (!pthis) return MFX_ERR_NULL_PTR;
    return static_cast<VplFrameAllocator*>(pthis)->free(response);
}

mfxStatus MFX_CDECL VplFrameAllocator::lockCb(mfxHDL, mfxMemId, mfxFrameData*)
{
    // Nothing in this pipeline reads a surface on the CPU. Saying so plainly
    // beats mapping a staging texture nobody asked for.
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus MFX_CDECL VplFrameAllocator::unlockCb(mfxHDL, mfxMemId, mfxFrameData*)
{
    return MFX_ERR_UNSUPPORTED;
}

mfxStatus MFX_CDECL VplFrameAllocator::getHdlCb(mfxHDL, mfxMemId mid, mfxHDL* handle)
{
    if (!mid || !handle) return MFX_ERR_NULL_PTR;
    // The whole contract, in one line: every MemId in this engine is an
    // mfxHDLPair, whoever allocated the texture behind it.
    *reinterpret_cast<mfxHDLPair*>(handle) = *static_cast<mfxHDLPair*>(mid);
    return MFX_ERR_NONE;
}

} // namespace mw::native::encode
