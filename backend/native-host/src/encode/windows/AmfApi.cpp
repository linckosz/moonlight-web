/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "AmfApi.h"

#include "../../core/Log.h"

#include <windows.h>

#include <core/Version.h>

namespace mw::native::encode {

const char* AmfApi::resultToString(AMF_RESULT result)
{
    switch (result) {
    case AMF_OK: return "ok";
    case AMF_FAIL: return "failed";
    case AMF_NOT_SUPPORTED: return "not supported";
    case AMF_NOT_INITIALIZED: return "not initialized";
    case AMF_INVALID_ARG: return "invalid argument";
    case AMF_OUT_OF_MEMORY: return "out of memory";
    case AMF_NO_DEVICE: return "no device";
    case AMF_INVALID_FORMAT: return "invalid format";
    case AMF_ENCODER_NOT_PRESENT: return "no encoder on this GPU";
    case AMF_INPUT_FULL: return "encoder input full";
    case AMF_REPEAT: return "output not ready yet";
    case AMF_EOF: return "end of stream";
    default: break;
    }
    return "unknown error";
}

AmfApi::AmfApi()
{
    // Never linked: the DLL belongs to the Radeon driver, and its absence is
    // the normal state of every machine without an AMD GPU.
    HMODULE dll = ::GetModuleHandleA(AMF_DLL_NAMEA);
    if (!dll) dll = ::LoadLibraryA(AMF_DLL_NAMEA);
    if (!dll) {
        m_Reason = "no AMD encoder runtime on this machine";
        return;
    }

    const auto init = reinterpret_cast<AMFInit_Fn>(
        reinterpret_cast<void*>(::GetProcAddress(dll, AMF_INIT_FUNCTION_NAME)));
    if (!init) {
        m_Reason = "the AMD encoder runtime is missing its entry point";
        return;
    }

    // AMF checks compatibility itself: it is handed the version these headers
    // were built against and decides whether it can serve it. A runtime too old
    // fails here with a reason, which is why — unlike NVENC — no driver-floor
    // comparison is written by hand.
    const AMF_RESULT result = init(AMF_FULL_VERSION, &m_Factory);
    if (result != AMF_OK || !m_Factory) {
        m_Reason = std::string("could not open the AMF runtime: ") + resultToString(result);
        m_Factory = nullptr;
        return;
    }

    m_Available = true;
}

const AmfApi* AmfApi::instance()
{
    // Loaded once per process: the driver does not change under a running
    // process, and re-resolving would be work with no possible new answer.
    static const AmfApi api;
    return &api;
}

} // namespace mw::native::encode
