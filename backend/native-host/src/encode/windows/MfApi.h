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

#include <windows.h>

#include <mfapi.h>
#include <mfidl.h>
#include <mftransform.h>

#include <string>

namespace mw::native::encode {

/// Media Foundation, loaded at run time like every other encoder runtime here.
///
/// ── Why this is not simply linked ──────────────────────────────────────────
///
/// `mfplat.dll` looks like a core Windows component, and on an ordinary install
/// it is. It is absent on the **N and KN editions** of Windows until someone
/// installs the Media Feature Pack, and it can be absent on Server without the
/// Media Foundation feature. Linking it would make MoonlightWeb fail to *start*
/// on those machines — the loader resolves imports before any of our code runs —
/// which would trade a missing fallback encoder for a dead application.
///
/// So the same rule as NVENC, AMF and oneVPL applies, for a different reason:
/// resolved at run time, and its absence is an ordinary answer ("no Media
/// Foundation encoder here") rather than a crash.
///
/// The GUIDs are a separate matter and are linked normally (`mfuuid.lib`): that
/// library is pure constant data with no DLL behind it, so it cannot fail to
/// load.
class MfApi
{
public:
    static const MfApi* instance();

    bool available() const { return m_Available; }
    const std::string& unavailableReason() const { return m_Reason; }

    // ── mfplat.dll ──────────────────────────────────────────────────────────
    HRESULT(STDAPICALLTYPE* Startup)(ULONG, DWORD) = nullptr;
    HRESULT(STDAPICALLTYPE* Shutdown)() = nullptr;
    HRESULT(STDAPICALLTYPE* TEnumEx)
    (GUID, UINT32, const MFT_REGISTER_TYPE_INFO*, const MFT_REGISTER_TYPE_INFO*, IMFActivate***,
     UINT32*) = nullptr;
    HRESULT(STDAPICALLTYPE* CreateMediaType)(IMFMediaType**) = nullptr;
    HRESULT(STDAPICALLTYPE* CreateAttributes)(IMFAttributes**, UINT32) = nullptr;
    HRESULT(STDAPICALLTYPE* CreateSample)(IMFSample**) = nullptr;
    HRESULT(STDAPICALLTYPE* CreateMemoryBuffer)(DWORD, IMFMediaBuffer**) = nullptr;
    HRESULT(STDAPICALLTYPE* CreateDXGIDeviceManager)(UINT*, IMFDXGIDeviceManager**) = nullptr;

    /// Human-readable form of an HRESULT, for logs. The numeric code is always
    /// appended: Media Foundation returns codes no switch here will cover, and a
    /// bare "failed" is a dead end for anyone reading a bug report.
    static std::string hresultToString(HRESULT hr);

private:
    MfApi();

    bool m_Available = false;
    std::string m_Reason;
};

} // namespace mw::native::encode
