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

#pragma once

#include <ffnvcodec/nvEncodeAPI.h>

#include <string>

namespace mw::native::encode {

/// The NVENC entry points, loaded from the driver's own DLL.
///
/// NVENC is never linked: `nvEncodeAPI64.dll` ships with the display driver, so
/// it is resolved at runtime and its absence is a normal answer ("this machine
/// has no NVIDIA encoder") rather than a link error that would stop the whole
/// application from starting on an AMD box.
///
/// Nothing from NVIDIA's SDK is redistributed — only the API header is vendored
/// (see third_party/nvenc-headers/README.md).
class NvencApi
{
public:
    /// Load the driver DLL and build the function table.
    ///
    /// Returns false, with a reason, when NVENC is simply not here — which is
    /// most machines — or when the DRIVER IS OLDER than the header this was
    /// built against. That second case matters: NvEncodeAPICreateInstance would
    /// otherwise fail with a bare "invalid version" much later, at session
    /// creation, where it reads as a bug rather than as "update your driver".
    static const NvencApi* instance();

    bool available() const { return m_Available; }
    const std::string& unavailableReason() const { return m_Reason; }

    const NV_ENCODE_API_FUNCTION_LIST& fn() const { return m_Fn; }

    /// Human-readable form of an NVENCSTATUS, for logs.
    static const char* statusToString(NVENCSTATUS status);

private:
    NvencApi();

    NV_ENCODE_API_FUNCTION_LIST m_Fn = {};
    bool m_Available = false;
    std::string m_Reason;
};

} // namespace mw::native::encode
