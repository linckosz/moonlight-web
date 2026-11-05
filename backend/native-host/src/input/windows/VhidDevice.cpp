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

#include "input/VirtualHid.h"

#include "core/Log.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#include <wtsapi32.h>

#include <cwchar>
#include <string>
#include <vector>

#include "WinUHid.h"

namespace mw::native::input {

namespace {

/// One OEMName: written unless a name that is not VHF's is already there.
void nameUnder(HKEY root, const wchar_t* path, const std::wstring& name)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(root, path, 0, nullptr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, nullptr, &key,
                        nullptr) != ERROR_SUCCESS)
        return; // not elevated (HKLM): this copy stays as it is
    wchar_t current[256] = {};
    DWORD bytes = sizeof(current) - sizeof(wchar_t);
    const bool has = RegQueryValueExW(key, L"OEMName", nullptr, nullptr,
                                      reinterpret_cast<BYTE*>(current), &bytes) == ERROR_SUCCESS;
    if (!has || current[0] == 0 || wcsstr(current, L"VHF"))
        RegSetValueExW(key, L"OEMName", 0, REG_SZ, reinterpret_cast<const BYTE*>(name.c_str()),
                       static_cast<DWORD>((name.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}

/// DirectInput (joy.cpl, and the games that list wheels through it) names a
/// controller from its OEMName under MediaProperties, keyed by VID and PID.
/// Windows writes one on first sight from the device description, and VHF's
/// is "Virtual HID Framework (VHF) HID device": VHF answers the product
/// string itself and has no way to take ours. So the real product name goes
/// there, unless something else (the wheel's own software, on a host it was
/// once plugged into) already wrote a name that is not VHF's.
///
/// The console user's SID as text, or "" — only a SYSTEM worker can ask
/// (WTSQueryUserToken needs SeTcbPrivilege).
std::wstring consoleUserSid()
{
    const DWORD session = ::WTSGetActiveConsoleSessionId();
    HANDLE token = nullptr;
    if (session == 0xFFFFFFFF || !::WTSQueryUserToken(session, &token)) return {};
    std::wstring out;
    DWORD size = 0;
    ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
    std::vector<unsigned char> user(size);
    LPWSTR sid = nullptr;
    if (size && ::GetTokenInformation(token, TokenUser, user.data(), size, &size) &&
        ::ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid)) {
        out = sid;
        ::LocalFree(sid);
    }
    ::CloseHandle(token);
    return out;
}

/// Twice: the machine's copy, and the user's. Windows keeps one per user,
/// made on the user's first sight of the device, and DirectInput reads that
/// one first — a user who saw the device before the machine's copy was right
/// kept VHF's name (seen 11/10 on the bench host). The worker the service
/// starts runs as SYSTEM, whose HKCU is nobody's: the console user's hive is
/// then reached under HKEY_USERS. An elevated worker (the scheduled task) runs
/// as the user, and HKCU is theirs.
void nameForDirectInput(uint16_t vendorId, uint16_t productId, const std::string& name)
{
    if (name.empty()) return;
    wchar_t path[160];
    swprintf_s(path,
               L"System\\CurrentControlSet\\Control\\MediaProperties\\PrivateProperties\\Joystick"
               L"\\OEM\\VID_%04X&PID_%04X",
               vendorId, productId);
    const int n = MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, nullptr, 0);
    if (n <= 1) return;
    std::wstring wide(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, name.c_str(), -1, wide.data(), n);
    wide.resize(n - 1); // without the terminator the conversion counted
    nameUnder(HKEY_LOCAL_MACHINE, path, wide);
    const std::wstring sid = consoleUserSid();
    if (sid.empty())
        nameUnder(HKEY_CURRENT_USER, path, wide);
    else
        nameUnder(HKEY_USERS, (sid + L"\\" + path).c_str(), wide);
}

/// A device made through the "MoonlightWeb Virtual HID" driver
/// (drivers/vhid), with WinUHid's client library built in. The driver lets
/// administrators and SYSTEM only open its control device: the worker the
/// service launches runs with the user's elevated token, so it can.
class VhidDevice final : public IVirtualHid
{
public:
    ~VhidDevice() override { destroy(); }

    bool create(const VirtualHidIdentity& id, std::string& error) override
    {
        WINUHID_DEVICE_CONFIG c{};
        c.SupportedEvents = static_cast<WINUHID_EVENT_TYPE>(
            WINUHID_EVENT_WRITE_REPORT | WINUHID_EVENT_GET_FEATURE | WINUHID_EVENT_SET_FEATURE);
        c.VendorID = id.vendorId;
        c.ProductID = id.productId;
        c.VersionNumber = id.version;
        c.ReportDescriptorLength = static_cast<USHORT>(id.descriptor.size());
        c.ReportDescriptor = id.descriptor.data();
        // Hardware ids, so the collections enumerate as
        // HID\MOONLIGHTWEB_VHID&VID_046D&PID_C26E&COL01 instead of
        // HID\HID_DEVICE_SYSTEM_VHF&COL01: the device path says whose it is and
        // carries the ids a tool reads from it. The second is the one VHF gives
        // when given none, so the collections keep every id they had. The first
        // names the device and needs its enumerator: without one, no device at
        // all (04/10). VHF puts its HID_DEVICE_SYSTEM_VHF, the id hidvhf.inf
        // installs, before both (bench of 20/10). A device with no vendor id
        // (the host's own mouse, its own keyboard) keeps VHF's.
        wchar_t ids[96] = {};
        if (id.vendorId) {
            const int n = swprintf_s(ids, L"VHF\\MOONLIGHTWEB_VHID&VID_%04X&PID_%04X", id.vendorId,
                                     id.productId);
            swprintf_s(ids + n + 1, std::size(ids) - n - 2, L"VHF\\VID_%04X&PID_%04X", id.vendorId,
                       id.productId);
            c.HardwareIDs = ids; // REG_MULTI_SZ: the array's zeros end both
        }
        // DirectInput's name comes from the registry, written before the device
        // shows up so the first enumeration already reads it.
        nameForDirectInput(id.vendorId, id.productId, id.name);
        m_device = WinUHidCreateDevice(&c);
        if (!m_device) {
            error = "WinUHidCreateDevice failed (error " + std::to_string(::GetLastError()) + ")";
            return false;
        }
        if (!WinUHidStartDevice(m_device, &VhidDevice::onEvent, this)) {
            error = "WinUHidStartDevice failed (error " + std::to_string(::GetLastError()) + ")";
            WinUHidDestroyDevice(m_device);
            m_device = nullptr;
            return false;
        }
        logDriverFixes();
        return true;
    }

    bool input(const uint8_t* report, size_t size) override
    {
        return m_device && WinUHidSubmitInputReport(m_device, report, static_cast<DWORD>(size));
    }

    /// Closes this process's handle: the device goes when no process holds
    /// one any more (HidParking hands a copy to the next stream's).
    void destroy() override
    {
        if (!m_device) return;
        WinUHidStopDevice(m_device);
        WinUHidDestroyDevice(m_device);
        m_device = nullptr;
    }

    uint64_t nativeHandle() const override
    {
        return m_device ? reinterpret_cast<uint64_t>(WinUHidGetHandle(m_device)) : 0;
    }

    void pause() override
    {
        if (m_device) WinUHidStopDevice(m_device);
    }

    void resume() override
    {
        if (m_device) WinUHidResumeDevice(m_device, &VhidDevice::onEvent, this);
    }

    bool servicing() const override { return m_device && WinUHidIsServicingEvents(m_device); }

    /// A device another process made: `handle` is this process's copy, and
    /// closed when it is not one of the driver's devices.
    bool adopt(HANDLE handle, std::string& error)
    {
        m_device = WinUHidAdoptDevice(handle);
        if (m_device) {
            logDriverFixes();
            return true;
        }
        error = "not a MoonlightWeb Virtual HID device (error " + std::to_string(::GetLastError()) +
                ")";
        ::CloseHandle(handle);
        return false;
    }

private:
    /// A driver from before 11/10 loses a request now and then: the client
    /// library asks again every 10 ms with it. One stays loaded until Windows
    /// restarts when an update found a game holding a device.
    void logDriverFixes() const
    {
        if (!(WinUHidGetDriverFixes(m_device) & MOONLIGHTWEB_VHID_FIX_EVENT_WAKEUP))
            log::info("[hid] the driver loaded predates its fix of 11/10: requests are asked for "
                      "every 10 ms");
    }

    // On WinUHid's event thread: every request is completed at once.
    static VOID onEvent(PVOID context, PWINUHID_DEVICE device, PCWINUHID_EVENT ev)
    {
        auto* self = static_cast<VhidDevice*>(context);
        // Completing an event frees it: what is needed after is copied first
        // (reading it after crashed a worker on the bench, 11/10).
        const WINUHID_EVENT_TYPE type = ev->Type;
        const uint8_t reportId = ev->ReportId;
        switch (type) {
        case WINUHID_EVENT_WRITE_REPORT:
        case WINUHID_EVENT_SET_FEATURE: {
            std::vector<uint8_t> data(ev->Write.Data, ev->Write.Data + ev->Write.DataLength);
            WinUHidCompleteWriteEvent(device, ev, TRUE);
            if (self->m_onRequest)
                self->m_onRequest(type == WINUHID_EVENT_WRITE_REPORT ? Request::Output
                                                                     : Request::SetFeature,
                                  reportId, data);
            break;
        }
        case WINUHID_EVENT_GET_FEATURE: {
            // VHF hands the report buffer id byte first, as HidD_GetFeature does.
            std::vector<uint8_t> answer;
            if (self->m_onGetFeature) answer = self->m_onGetFeature(reportId);
            WinUHidCompleteReadEvent(device, ev, answer.empty() ? nullptr : answer.data(),
                                     static_cast<DWORD>(answer.size()));
            if (self->m_onRequest) self->m_onRequest(Request::GetFeature, reportId, {});
            break;
        }
        default:
            // Never asked for (SupportedEvents): failed, or the game would wait.
            log::warning("[hid] unexpected request type " + std::to_string(type) +
                         " from the driver, report " + std::to_string(reportId));
            if (WINUHID_EVENT_TYPE_IS_READ(type))
                WinUHidCompleteReadEvent(device, ev, nullptr, 0);
            else
                WinUHidCompleteWriteEvent(device, ev, FALSE);
            break;
        }
    }

    PWINUHID_DEVICE m_device = nullptr;
};

} // namespace

std::unique_ptr<IVirtualHid> makeVirtualHid(bool)
{
    return std::make_unique<VhidDevice>();
}

std::unique_ptr<IVirtualHid> adoptVirtualHid(uint64_t handle, std::string& error)
{
    auto device = std::make_unique<VhidDevice>();
    if (!device->adopt(reinterpret_cast<HANDLE>(handle), error)) return nullptr;
    return device;
}

bool virtualHidForceFeedback()
{
    return true; // pid.dll drives the PID block the device gets
}

std::string virtualHidUnavailableReason()
{
    if (WinUHidGetDriverInterfaceVersion() == 0)
        return "the MoonlightWeb Virtual HID driver is not installed (error " +
               std::to_string(::GetLastError()) + ")";
    return {};
}

VirtualHidSupport virtualHidSupport()
{
    if (WinUHidGetDriverInterfaceVersion() != 0) return VirtualHidSupport::Ready;
    // The control device admits administrators, SYSTEM and UMDF drivers only:
    // refused, it is there. The stream's worker, elevated, opens it.
    return ::GetLastError() == ERROR_ACCESS_DENIED ? VirtualHidSupport::Ready
                                                   : VirtualHidSupport::DriverMissing;
}

} // namespace mw::native::input
