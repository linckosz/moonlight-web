/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#pragma once

#include "VplApi.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <map>
#include <memory>
#include <vector>

namespace mw::native::encode {

/// The external frame allocator every oneVPL session in this engine needs.
///
/// ── Why it is not optional ─────────────────────────────────────────────────
///
/// With `MFX_IOPATTERN_IN_VIDEO_MEMORY` a surface carries no pixels: it carries
/// a `Data.MemId`, and the runtime turns that into a real texture by calling the
/// session's allocator. With no allocator registered, the runtime uses its own —
/// and hands it OUR MemId, which it then reads as one of its internal records.
/// That is not an error path: it is a wild pointer. On the first Intel machine
/// this engine ever ran on, the very first encoded frame killed the process with
/// a stack-corruption fail-fast (0xC0000409) and no log line to say why.
///
/// So the deal is: a MemId in this pipeline is always a pointer to an
/// `mfxHDLPair` — {ID3D11Texture2D*, subresource index} — and GetHDL is the
/// three lines that say so. The encoder's per-frame surface points at a pair on
/// its own stack; the surfaces the encoder allocates for itself (reconstructed
/// frames, which the caller never sees) point at pairs owned here.
class VplFrameAllocator
{
public:
    explicit VplFrameAllocator(ID3D11Device* device);

    VplFrameAllocator(const VplFrameAllocator&) = delete;
    VplFrameAllocator& operator=(const VplFrameAllocator&) = delete;

    /// The callback block to hand to MFXVideoCORE_SetFrameAllocator. Valid for
    /// as long as this object lives, which must be longer than the session.
    mfxFrameAllocator* callbacks() { return &m_Callbacks; }

private:
    /// One Alloc call's worth of textures, kept alive until the matching Free.
    struct Pool
    {
        std::vector<Microsoft::WRL::ComPtr<ID3D11Texture2D>> textures;
        /// Stable storage: the mids handed back point straight at these.
        std::vector<mfxHDLPair> handles;
        std::vector<mfxMemId> mids;
    };

    static mfxStatus MFX_CDECL allocCb(mfxHDL pthis, mfxFrameAllocRequest* request,
                                       mfxFrameAllocResponse* response);
    static mfxStatus MFX_CDECL lockCb(mfxHDL pthis, mfxMemId mid, mfxFrameData* data);
    static mfxStatus MFX_CDECL unlockCb(mfxHDL pthis, mfxMemId mid, mfxFrameData* data);
    static mfxStatus MFX_CDECL getHdlCb(mfxHDL pthis, mfxMemId mid, mfxHDL* handle);
    static mfxStatus MFX_CDECL freeCb(mfxHDL pthis, mfxFrameAllocResponse* response);

    mfxStatus alloc(mfxFrameAllocRequest* request, mfxFrameAllocResponse* response);
    mfxStatus free(mfxFrameAllocResponse* response);

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    mfxFrameAllocator m_Callbacks = {};
    /// Keyed by the mids array handed out, which is what Free names.
    std::map<mfxMemId*, std::unique_ptr<Pool>> m_Pools;
};

} // namespace mw::native::encode
