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

#include <vpl/mfxdispatcher.h>
#include <vpl/mfxvideo.h>

#include <string>

namespace mw::native::encode {

/// Intel's oneVPL dispatcher, loaded from the driver's own DLL.
///
/// Same shape and the same reasoning as NvencApi and AmfApi: `libvpl.dll` ships
/// with the Intel graphics driver, so it is resolved at run time and its
/// absence is a normal answer ("this machine has no Intel encoder") rather than
/// a link error that would stop MoonlightWeb from starting on an AMD box.
///
/// ── Only the 2.x dispatcher ────────────────────────────────────────────────
///
/// Intel ships two runtimes: oneVPL (`libvpl.dll`, entered through MFXLoad) and
/// the legacy Media SDK (`libmfxhw64.dll`, entered through MFXInit). Only the
/// first is implemented — see third_party/vpl-headers/README.md for why. A
/// machine carrying only the legacy runtime reports no Intel encoder and falls
/// back like any other unsupported configuration.
class VplApi
{
public:
    static const VplApi* instance();

    bool available() const { return m_Available; }
    const std::string& unavailableReason() const { return m_Reason; }

    /// Human-readable form of an mfxStatus, for logs.
    static const char* statusToString(mfxStatus status);

    // ── The dispatcher's entry points ───────────────────────────────────────
    mfxLoader(MFX_CDECL* Load)() = nullptr;
    void(MFX_CDECL* Unload)(mfxLoader) = nullptr;
    mfxConfig(MFX_CDECL* CreateConfig)(mfxLoader) = nullptr;
    mfxStatus(MFX_CDECL* SetConfigFilterProperty)(mfxConfig, const mfxU8*, mfxVariant) = nullptr;
    mfxStatus(MFX_CDECL* CreateSession)(mfxLoader, mfxU32, mfxSession*) = nullptr;
    mfxStatus(MFX_CDECL* Close)(mfxSession) = nullptr;

    // ── Session functions ───────────────────────────────────────────────────
    mfxStatus(MFX_CDECL* SetHandle)(mfxSession, mfxHandleType, mfxHDL) = nullptr;
    mfxStatus(MFX_CDECL* SyncOperation)(mfxSession, mfxSyncPoint, mfxU32) = nullptr;
    mfxStatus(MFX_CDECL* EncodeQuery)(mfxSession, mfxVideoParam*, mfxVideoParam*) = nullptr;
    mfxStatus(MFX_CDECL* EncodeInit)(mfxSession, mfxVideoParam*) = nullptr;
    mfxStatus(MFX_CDECL* EncodeClose)(mfxSession) = nullptr;
    mfxStatus(MFX_CDECL* EncodeFrameAsync)(mfxSession, mfxEncodeCtrl*, mfxFrameSurface1*,
                                           mfxBitstream*, mfxSyncPoint*) = nullptr;
    mfxStatus(MFX_CDECL* EncodeGetVideoParam)(mfxSession, mfxVideoParam*) = nullptr;
    mfxStatus(MFX_CDECL* EncodeReset)(mfxSession, mfxVideoParam*) = nullptr;

private:
    VplApi();

    bool m_Available = false;
    std::string m_Reason;
};

} // namespace mw::native::encode
