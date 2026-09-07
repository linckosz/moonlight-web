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

#include "HostMute.h"

#include "../../core/Log.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
// Defines the PKEY_* constants this file reads (declared extern otherwise).
// Must precede mmdeviceapi.h; selectany, so a second definer links fine.
#include <initguid.h>
// Order matters: the PKEY header expands to nothing sensible unless the
// PROPERTYKEY machinery mmdeviceapi.h drags in came first.
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <vector>

namespace mw::native::audio {

namespace {

using Microsoft::WRL::ComPtr;

// The interface every "switch the default output" tool has relied on since
// Vista — Sunshine's audio.cpp, SoundSwitch, AudioSwitcher… Unpublished by
// Microsoft, unchanged since; the vtable below is the one they all declare.
// clang-format off
MIDL_INTERFACE("f8679f50-850a-41cf-9c72-430f290290c8")
IPolicyConfig : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, struct DeviceShareMode*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR deviceId, ERole role) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
// clang-format on
// {870af99c-171d-4f9e-af0d-e63df40c2bc9}
const CLSID kPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1) WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring lowered(std::wstring s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return std::towlower(c); });
    return s;
}

/// Playback devices that drive no speaker. Substrings of the endpoint's
/// friendly name or of its adapter's, matched case-insensitively. Each is
/// what the product names itself in the Sound panel.
const wchar_t* const kVirtualSinks[] = {
    L"steam streaming speakers", // Steam Remote Play
    L"cable input",              // VB-Audio Virtual Cable
    L"voicemeeter",              // VB-Audio VoiceMeeter (all inputs)
    L"virtual audio cable",      // Eugene Muzychenko's VAC ("Line 1 (Virtual Audio Cable)")
    L"virtual desktop audio",    // Virtual Desktop
};

struct Endpoint
{
    ComPtr<IMMDevice> device;
    std::wstring id;
    std::wstring name;    // friendly name, as the Sound panel shows it
    std::wstring adapter; // interface (adapter) name
};

std::wstring readName(IMMDevice* device, const PROPERTYKEY& key)
{
    ComPtr<IPropertyStore> store;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &store))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring out;
    if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal)
        out = value.pwszVal;
    PropVariantClear(&value);
    return out;
}

bool describe(IMMDevice* device, Endpoint& out)
{
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || !id) return false;
    out.device = device;
    out.id = id;
    CoTaskMemFree(id);
    out.name = readName(device, PKEY_Device_FriendlyName);
    out.adapter = readName(device, PKEY_DeviceInterface_FriendlyName);
    return true;
}

bool isVirtualSink(const Endpoint& e)
{
    const std::wstring name = lowered(e.name), adapter = lowered(e.adapter);
    for (const wchar_t* needle : kVirtualSinks)
        if (name.find(needle) != std::wstring::npos || adapter.find(needle) != std::wstring::npos)
            return true;
    return false;
}

/// The default output, the virtual sink if any, and whether the default mutes
/// in hardware. False only when there is no playback device at all.
struct Plan
{
    Endpoint def;
    bool defaultMutesInHardware = false;
    bool defaultIsVirtual = false;
    Endpoint sink; // empty id when none
};

bool survey(IMMDeviceEnumerator* enumerator, Plan& plan, std::string& why)
{
    ComPtr<IMMDevice> device;
    HRESULT hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) {
        why = "no default playback device";
        return false;
    }
    if (!describe(device.Get(), plan.def)) {
        why = "the default playback device has no id";
        return false;
    }
    plan.defaultIsVirtual = isVirtualSink(plan.def);

    ComPtr<IAudioEndpointVolume> volume;
    if (SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume))) {
        DWORD support = 0;
        if (SUCCEEDED(volume->QueryHardwareSupport(&support)))
            plan.defaultMutesInHardware = (support & ENDPOINT_HARDWARE_SUPPORT_MUTE) != 0;
    }

    ComPtr<IMMDeviceCollection> all;
    if (SUCCEEDED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &all))) {
        UINT count = 0;
        all->GetCount(&count);
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> candidate;
            if (FAILED(all->Item(i, &candidate))) continue;
            Endpoint e;
            if (!describe(candidate.Get(), e) || e.id == plan.def.id) continue;
            if (isVirtualSink(e)) {
                plan.sink = e;
                break;
            }
        }
    }
    return true;
}

std::string quoted(const std::wstring& name)
{
    return "\"" + narrow(name) + "\"";
}

bool setDefault(const std::wstring& id, std::string& error)
{
    ComPtr<IPolicyConfig> policy;
    HRESULT hr = CoCreateInstance(kPolicyConfigClient, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&policy));
    if (FAILED(hr)) {
        error = "IPolicyConfig unavailable on this Windows";
        return false;
    }
    // Console (games, system sounds) and multimedia (players): what a game's
    // sound reaches. Communications is left where the user put it — a call
    // in progress has no business moving to a device nobody hears.
    for (ERole role : {eConsole, eMultimedia}) {
        hr = policy->SetDefaultEndpoint(id.c_str(), role);
        if (FAILED(hr)) {
            error = "SetDefaultEndpoint refused";
            return false;
        }
    }
    return true;
}

} // namespace

const char* toString(HostMute::Strategy s)
{
    switch (s) {
    case HostMute::Strategy::HardwareMute: return "hardware mute";
    case HostMute::Strategy::VirtualSink: return "virtual output";
    case HostMute::Strategy::None: break;
    }
    return "none";
}

HostMute::~HostMute()
{
    release();
}

HostMute::Strategy HostMute::available(std::string& why)
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coOwned = SUCCEEDED(coInit);
    Strategy result = Strategy::None;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        why = "no audio service";
    } else {
        Plan plan;
        if (survey(enumerator.Get(), plan, why)) {
            if (plan.defaultIsVirtual) {
                result = Strategy::VirtualSink;
                why = quoted(plan.def.name) + " drives no speaker already";
            } else if (plan.defaultMutesInHardware) {
                result = Strategy::HardwareMute;
                why = quoted(plan.def.name) + " mutes in hardware";
            } else if (!plan.sink.id.empty()) {
                result = Strategy::VirtualSink;
                why = quoted(plan.def.name) + " mutes in software; " + quoted(plan.sink.name) +
                      " is available";
            } else {
                why = quoted(plan.def.name) +
                      " mutes in software and there is no virtual output to route to";
            }
        }
    }
    if (coOwned) CoUninitialize();
    return result;
}

HostMute::Strategy HostMute::engage(std::string& how)
{
    release();
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    m_CoOwned = SUCCEEDED(coInit);

    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator)))) {
        how = "the host keeps hearing its audio: no audio service";
        return m_Strategy;
    }
    Plan plan;
    std::string why;
    if (!survey(enumerator.Get(), plan, why)) {
        how = "the host keeps hearing its audio: " + why;
        return m_Strategy;
    }

    if (plan.defaultIsVirtual) {
        // Routed to a device without speakers by the user already: nothing to
        // do, and nothing to undo.
        m_Strategy = Strategy::VirtualSink;
        how =
            "the default output " + quoted(plan.def.name) + " drives no speaker — nothing to mute";
        return m_Strategy;
    }

    if (plan.defaultMutesInHardware) {
        ComPtr<IAudioEndpointVolume> volume;
        if (SUCCEEDED(plan.def.device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                                &volume))) {
            BOOL muted = FALSE;
            volume->GetMute(&muted);
            if (muted) {
                m_Strategy = Strategy::HardwareMute;
                how = quoted(plan.def.name) + " is already muted — left as it is";
                return m_Strategy;
            }
            if (SUCCEEDED(volume->SetMute(TRUE, nullptr))) {
                m_Strategy = Strategy::HardwareMute;
                m_RestoreId = plan.def.id;
                how = "speakers muted on " + quoted(plan.def.name) +
                      " (hardware mute — the capture keeps hearing the mix)";
                return m_Strategy;
            }
        }
        // Fall through: a device that says hardware and refuses is treated
        // like one without.
    }

    if (!plan.sink.id.empty()) {
        std::string error;
        if (setDefault(plan.sink.id, error)) {
            m_Strategy = Strategy::VirtualSink;
            m_RestoreId = plan.def.id;
            m_SinkId = plan.sink.id;
            how = "default output moved to " + quoted(plan.sink.name) + " for the session (" +
                  quoted(plan.def.name) + " mutes in software)";
            return m_Strategy;
        }
        how = "the host keeps hearing its audio: " + quoted(plan.def.name) +
              " mutes in software, and routing to " + quoted(plan.sink.name) + " failed (" + error +
              ")";
        return m_Strategy;
    }

    how = "the host keeps hearing its audio: " + quoted(plan.def.name) +
          " mutes in software (the capture would go quiet too) and there is no virtual output "
          "to route to";
    return m_Strategy;
}

void HostMute::release()
{
    if (m_Strategy != Strategy::None && !m_RestoreId.empty()) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(&enumerator)))) {
            if (m_Strategy == Strategy::HardwareMute) {
                ComPtr<IMMDevice> device;
                ComPtr<IAudioEndpointVolume> volume;
                if (SUCCEEDED(enumerator->GetDevice(m_RestoreId.c_str(), &device)) &&
                    SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                                               &volume))) {
                    BOOL muted = FALSE;
                    volume->GetMute(&muted);
                    // Still muted = still ours to lift. Unmuted meanwhile = the
                    // user's doing, already what they want.
                    if (muted) volume->SetMute(FALSE, nullptr);
                }
            } else if (m_Strategy == Strategy::VirtualSink) {
                ComPtr<IMMDevice> current;
                Endpoint now;
                if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &current)) &&
                    describe(current.Get(), now) && now.id == m_SinkId) {
                    std::string error;
                    if (!setDefault(m_RestoreId, error))
                        log::warning("[native] audio: could not put the default output back (" +
                                     error + ") — pick it again in the Sound panel");
                }
            }
        }
    }
    m_Strategy = Strategy::None;
    m_RestoreId.clear();
    m_SinkId.clear();
    if (m_CoOwned) {
        CoUninitialize();
        m_CoOwned = false;
    }
}

} // namespace mw::native::audio
