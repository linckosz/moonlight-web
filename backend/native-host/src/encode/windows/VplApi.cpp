/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "VplApi.h"

#include "../../core/Log.h"

#include <windows.h>

namespace mw::native::encode {
namespace {

/// Resolve one entry point, recording the first name that is missing.
template <typename Fn> bool resolve(HMODULE dll, const char* name, Fn& out, std::string& missing)
{
    out = reinterpret_cast<Fn>(reinterpret_cast<void*>(::GetProcAddress(dll, name)));
    if (out) return true;
    if (missing.empty()) missing = name;
    return false;
}

} // namespace

const char* VplApi::statusToString(mfxStatus status)
{
    switch (status) {
    case MFX_ERR_NONE: return "ok";
    case MFX_ERR_UNKNOWN: return "unknown error";
    case MFX_ERR_NULL_PTR: return "null pointer";
    case MFX_ERR_UNSUPPORTED: return "unsupported";
    case MFX_ERR_MEMORY_ALLOC: return "allocation failed";
    case MFX_ERR_NOT_ENOUGH_BUFFER: return "buffer too small";
    case MFX_ERR_INVALID_HANDLE: return "invalid handle";
    case MFX_ERR_DEVICE_FAILED: return "device failed";
    case MFX_ERR_NOT_INITIALIZED: return "not initialized";
    case MFX_ERR_NOT_FOUND: return "not found";
    case MFX_ERR_MORE_DATA: return "needs more input";
    case MFX_ERR_DEVICE_LOST: return "device lost";
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: return "incompatible video parameters";
    case MFX_ERR_INVALID_VIDEO_PARAM: return "invalid video parameters";
    case MFX_WRN_DEVICE_BUSY: return "device busy";
    case MFX_WRN_VIDEO_PARAM_CHANGED: return "video parameters adjusted";
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM: return "video parameters partly unsupported";
    default: break;
    }
    return "unexpected status";
}

VplApi::VplApi()
{
    // Never linked: the dispatcher belongs to the Intel driver, and its absence
    // is the normal state of every machine without an Intel GPU.
    HMODULE dll = ::GetModuleHandleW(L"libvpl.dll");
    if (!dll) dll = ::LoadLibraryW(L"libvpl.dll");
    if (!dll) {
        m_Reason = "no Intel oneVPL runtime on this machine";
        return;
    }

    std::string missing;
    const bool ok = resolve(dll, "MFXLoad", Load, missing) &
                    resolve(dll, "MFXUnload", Unload, missing) &
                    resolve(dll, "MFXCreateConfig", CreateConfig, missing) &
                    resolve(dll, "MFXSetConfigFilterProperty", SetConfigFilterProperty, missing) &
                    resolve(dll, "MFXCreateSession", CreateSession, missing) &
                    resolve(dll, "MFXClose", Close, missing) &
                    resolve(dll, "MFXVideoCORE_SetHandle", SetHandle, missing) &
                    resolve(dll, "MFXVideoCORE_SyncOperation", SyncOperation, missing) &
                    resolve(dll, "MFXVideoENCODE_Query", EncodeQuery, missing) &
                    resolve(dll, "MFXVideoENCODE_Init", EncodeInit, missing) &
                    resolve(dll, "MFXVideoENCODE_Close", EncodeClose, missing) &
                    resolve(dll, "MFXVideoENCODE_EncodeFrameAsync", EncodeFrameAsync, missing) &
                    resolve(dll, "MFXVideoENCODE_GetVideoParam", EncodeGetVideoParam, missing) &
                    resolve(dll, "MFXVideoENCODE_Reset", EncodeReset, missing);

    if (!ok) {
        // Naming the first missing symbol turns "it did not work" into
        // something a bug report can act on — a partial or shim dispatcher is
        // otherwise indistinguishable from an absent one.
        m_Reason = "the Intel oneVPL runtime is missing " + missing;
        return;
    }

    m_Available = true;
}

const VplApi* VplApi::instance()
{
    // Loaded once per process: the driver does not change under a running
    // process, and re-resolving would be work with no possible new answer.
    static const VplApi api;
    return &api;
}

} // namespace mw::native::encode
