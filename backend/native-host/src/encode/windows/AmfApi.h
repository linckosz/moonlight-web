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
 */

#pragma once

#include <core/Factory.h>

#include <string>

namespace mw::native::encode {

/// AMD's AMF runtime, loaded from the driver's own DLL.
///
/// Same shape and the same reasoning as NvencApi: `amfrt64.dll` ships with the
/// Radeon driver, so it is resolved at run time and its absence is a normal
/// answer ("this machine has no AMD encoder") rather than a link error that
/// would stop MoonlightWeb from starting on an NVIDIA box.
///
/// Unlike NVENC, AMF checks its own version compatibility internally: AMFInit
/// is given the version the headers were built against and the runtime decides
/// whether it can serve it. So there is no driver-floor check to write here —
/// a runtime too old simply fails AMFInit, with a reason.
class AmfApi
{
public:
    static const AmfApi* instance();

    bool available() const { return m_Available; }
    const std::string& unavailableReason() const { return m_Reason; }

    amf::AMFFactory* factory() const { return m_Factory; }

    /// Human-readable form of an AMF_RESULT, for logs.
    static const char* resultToString(AMF_RESULT result);

private:
    AmfApi();

    amf::AMFFactory* m_Factory = nullptr;
    bool m_Available = false;
    std::string m_Reason;
};

} // namespace mw::native::encode
