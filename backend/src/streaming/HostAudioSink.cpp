/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include "HostAudioSink.h"

#include <QDebug>

#ifdef Q_OS_WIN

#include <QString>

#include <windows.h>
#include <mmdeviceapi.h>
#include <endpointvolume.h>
#include <propsys.h>
#include <propidl.h>

namespace {

// PKEY_Device_FriendlyName, defined locally to avoid the INITGUID include
// order dance of <functiondiscoverykeys_devpkey.h>.
const PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

/// Release-on-scope-exit wrapper for the few COM interfaces used here.
template <typename T> struct ComPtr
{
    T* p = nullptr;
    ~ComPtr()
    {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() { return p; }
    explicit operator bool() const { return p != nullptr; }
};

} // namespace

void HostAudioSink::ensureFullVolume()
{
    // COM may already be initialized on this thread (Qt/OLE); a mode mismatch
    // (RPC_E_CHANGED_MODE) still leaves COM usable — only skip on real failure.
    HRESULT initHr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool needUninit = SUCCEEDED(initHr);
    if (FAILED(initHr) && initHr != RPC_E_CHANGED_MODE) {
        qWarning() << "[HostAudioSink] CoInitializeEx failed:" << Qt::hex << initHr;
        return;
    }

    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr =
            CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                             __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
        if (FAILED(hr)) {
            qWarning() << "[HostAudioSink] MMDeviceEnumerator creation failed:" << Qt::hex << hr;
            if (needUninit) CoUninitialize();
            return;
        }

        ComPtr<IMMDeviceCollection> devices;
        hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices);
        if (FAILED(hr)) {
            qWarning() << "[HostAudioSink] EnumAudioEndpoints failed:" << Qt::hex << hr;
            if (needUninit) CoUninitialize();
            return;
        }

        UINT count = 0;
        devices->GetCount(&count);

        for (UINT i = 0; i < count; i++) {
            ComPtr<IMMDevice> device;
            if (FAILED(devices->Item(i, &device))) continue;

            ComPtr<IPropertyStore> props;
            if (FAILED(device->OpenPropertyStore(STGM_READ, &props))) continue;

            PROPVARIANT nameVar;
            PropVariantInit(&nameVar);
            if (FAILED(props->GetValue(kDeviceFriendlyName, &nameVar))) continue;
            const QString name =
                (nameVar.vt == VT_LPWSTR) ? QString::fromWCharArray(nameVar.pwszVal) : QString();
            PropVariantClear(&nameVar);

            // Sunshine's loopback-captured virtual sinks only — NEVER the
            // user's physical output devices.
            if (!name.contains(QLatin1String("Steam Streaming"), Qt::CaseInsensitive)) continue;

            ComPtr<IAudioEndpointVolume> volume;
            hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_INPROC_SERVER, nullptr,
                                  reinterpret_cast<void**>(&volume));
            if (FAILED(hr)) {
                qWarning() << "[HostAudioSink] IAudioEndpointVolume activation failed for" << name
                           << ":" << Qt::hex << hr;
                continue;
            }

            float level = -1.0f;
            BOOL muted = FALSE;
            volume->GetMasterVolumeLevelScalar(&level);
            volume->GetMute(&muted);

            if (level >= 0.999f && !muted) continue; // already at 100%, unmuted

            HRESULT hrVol = volume->SetMasterVolumeLevelScalar(1.0f, nullptr);
            HRESULT hrMute = volume->SetMute(FALSE, nullptr);
            if (SUCCEEDED(hrVol) && SUCCEEDED(hrMute)) {
                qInfo() << "[HostAudioSink] Restored" << name << "to 100% (was"
                        << qRound(level * 100) << "%, muted=" << (muted ? "true" : "false") << ")";
            } else {
                qWarning() << "[HostAudioSink] Failed to restore" << name << ": vol=" << Qt::hex
                           << hrVol << "mute=" << hrMute;
            }
        }
    } // COM interfaces released before CoUninitialize

    if (needUninit) CoUninitialize();
}

#else // !Q_OS_WIN

void HostAudioSink::ensureFullVolume()
{
    // Linux/macOS Sunshine builds don't use the Steam Streaming virtual sink;
    // nothing to normalize.
}

#endif
