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

#include "MfApi.h"

#include <mferror.h>

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

std::string MfApi::hresultToString(HRESULT hr)
{
    const char* name = "unexpected result";
    switch (hr) {
    case S_OK: name = "ok"; break;
    case E_FAIL: name = "failed"; break;
    case E_INVALIDARG: name = "invalid argument"; break;
    case E_OUTOFMEMORY: name = "out of memory"; break;
    case E_NOTIMPL: name = "not implemented"; break;
    case E_ACCESSDENIED: name = "access denied"; break;
    case MF_E_INVALIDMEDIATYPE: name = "invalid media type"; break;
    case MF_E_INVALIDTYPE: name = "invalid type"; break;
    case MF_E_TRANSFORM_TYPE_NOT_SET: name = "the transform has no type set"; break;
    case MF_E_UNSUPPORTED_D3D_TYPE: name = "unsupported Direct3D type"; break;
    case MF_E_TRANSFORM_NEED_MORE_INPUT: name = "needs more input"; break;
    case MF_E_TRANSFORM_STREAM_CHANGE: name = "the output type changed"; break;
    case MF_E_ATTRIBUTENOTFOUND: name = "attribute not found"; break;
    case MF_E_NOT_INITIALIZED: name = "not initialized"; break;
    case MF_E_SHUTDOWN: name = "shut down"; break;
    default: break;
    }
    // Unsigned hex, because that is the form every Microsoft page and every
    // event-log entry uses; a signed decimal would have to be converted by hand.
    char code[16] = {};
    ::wsprintfA(code, "0x%08lX", static_cast<unsigned long>(hr));
    return std::string(name) + " (" + code + ")";
}

MfApi::MfApi()
{
    // Never linked — see the header for why an N edition of Windows would
    // otherwise refuse to start MoonlightWeb at all.
    HMODULE dll = ::GetModuleHandleW(L"mfplat.dll");
    if (!dll) dll = ::LoadLibraryW(L"mfplat.dll");
    if (!dll) {
        m_Reason = "Media Foundation is not installed on this machine (an N edition of Windows "
                   "without the Media Feature Pack, or Server without the feature)";
        return;
    }

    std::string missing;
    const bool ok = resolve(dll, "MFStartup", Startup, missing) &&
                    resolve(dll, "MFShutdown", Shutdown, missing) &&
                    resolve(dll, "MFTEnumEx", TEnumEx, missing) &&
                    resolve(dll, "MFCreateMediaType", CreateMediaType, missing) &&
                    resolve(dll, "MFCreateAttributes", CreateAttributes, missing) &&
                    resolve(dll, "MFCreateSample", CreateSample, missing) &&
                    resolve(dll, "MFCreateMemoryBuffer", CreateMemoryBuffer, missing) &&
                    resolve(dll, "MFCreateDXGIDeviceManager", CreateDXGIDeviceManager, missing);
    if (!ok) {
        m_Reason = "mfplat.dll is present but does not export " + missing;
        return;
    }

    m_Available = true;
}

const MfApi* MfApi::instance()
{
    // Loaded once per process, on first use. Deliberately never unloaded: the
    // encoder holds transforms created from it, and a machine that has Media
    // Foundation keeps it for the life of the process anyway.
    static const MfApi api;
    return &api;
}

} // namespace mw::native::encode
