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

#include "NvencApi.h"

#include "../../core/Log.h"

#include <windows.h>

namespace mw::native::encode {
namespace {

using GetMaxVersionFn = NVENCSTATUS(NVENCAPI*)(uint32_t*);
using CreateInstanceFn = NVENCSTATUS(NVENCAPI*)(NV_ENCODE_API_FUNCTION_LIST*);

} // namespace

const char* NvencApi::statusToString(NVENCSTATUS status)
{
    switch (status) {
    case NV_ENC_SUCCESS: return "success";
    case NV_ENC_ERR_NO_ENCODE_DEVICE: return "no encode device";
    case NV_ENC_ERR_UNSUPPORTED_DEVICE: return "unsupported device";
    case NV_ENC_ERR_INVALID_ENCODERDEVICE: return "invalid encoder device";
    case NV_ENC_ERR_INVALID_DEVICE: return "invalid device";
    case NV_ENC_ERR_DEVICE_NOT_EXIST: return "device no longer exists";
    case NV_ENC_ERR_INVALID_PTR: return "invalid pointer";
    case NV_ENC_ERR_INVALID_PARAM: return "invalid parameter";
    case NV_ENC_ERR_INVALID_VERSION: return "version mismatch (driver too old)";
    case NV_ENC_ERR_OUT_OF_MEMORY: return "out of memory";
    case NV_ENC_ERR_ENCODER_NOT_INITIALIZED: return "encoder not initialized";
    case NV_ENC_ERR_UNSUPPORTED_PARAM: return "unsupported parameter";
    case NV_ENC_ERR_GENERIC: return "generic error";
    default: break;
    }
    return "unknown error";
}

NvencApi::NvencApi()
{
    m_Fn.version = NV_ENCODE_API_FUNCTION_LIST_VER;

    // Never linked: the DLL belongs to the display driver, and its absence is
    // the normal state of every machine without an NVIDIA GPU.
    HMODULE dll = ::GetModuleHandleW(L"nvEncodeAPI64.dll");
    if (!dll) dll = ::LoadLibraryW(L"nvEncodeAPI64.dll");
    if (!dll) {
        m_Reason = "no NVIDIA encoder runtime on this machine";
        return;
    }

    const auto getMaxVersion = reinterpret_cast<GetMaxVersionFn>(
        reinterpret_cast<void*>(::GetProcAddress(dll, "NvEncodeAPIGetMaxSupportedVersion")));
    const auto createInstance = reinterpret_cast<CreateInstanceFn>(
        reinterpret_cast<void*>(::GetProcAddress(dll, "NvEncodeAPICreateInstance")));
    if (!getMaxVersion || !createInstance) {
        m_Reason = "the NVIDIA encoder runtime is missing its entry points";
        return;
    }

    // Ask the DRIVER what it supports before handing it a header version it may
    // not know. Skipping this check does not avoid the problem — it just moves
    // the failure to session creation, where "invalid version" reads like a bug
    // in MoonlightWeb rather than an out-of-date driver.
    uint32_t driverVersion = 0;
    NVENCSTATUS status = getMaxVersion(&driverVersion);
    if (status != NV_ENC_SUCCESS) {
        m_Reason =
            std::string("could not read the NVENC driver version: ") + statusToString(status);
        return;
    }

    const uint32_t headerVersion = (NVENCAPI_MAJOR_VERSION << 4) | NVENCAPI_MINOR_VERSION;
    if (driverVersion < headerVersion) {
        m_Reason = "the NVIDIA driver is older than the encoder API this build needs — "
                   "updating the driver enables native streaming";
        log::info("[native] NVENC driver version " + std::to_string(driverVersion) +
                  " < required " + std::to_string(headerVersion));
        return;
    }

    status = createInstance(&m_Fn);
    if (status != NV_ENC_SUCCESS) {
        m_Reason = std::string("could not open the NVENC API: ") + statusToString(status);
        return;
    }

    m_Available = true;
}

const NvencApi* NvencApi::instance()
{
    // Loaded once per process. The driver does not change under a running
    // process, and re-resolving the entry points on every probe would be work
    // with no possible new answer.
    static const NvencApi api;
    return &api;
}

} // namespace mw::native::encode
