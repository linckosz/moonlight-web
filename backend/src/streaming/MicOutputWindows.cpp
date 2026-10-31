/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 MoonlightWeb contributors
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

// The viewer's microphone on Windows: shared-mode WASAPI playback into the
// render side of a virtual cable, whose capture side is what the apps record.
// Event-driven on a thread of its own (MMCSS "Pro Audio"), which owns COM and
// every interface; start() waits for it to say whether the device opened.

#include "MicJitter.h"
#include "MicOutput.h"

#include <windows.h>
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
#include <propsys.h>

#include <algorithm>
#include <atomic>
#include <cwctype>
#include <future>
#include <thread>
#include <vector>

namespace mw::mic {
namespace {

// PKEY_Device_FriendlyName, defined locally for the reason HostAudioSink gives.
const PROPERTYKEY kDeviceFriendlyName = {
    {0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

// KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, without <ksmedia.h>'s INITGUID dance.
const GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// The render sides of the virtual cables people already have, in order. Never
// a real output, and never "Steam Streaming Speakers": Sunshine captures that
// one, and the viewer would hear their own voice come back.
const wchar_t* const kKnownCables[] = {
    L"CABLE Input",                // VB-Audio Virtual Cable
    L"VB-Audio",                   // its A/B/C/D variants and VoiceMeeter's inputs
    L"Steam Streaming Microphone", // installed with Steam Remote Play
};

constexpr REFERENCE_TIME kBufferHns = 20 * 10000; // 20 ms
constexpr DWORD kRetryMs = 2000;

template <typename T> struct ComPtr
{
    T* p = nullptr;
    ComPtr() = default;
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ~ComPtr() { reset(); }
    void reset()
    {
        if (p) p->Release();
        p = nullptr;
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string narrow(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0,
                                      nullptr, nullptr);
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr,
                        nullptr);
    return out;
}

std::wstring widen(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool containsNoCase(const std::wstring& haystack, const std::wstring& needle)
{
    auto lower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    };
    return !needle.empty() && lower(haystack).find(lower(needle)) != std::wstring::npos;
}

bool wantsAuto(const std::string& device)
{
    return device.empty() || _stricmp(device.c_str(), "auto") == 0;
}

/// The render endpoint to play into, and its name. COM must be initialised on
/// the calling thread. False when no endpoint matches.
bool findDevice(IMMDeviceEnumerator* enumerator, const std::string& device, ComPtr<IMMDevice>& out,
                std::wstring& name)
{
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices)))
        return false;
    UINT count = 0;
    devices->GetCount(&count);

    std::vector<std::wstring> wanted;
    if (wantsAuto(device))
        wanted.assign(std::begin(kKnownCables), std::end(kKnownCables));
    else
        wanted.push_back(widen(device));

    for (const std::wstring& needle : wanted) {
        for (UINT i = 0; i < count; ++i) {
            ComPtr<IMMDevice> candidate;
            if (FAILED(devices->Item(i, &candidate))) continue;
            ComPtr<IPropertyStore> props;
            if (FAILED(candidate->OpenPropertyStore(STGM_READ, &props))) continue;
            PROPVARIANT value;
            PropVariantInit(&value);
            if (FAILED(props->GetValue(kDeviceFriendlyName, &value))) continue;
            const std::wstring friendly = value.vt == VT_LPWSTR ? value.pwszVal : L"";
            PropVariantClear(&value);
            if (!containsNoCase(friendly, needle)) continue;
            out.reset();
            out.p = candidate.p;
            candidate.p = nullptr;
            name = friendly;
            return true;
        }
    }
    return false;
}

/// COM for the scope of one call, whatever the thread already had.
struct ComScope
{
    bool ok = false;
    bool owned = false;
    ComScope()
    {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr);
        ok = owned || hr == RPC_E_CHANGED_MODE;
    }
    ~ComScope()
    {
        if (owned) CoUninitialize();
    }
};

std::string noCableReason(const std::string& device)
{
    if (!wantsAuto(device))
        return "no playback device named like \"" + device +
               "\" (settings.json mic_device) is active on this PC";
    return "this PC has no virtual audio cable to play the microphone into. Install VB-CABLE "
           "(vb-audio.com/Cable): MoonlightWeb plays into its \"CABLE Input\", and apps record "
           "it as \"CABLE Output\"";
}

class WasapiOutput final : public IMicOutput
{
public:
    WasapiOutput(std::string device, LogFn log)
        : m_Device(std::move(device))
        , m_Log(std::move(log))
    {}
    ~WasapiOutput() override { stop(); }

    bool start(JitterRing& ring, std::string& why) override
    {
        if (m_Thread.joinable()) return true;
        m_Ring = &ring;
        m_Stop = false;
        m_Wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!m_Wake) {
            why = "could not create the audio event";
            return false;
        }
        std::promise<std::string> opened;
        std::future<std::string> result = opened.get_future();
        m_Thread = std::thread([this, &opened] { run(opened); });
        why = result.get();
        if (!why.empty()) {
            m_Thread.join();
            CloseHandle(m_Wake);
            m_Wake = nullptr;
            return false;
        }
        return true;
    }

    void stop() override
    {
        if (!m_Thread.joinable()) return;
        m_Stop = true;
        SetEvent(m_Wake);
        m_Thread.join();
        CloseHandle(m_Wake);
        m_Wake = nullptr;
    }

    std::string deviceName() const override { return m_Name; }

private:
    struct Stream
    {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioClient> client;
        ComPtr<IAudioRenderClient> render;
        HANDLE event = nullptr;
        UINT32 bufferFrames = 0;
        UINT32 channels = 2;
        void close()
        {
            if (client) client->Stop();
            render.reset();
            client.reset();
            device.reset();
            if (event) CloseHandle(event);
            event = nullptr;
        }
    };

    /// Opens the endpoint in the device's own channel count, at 48 kHz float:
    /// the engine converts the rate if the cable runs at another. Empty on
    /// success, else why; `name` is the endpoint's, once one matched.
    std::string open(IMMDeviceEnumerator* enumerator, Stream& s, std::string& name)
    {
        std::wstring wide;
        if (!findDevice(enumerator, m_Device, s.device, wide)) return noCableReason(m_Device);
        name = narrow(wide);

        if (FAILED(s.device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                      reinterpret_cast<void**>(&s.client))))
            return "could not open \"" + name + "\"";

        WAVEFORMATEX* mix = nullptr;
        if (SUCCEEDED(s.client->GetMixFormat(&mix)) && mix) {
            s.channels = std::max<UINT32>(1, std::min<UINT32>(mix->nChannels, 8));
            CoTaskMemFree(mix);
        }

        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = static_cast<WORD>(s.channels);
        fmt.Format.nSamplesPerSec = 48000;
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign = static_cast<WORD>(s.channels * sizeof(float));
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = s.channels == 1   ? SPEAKER_FRONT_CENTER
                            : s.channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT)
                                              : 0;
        fmt.SubFormat = kSubtypeFloat;

        const DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        HRESULT hr = s.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferHns, 0,
                                          reinterpret_cast<WAVEFORMATEX*>(&fmt), nullptr);
        if (FAILED(hr)) return "\"" + name + "\" refused the stream's format";

        s.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!s.event || FAILED(s.client->SetEventHandle(s.event)) ||
            FAILED(s.client->GetBufferSize(&s.bufferFrames)) ||
            FAILED(s.client->GetService(__uuidof(IAudioRenderClient),
                                        reinterpret_cast<void**>(&s.render))) ||
            FAILED(s.client->Start()))
            return "\"" + name + "\" could not be started";
        return {};
    }

    /// Fills what the device has room for. False when the device went away.
    bool fill(Stream& s, std::vector<float>& mono)
    {
        UINT32 padding = 0;
        HRESULT hr = s.client->GetCurrentPadding(&padding);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return false;
        if (FAILED(hr) || padding >= s.bufferFrames) return SUCCEEDED(hr);
        const UINT32 frames = s.bufferFrames - padding;
        BYTE* data = nullptr;
        hr = s.render->GetBuffer(frames, &data);
        if (hr == AUDCLNT_E_DEVICE_INVALIDATED) return false;
        if (FAILED(hr)) return true;
        mono.resize(frames);
        m_Ring->pull(mono.data(), frames);
        auto* out = reinterpret_cast<float*>(data);
        for (UINT32 i = 0; i < frames; ++i)
            for (UINT32 c = 0; c < s.channels; ++c)
                out[i * s.channels + c] = mono[i];
        s.render->ReleaseBuffer(frames, 0);
        return true;
    }

    void loop(IMMDeviceEnumerator* enumerator, Stream& s)
    {
        std::vector<float> mono;
        while (!m_Stop) {
            if (!s.client) {
                // The device left (cable disabled, driver reinstalled): look
                // for it again once per retry period, quietly.
                WaitForSingleObject(m_Wake, kRetryMs);
                if (m_Stop) break;
                std::string name;
                if (open(enumerator, s, name).empty()) {
                    if (m_Log) m_Log("\"" + name + "\" is back");
                } else {
                    s.close();
                }
                continue;
            }
            HANDLE waits[2] = {s.event, m_Wake};
            WaitForMultipleObjects(2, waits, FALSE, 200);
            if (m_Stop) break;
            if (!fill(s, mono)) {
                if (m_Log) m_Log("\"" + m_Name + "\" went away — retrying");
                s.close();
            }
        }
    }

    void run(std::promise<std::string>& opened)
    {
        ComScope com;
        if (!com.ok) {
            opened.set_value("COM could not be initialised on the audio thread");
            return;
        }
        DWORD task = 0;
        HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);

        {
            ComPtr<IMMDeviceEnumerator> enumerator;
            Stream s;
            std::string why;
            if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                        __uuidof(IMMDeviceEnumerator),
                                        reinterpret_cast<void**>(&enumerator))))
                why = "the Windows audio service is not available";
            else
                why = open(enumerator.p, s, m_Name);
            // m_Name is written before this and never again, so deviceName()
            // reads it without a lock. From here `opened` is gone.
            opened.set_value(why);
            if (why.empty()) loop(enumerator.p, s);
            s.close();
        }

        if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    }

    const std::string m_Device;
    const LogFn m_Log;
    JitterRing* m_Ring = nullptr;
    std::thread m_Thread;
    std::atomic<bool> m_Stop{false};
    HANDLE m_Wake = nullptr;
    std::string m_Name;
};

} // namespace

std::unique_ptr<IMicOutput> makeOutput(const std::string& device, LogFn log)
{
    return std::make_unique<WasapiOutput>(device, std::move(log));
}

std::string unavailable(const std::string& device)
{
    ComScope com;
    if (!com.ok) return "COM could not be initialised";
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_INPROC_SERVER,
                                __uuidof(IMMDeviceEnumerator),
                                reinterpret_cast<void**>(&enumerator))))
        return "the Windows audio service is not available";
    ComPtr<IMMDevice> found;
    std::wstring name;
    return findDevice(enumerator.p, device, found, name) ? std::string() : noCableReason(device);
}

} // namespace mw::mic
