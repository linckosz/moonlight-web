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

std::string VplApi::statusToString(mfxStatus status)
{
    const char* name = "unexpected status";
    switch (status) {
    case MFX_ERR_NONE: name = "ok"; break;
    case MFX_ERR_UNKNOWN: name = "unknown error"; break;
    case MFX_ERR_NULL_PTR: name = "null pointer"; break;
    case MFX_ERR_UNSUPPORTED: name = "unsupported"; break;
    case MFX_ERR_MEMORY_ALLOC: name = "allocation failed"; break;
    case MFX_ERR_NOT_ENOUGH_BUFFER: name = "buffer too small"; break;
    case MFX_ERR_INVALID_HANDLE: name = "invalid handle"; break;
    case MFX_ERR_LOCK_MEMORY: name = "could not lock the memory block"; break;
    case MFX_ERR_DEVICE_FAILED: name = "device failed"; break;
    case MFX_ERR_NOT_INITIALIZED: name = "not initialized"; break;
    case MFX_ERR_NOT_FOUND: name = "not found"; break;
    case MFX_ERR_MORE_DATA: name = "needs more input"; break;
    case MFX_ERR_MORE_SURFACE: name = "needs another output surface"; break;
    case MFX_ERR_ABORTED: name = "aborted"; break;
    case MFX_ERR_DEVICE_LOST: name = "device lost"; break;
    case MFX_ERR_INCOMPATIBLE_VIDEO_PARAM: name = "incompatible video parameters"; break;
    case MFX_ERR_INVALID_VIDEO_PARAM: name = "invalid video parameters"; break;
    case MFX_ERR_UNDEFINED_BEHAVIOR: name = "undefined behaviour"; break;
    case MFX_ERR_NOT_IMPLEMENTED: name = "not implemented"; break;
    case MFX_ERR_RESOURCE_MAPPED: name = "the resource is already mapped"; break;
    case MFX_ERR_GPU_HANG: name = "GPU hang"; break;
    case MFX_ERR_REALLOC_SURFACE: name = "a bigger surface is required"; break;
    case MFX_WRN_IN_EXECUTION: name = "still executing"; break;
    case MFX_WRN_DEVICE_BUSY: name = "device busy"; break;
    case MFX_WRN_VIDEO_PARAM_CHANGED: name = "video parameters adjusted"; break;
    case MFX_WRN_PARTIAL_ACCELERATION: name = "partly software"; break;
    case MFX_WRN_INCOMPATIBLE_VIDEO_PARAM: name = "video parameters partly unsupported"; break;
    case MFX_WRN_VALUE_NOT_CHANGED: name = "value not changed"; break;
    case MFX_WRN_OUT_OF_RANGE: name = "value out of range"; break;
    default: break;
    }
    return std::string(name) + " (" + std::to_string(static_cast<int>(status)) + ")";
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
                    resolve(dll, "MFXVideoCORE_GetHandle", GetHandle, missing) &
                    resolve(dll, "MFXVideoCORE_SetFrameAllocator", SetFrameAllocator, missing) &
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
