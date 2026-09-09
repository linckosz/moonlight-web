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

#include "MfEncoder.h"

#include "../../core/Log.h"
#include "../RateControl.h"

#include <codecapi.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <mferror.h>
#include <wrl/implements.h>

#include <chrono>
#include <cstring>

namespace mw::native::encode {

using Microsoft::WRL::ComPtr;

namespace {

/// How long encode() waits for a transform to answer before deciding something
/// is wrong. Generous, because the first frame of a hardware transform includes
/// its own warm-up; a transform that is merely buffering is detected well before
/// this (see kPipelineHintMs).
constexpr unsigned kOutputTimeoutMs = 1000;

/// A transform that asks for the next picture and then stays silent this long
/// is holding the one it has: it encodes with one frame of pipeline. Logged and
/// accepted — its output arrives with the next input — rather than waited on.
constexpr unsigned kPipelineHintMs = 150;

/// The GOP "length" that means "never on your own": a keyframe is only ever
/// emitted on request. Transforms that cap this keep a periodic keyframe, which
/// is a cost and not an error.
constexpr UINT32 kNoPeriodicKeyframe = 0x7FFFFFFF;

std::string hr(HRESULT code)
{
    return MfApi::hresultToString(code);
}

/// ICodecAPI takes VARIANTs; these make the call sites readable.
bool setU32(ICodecAPI* api, const GUID& key, UINT32 value)
{
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_UI4;
    v.ulVal = value;
    return SUCCEEDED(api->SetValue(&key, &v));
}

bool setBool(ICodecAPI* api, const GUID& key, bool value)
{
    VARIANT v;
    ::VariantInit(&v);
    v.vt = VT_BOOL;
    v.boolVal = value ? VARIANT_TRUE : VARIANT_FALSE;
    return SUCCEEDED(api->SetValue(&key, &v));
}

} // namespace

// ── The event sink ─────────────────────────────────────────────────────────

class MfEncoder::EventSink final
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, IMFAsyncCallback>
{
public:
    struct Event
    {
        MediaEventType type = MEUnknown;
        HRESULT status = S_OK;
    };

    HRESULT RuntimeClassInitialize(IMFMediaEventGenerator* generator)
    {
        m_Generator = generator;
        return arm();
    }

    /// Stop re-arming; the generator is going away.
    void close()
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Closed = true;
    }

    /// Wait for the next event, up to @p timeoutMs. False on timeout.
    bool next(Event& out, unsigned timeoutMs)
    {
        std::unique_lock<std::mutex> lock(m_Mutex);
        if (!m_Cv.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                           [this] { return !m_Queue.empty(); }))
            return false;
        out = m_Queue.front();
        m_Queue.pop_front();
        return true;
    }

    // IMFAsyncCallback
    HRESULT STDMETHODCALLTYPE GetParameters(DWORD*, DWORD*) override { return E_NOTIMPL; }

    HRESULT STDMETHODCALLTYPE Invoke(IMFAsyncResult* result) override
    {
        ComPtr<IMFMediaEvent> event;
        Event parsed;
        if (SUCCEEDED(m_Generator->EndGetEvent(result, &event)) && event) {
            event->GetType(&parsed.type);
            event->GetStatus(&parsed.status);
        } else {
            parsed.type = MEError;
            parsed.status = E_FAIL;
        }
        // Every event, at debug level: the only window into a transform that
        // takes a picture and then says nothing.
        if (log::enabled(log::Debug))
            log::debug("[native] MF event " + std::to_string(parsed.type) + " status " +
                       MfApi::hresultToString(parsed.status));
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Queue.push_back(parsed);
            if (m_Closed) {
                m_Cv.notify_all();
                return S_OK;
            }
        }
        m_Cv.notify_all();
        arm();
        return S_OK;
    }

private:
    HRESULT arm() { return m_Generator->BeginGetEvent(this, nullptr); }

    ComPtr<IMFMediaEventGenerator> m_Generator;
    std::mutex m_Mutex;
    std::condition_variable m_Cv;
    std::deque<Event> m_Queue;
    bool m_Closed = false;
};

// ── Lifecycle ──────────────────────────────────────────────────────────────

MfEncoder::MfEncoder() = default;

MfEncoder::~MfEncoder()
{
    stop();
}

bool MfEncoder::init(ID3D11Device* device, Codec codec, int width, int height, int fps,
                     int bitrateKbps, bool yuv444, bool hdr, bool intraRefresh,
                     const EncoderTuning& tuning, std::string& error)
{
    (void)intraRefresh; // Media Foundation has no intra-refresh; reported false

    if (yuv444 || hdr) {
        // The Selector never grants either on the fallback tier; this is the
        // guard against a misrouted session, as on every other encoder.
        error = "Media Foundation encodes 8-bit 4:2:0 only";
        return false;
    }
    if (codec == Codec::Av1) {
        error = "no AV1 through Media Foundation";
        return false;
    }

    m_Api = MfApi::instance();
    if (!m_Api->available()) {
        error = m_Api->unavailableReason();
        return false;
    }
    const HRESULT started = m_Api->Startup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(started)) {
        error = "MFStartup failed: " + hr(started);
        return false;
    }
    m_Started = true;

    m_Device = device;
    m_Device->GetImmediateContext(&m_Context);
    m_Width = width;
    m_Height = height;
    m_Fps = fps > 0 ? fps : 60;
    m_FrameDuration = 10000000LL / m_Fps;
    m_SkipHardware = tuning.fallback == EncoderTuning::Fallback::MediaFoundationSoftware;

    // Bring the transform up and make it encode one picture before the session
    // trusts it. A hardware transform that takes the picture and then says
    // nothing — AMD's does exactly that on this bench, texture or system
    // memory alike, one METransformNeedInput and then silence (07/09/2026) —
    // must cost this machine a slower encoder, not a dead stream. So it is
    // found out here, at init, and the next tier down is tried in its place.
    if (!bringUp(codec, width, height, bitrateKbps, tuning, error)) {
        if (m_SkipHardware || !m_Hardware) {
            stop();
            return false;
        }
        const std::string module = describeModule();
        log::warning("[native] " + m_Name + " is not usable (" + error +
                     ") — trying the software transform instead" +
                     (module.empty() ? ""
                                     : ". That transform is " + module +
                                           " — if it is much older than this machine's "
                                           "graphics driver, update the driver"));
        teardownTransform();
        m_SkipHardware = true;
        if (!bringUp(codec, width, height, bitrateKbps, tuning, error)) {
            stop();
            return false;
        }
    }

    const std::string overrides = tuning.describe();
    log::info(
        "[native] Media Foundation ready: " + std::to_string(width) + "x" + std::to_string(height) +
        "@" + std::to_string(m_Fps) + " " + toString(codec) + " CBR " +
        std::to_string(bitrateKbps) + " kbps, VBV " +
        std::to_string(vbvBits(static_cast<uint32_t>(bitrateKbps) * 1000, m_Fps, tuning.vbvFrames) /
                       8 / 1024) +
        " KB, keyframes on request — " + m_Name + (m_Hardware ? " (hardware" : " (software") +
        (m_Async ? ", asynchronous" : ", synchronous") +
        (m_D3dAware ? ", frames stay on the GPU)" : ", frames copied to system memory)") +
        (overrides.empty() ? "" : " [bench: " + overrides + "]"));
    return true;
}

bool MfEncoder::bringUp(Codec codec, int width, int height, int bitrateKbps,
                        const EncoderTuning& tuning, std::string& error)
{
    if (!openTransform(codec, error)) return false;

    if (tuning.fallback == EncoderTuning::Fallback::MediaFoundationCpuInput && m_D3dAware) {
        // Bench: the hardware transform, fed through system memory — tells a
        // texture the transform dislikes apart from a transform that is stuck.
        log::info("[native] " + m_Name + ": D3D11 path disabled by the bench");
        m_D3dAware = false;
    }

    // The device goes to the transform BEFORE the types: a D3D-aware transform
    // decides what it accepts from what it has been given.
    if (m_D3dAware && !attachDevice(m_Device.Get())) {
        log::warning("[native] " + m_Name +
                     " did not take the D3D11 device — frames will cross to system memory");
        m_D3dAware = false;
    }

    if (!configureTypes(codec, width, height, m_Fps, bitrateKbps, error)) return false;

    applyCodecOptions(bitrateKbps, m_Fps, m_Hardware, tuning);

    MFT_OUTPUT_STREAM_INFO info = {};
    if (SUCCEEDED(m_Transform->GetOutputStreamInfo(m_OutputId, &info))) {
        m_ProvidesSamples = (info.dwFlags & MFT_OUTPUT_STREAM_PROVIDES_SAMPLES) != 0;
        m_OutputSize = info.cbSize;
    }
    // A generous floor when the transform does not say: an intra frame of this
    // size at a usable QP never approaches raw NV12.
    if (m_OutputSize == 0) m_OutputSize = static_cast<DWORD>(width) * height * 3 / 2;

    HRESULT s = m_Transform->ProcessMessage(MFT_MESSAGE_NOTIFY_BEGIN_STREAMING, 0);
    if (SUCCEEDED(s)) s = m_Transform->ProcessMessage(MFT_MESSAGE_NOTIFY_START_OF_STREAM, 0);
    if (FAILED(s)) {
        error = "the transform refused to start streaming: " + hr(s);
        return false;
    }

    if (m_Async) {
        // Asynchronous transforms have to be listened to from the start: the
        // first METransformNeedInput is what lets the first picture in.
        ComPtr<IMFMediaEventGenerator> events;
        if (FAILED(m_Transform.As(&events))) {
            error = "an asynchronous transform without an event generator";
            return false;
        }
        m_Events = events;
        if (FAILED(Microsoft::WRL::MakeAndInitialize<EventSink>(&m_Sink, events.Get()))) {
            error = "could not listen to the transform's events";
            return false;
        }
    }

    return proveWithOnePicture(error);
}

std::string MfEncoder::describeModule() const
{
    // Only ever called on the failure path, which is what makes a registry read
    // and a file stat affordable here.
    //
    // ⚠️ Why this exists at all (09/09/2026): "AMDh264Encoder took one picture
    // and went silent" read for two days as an unexplained transform bug. It
    // was a mixed driver stack on the bench — that transform's DLL is dated
    // December 2023 while the machine's AMF runtime is from August 2026, the
    // discrete Radeon still running its 2023 driver beside a 2026 one on the
    // iGPU. A friendly name cannot say that. A path and a date can.
    if (::IsEqualGUID(m_Clsid, GUID{})) return {};

    wchar_t guid[64] = {};
    if (::StringFromGUID2(m_Clsid, guid, static_cast<int>(std::size(guid))) == 0) return {};
    const std::wstring key = std::wstring(L"CLSID\\") + guid + L"\\InprocServer32";

    wchar_t path[MAX_PATH] = {};
    DWORD bytes = sizeof(path);
    if (::RegGetValueW(HKEY_CLASSES_ROOT, key.c_str(), nullptr,
                       RRF_RT_REG_SZ | RRF_RT_REG_EXPAND_SZ, nullptr, path,
                       &bytes) != ERROR_SUCCESS)
        return {};

    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
    if (needed <= 1) return {};
    std::string out(static_cast<size_t>(needed - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, path, -1, out.data(), needed, nullptr, nullptr);

    WIN32_FILE_ATTRIBUTE_DATA attrs = {};
    if (::GetFileAttributesExW(path, GetFileExInfoStandard, &attrs)) {
        FILETIME local = {};
        SYSTEMTIME when = {};
        if (::FileTimeToLocalFileTime(&attrs.ftLastWriteTime, &local) &&
            ::FileTimeToSystemTime(&local, &when)) {
            char stamp[24] = {};
            std::snprintf(stamp, sizeof(stamp), ", dated %04u-%02u-%02u", when.wYear, when.wMonth,
                          when.wDay);
            out += stamp;
        }
    }
    return out;
}

bool MfEncoder::proveWithOnePicture(std::string& error)
{
    // A black NV12 picture of the session's size, on the session's device: the
    // same texture the converter would hand over, minus the content.
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(m_Width);
    desc.Height = static_cast<UINT>(m_Height);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> black;
    HRESULT s = m_Device->CreateTexture2D(&desc, nullptr, &black);
    if (FAILED(s)) {
        error = "could not create the trial picture: " + hr(s);
        return false;
    }
    // Zero is not black in NV12 (it is dark green); the encoder does not care,
    // and this picture is never sent.

    EncoderOutput out;
    const bool ok = encode(black.Get(), true, 0, out, error);
    const bool produced = ok && out.size > 0;
    releaseOutput();
    if (!ok) return false;
    if (!produced) {
        // The pipelining transform: it will answer with the next picture. That
        // is a cost, not a failure — as long as it does answer, which the
        // second trial picture establishes.
        const bool ok2 = encode(black.Get(), false, 1, out, error);
        const bool produced2 = ok2 && out.size > 0;
        releaseOutput();
        if (!ok2) return false;
        if (!produced2) {
            error = "accepted two pictures and produced nothing";
            return false;
        }
    }
    // The trial pictures were consumed; the session's first real one has to be
    // a keyframe again, whatever the caller asks.
    m_KeyframeNext = true;
    return true;
}

void MfEncoder::teardownTransform()
{
    releaseOutput();
    if (m_Sink) m_Sink->close();
    if (m_Transform) {
        m_Transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_OF_STREAM, 0);
        m_Transform->ProcessMessage(MFT_MESSAGE_NOTIFY_END_STREAMING, 0);
    }
    m_CodecApi.Reset();
    m_Events.Reset();
    m_Sink.Reset();
    m_Transform.Reset();
    m_DeviceManager.Reset();
    m_Name.clear();
    m_Hardware = m_Async = m_D3dAware = m_ProvidesSamples = false;
    m_OutputSize = 0;
    m_InputId = m_OutputId = 0;
    m_InputCredits = 0;
    m_FramesIn = m_FramesOut = 0;
    m_HeldFrameLogged = false;
}

bool MfEncoder::openTransform(Codec codec, std::string& error)
{
    const GUID& subtype = codec == Codec::Hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264;
    MFT_REGISTER_TYPE_INFO input = {MFMediaType_Video, MFVideoFormat_NV12};
    MFT_REGISTER_TYPE_INFO output = {MFMediaType_Video, subtype};

    // The same two tiers in the same order as the capability probe, so a
    // session lands on the transform the probe reported.
    const UINT32 kHardware = MFT_ENUM_FLAG_HARDWARE | MFT_ENUM_FLAG_SORTANDFILTER;
    const UINT32 kSoftware = MFT_ENUM_FLAG_SYNCMFT | MFT_ENUM_FLAG_ASYNCMFT |
                             MFT_ENUM_FLAG_LOCALMFT | MFT_ENUM_FLAG_SORTANDFILTER;

    // A hardware transform is asked for on THIS adapter. Machine-wide, the
    // enumeration sorts all vendors' transforms together: on a machine with an
    // AMD and an NVIDIA card, a session on the NVIDIA display was handed
    // AMDh264Encoder, which then refused every output type — its device is not
    // ours (measured 07/09/2026).
    ComPtr<IMFAttributes> adapter;
    {
        ComPtr<IDXGIDevice> dxgiDevice;
        ComPtr<IDXGIAdapter> dxgiAdapter;
        DXGI_ADAPTER_DESC desc = {};
        if (SUCCEEDED(m_Device.As(&dxgiDevice)) &&
            SUCCEEDED(dxgiDevice->GetAdapter(&dxgiAdapter)) &&
            SUCCEEDED(dxgiAdapter->GetDesc(&desc)) &&
            SUCCEEDED(m_Api->CreateAttributes(&adapter, 1))) {
            adapter->SetBlob(MFT_ENUM_ADAPTER_LUID,
                             reinterpret_cast<const UINT8*>(&desc.AdapterLuid),
                             sizeof(desc.AdapterLuid));
        }
    }

    for (const bool hardware : {true, false}) {
        // The bench may ask for Microsoft's software transform on a machine
        // whose hardware one would otherwise always win — the only way to
        // exercise the system-memory path where a hardware transform exists.
        if (hardware && m_SkipHardware) continue;
        IMFActivate** activates = nullptr;
        UINT32 count = 0;
        const HRESULT s =
            m_Api->TEnum2(MFT_CATEGORY_VIDEO_ENCODER, hardware ? kHardware : kSoftware, &input,
                          &output, hardware ? adapter.Get() : nullptr, &activates, &count);
        if (FAILED(s) || count == 0) {
            if (activates) ::CoTaskMemFree(activates);
            continue;
        }

        wchar_t* wname = nullptr;
        UINT32 wlen = 0;
        if (SUCCEEDED(
                activates[0]->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &wname, &wlen))) {
            m_Name.clear();
            for (const wchar_t* p = wname; *p; ++p)
                if (*p < 0x80) m_Name.push_back(static_cast<char>(*p));
            ::CoTaskMemFree(wname);
        }
        if (m_Name.empty()) m_Name = "unnamed transform";
        // Kept now because the activation object is freed three lines down, and
        // it is the only thing that leads back to the DLL. Read, never used,
        // unless the transform then misbehaves.
        if (FAILED(activates[0]->GetGUID(MFT_TRANSFORM_CLSID_Attribute, &m_Clsid)))
            m_Clsid = GUID{};

        const HRESULT act = activates[0]->ActivateObject(IID_PPV_ARGS(&m_Transform));
        for (UINT32 i = 0; i < count; ++i)
            activates[i]->Release();
        ::CoTaskMemFree(activates);

        if (FAILED(act) || !m_Transform) {
            log::warning("[native] " + m_Name + " could not be activated: " + hr(act));
            m_Transform.Reset();
            continue;
        }
        m_Hardware = hardware;
        break;
    }
    if (!m_Transform) {
        error = std::string("no Media Foundation encoder for ") + toString(codec);
        return false;
    }

    // What kind of transform this is — asked, never inferred from the name.
    ComPtr<IMFAttributes> attrs;
    if (SUCCEEDED(m_Transform->GetAttributes(&attrs)) && attrs) {
        UINT32 flag = 0;
        m_Async = SUCCEEDED(attrs->GetUINT32(MF_TRANSFORM_ASYNC, &flag)) && flag != 0;
        if (m_Async) attrs->SetUINT32(MF_TRANSFORM_ASYNC_UNLOCK, TRUE);
        flag = 0;
        m_D3dAware = SUCCEEDED(attrs->GetUINT32(MF_SA_D3D11_AWARE, &flag)) && flag != 0;
        // The one attribute that speaks directly to what this engine is for.
        attrs->SetUINT32(MF_LOW_LATENCY, TRUE);
    }

    DWORD inCount = 0, outCount = 0;
    if (SUCCEEDED(m_Transform->GetStreamCount(&inCount, &outCount)) && inCount > 0 &&
        outCount > 0) {
        // E_NOTIMPL here means the ids are 0 and 0, which is what they already are.
        DWORD inIds[1] = {0}, outIds[1] = {0};
        if (SUCCEEDED(m_Transform->GetStreamIDs(1, inIds, 1, outIds))) {
            m_InputId = inIds[0];
            m_OutputId = outIds[0];
        }
    }
    return true;
}

bool MfEncoder::attachDevice(ID3D11Device* device)
{
    // The transform will touch the device from its own threads.
    ComPtr<ID3D11Multithread> mt;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&mt))) && mt)
        mt->SetMultithreadProtected(TRUE);

    if (FAILED(m_Api->CreateDXGIDeviceManager(&m_ResetToken, &m_DeviceManager))) return false;
    if (FAILED(m_DeviceManager->ResetDevice(device, m_ResetToken))) return false;
    return SUCCEEDED(m_Transform->ProcessMessage(
        MFT_MESSAGE_SET_D3D_MANAGER, reinterpret_cast<ULONG_PTR>(m_DeviceManager.Get())));
}

bool MfEncoder::configureTypes(Codec codec, int width, int height, int fps, int bitrateKbps,
                               std::string& error)
{
    // Output first: an encoder transform derives what it will accept from what
    // it has been asked to produce.
    ComPtr<IMFMediaType> out;
    HRESULT s = m_Api->CreateMediaType(&out);
    if (FAILED(s)) {
        error = "could not create the output type: " + hr(s);
        return false;
    }
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, codec == Codec::Hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(bitrateKbps) * 1000);
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                       static_cast<UINT32>(height));
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // High profile: CABAC and the 8×8 transform cost the decoder nothing a
    // browser notices and buy real bits; every browser decodes it in hardware.
    // HEVC Main is the only 8-bit 4:2:0 profile there is.
    out->SetUINT32(MF_MT_MPEG2_PROFILE,
                   codec == Codec::Hevc ? eAVEncH265VProfile_Main_420_8 : eAVEncH264VProfile_High);
    s = m_Transform->SetOutputType(m_OutputId, out.Get(), 0);
    if (FAILED(s)) {
        error = m_Name + " refused " + std::to_string(width) + "x" + std::to_string(height) + "@" +
                std::to_string(fps) + " " + toString(codec) + " at " + std::to_string(bitrateKbps) +
                " kbps: " + hr(s);
        return false;
    }

    ComPtr<IMFMediaType> in;
    s = m_Api->CreateMediaType(&in);
    if (FAILED(s)) {
        error = "could not create the input type: " + hr(s);
        return false;
    }
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, static_cast<UINT32>(width),
                       static_cast<UINT32>(height));
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, static_cast<UINT32>(fps), 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    // The CPU path hands over contiguous rows; say so rather than let the
    // transform assume an aligned stride.
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(width));
    s = m_Transform->SetInputType(m_InputId, in.Get(), 0);
    if (FAILED(s)) {
        error = m_Name + " refused NV12 " + std::to_string(width) + "x" + std::to_string(height) +
                ": " + hr(s);
        return false;
    }
    return true;
}

void MfEncoder::applyCodecOptions(int bitrateKbps, int fps, bool hardware,
                                  const EncoderTuning& tuning)
{
    if (FAILED(m_Transform.As(&m_CodecApi)) || !m_CodecApi) {
        log::warning("[native] " + m_Name +
                     " exposes no ICodecAPI — running on its defaults (no low-latency mode, "
                     "no bitrate changes)");
        return;
    }

    // Each is best effort and each refusal is said once: a hardware transform
    // that ignores the GOP setting still beats the CPU, and the reader of the
    // log deserves to know which promises this particular machine keeps.
    std::string refused;
    auto note = [&refused](bool ok, const char* what) {
        if (ok) return;
        if (!refused.empty()) refused += ", ";
        refused += what;
    };

    note(setU32(m_CodecApi.Get(), CODECAPI_AVEncCommonRateControlMode,
                eAVEncCommonRateControlMode_CBR),
         "CBR");
    note(setU32(m_CodecApi.Get(), CODECAPI_AVEncCommonMeanBitRate,
                static_cast<UINT32>(bitrateKbps) * 1000),
         "bitrate");
    // The one-frame VBV, same rule as the vendor paths (RateControl.h).
    note(setU32(m_CodecApi.Get(), CODECAPI_AVEncCommonBufferSize,
                vbvBits(static_cast<uint32_t>(bitrateKbps) * 1000, fps, tuning.vbvFrames)),
         "VBV");
    // One in, one out: the transform may not hold a picture back.
    note(setBool(m_CodecApi.Get(), CODECAPI_AVLowLatencyMode, true), "low-latency mode");
    // No B-frames: one would make the encoder hold a frame back to reference a
    // picture that has not been sent, buying a whole frame of latency.
    note(setU32(m_CodecApi.Get(), CODECAPI_AVEncMPVDefaultBPictureCount, 0), "no B-frames");
    // Keyframes on request only. A keyframe is a bitrate spike, and on a
    // congested link the spike causes the loss that causes the next request.
    note(setU32(m_CodecApi.Get(), CODECAPI_AVEncMPVGOPSize, kNoPeriodicKeyframe), "infinite GOP");
    if (!hardware) {
        // Microsoft's software transform: speed over quality, all the way. This
        // is a machine with no encoder to spare; the bits it saves at the other
        // end of the scale are not worth the frames it would drop.
        note(setU32(m_CodecApi.Get(), CODECAPI_AVEncCommonQualityVsSpeed, 0), "speed over quality");
    }

    if (!refused.empty())
        log::info("[native] " + m_Name + " does not take: " + refused +
                  " — running its own default there");
}

// ── Encoding ───────────────────────────────────────────────────────────────

bool MfEncoder::readbackToCpu(ID3D11Texture2D* surface, std::string& error)
{
    if (!m_Staging) {
        D3D11_TEXTURE2D_DESC desc = {};
        surface->GetDesc(&desc);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        const HRESULT s = m_Device->CreateTexture2D(&desc, nullptr, &m_Staging);
        if (FAILED(s)) {
            error = "could not create the staging texture: " + hr(s);
            return false;
        }
        m_Cpu.resize(static_cast<size_t>(m_Width) * m_Height * 3 / 2);
    }

    // The GPU→CPU copy this path cannot avoid. Immediate and blocking: Map
    // waits for the copy, which is the only way to hand the transform a frame
    // that is finished.
    m_Context->CopyResource(m_Staging.Get(), surface);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    const HRESULT s = m_Context->Map(m_Staging.Get(), 0, D3D11_MAP_READ, 0, &mapped);
    if (FAILED(s)) {
        error = "could not map the staging texture: " + hr(s);
        return false;
    }
    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    uint8_t* dst = m_Cpu.data();
    const size_t rowBytes = static_cast<size_t>(m_Width);
    for (int y = 0; y < m_Height; ++y)
        std::memcpy(dst + rowBytes * y, src + static_cast<size_t>(mapped.RowPitch) * y, rowBytes);
    // NV12 staging: the interleaved chroma plane follows the luma plane at the
    // same pitch, half as many rows.
    const uint8_t* srcUv = src + static_cast<size_t>(mapped.RowPitch) * m_Height;
    uint8_t* dstUv = dst + rowBytes * m_Height;
    for (int y = 0; y < m_Height / 2; ++y)
        std::memcpy(dstUv + rowBytes * y, srcUv + static_cast<size_t>(mapped.RowPitch) * y,
                    rowBytes);
    m_Context->Unmap(m_Staging.Get(), 0);
    return true;
}

bool MfEncoder::makeInputSample(ID3D11Texture2D* surface, uint32_t frameNumber,
                                ComPtr<IMFSample>& sample, std::string& error)
{
    ComPtr<IMFMediaBuffer> buffer;
    if (m_D3dAware) {
        // The converter's texture, as it is. This is the zero-copy path the
        // interface promises, kept on a transform we have no SDK for.
        const HRESULT s =
            m_Api->CreateDXGISurfaceBuffer(__uuidof(ID3D11Texture2D), surface, 0, FALSE, &buffer);
        if (FAILED(s)) {
            error = "could not wrap the texture for the transform: " + hr(s);
            return false;
        }
        DWORD length = 0;
        if (SUCCEEDED(buffer->GetMaxLength(&length))) buffer->SetCurrentLength(length);
    } else {
        if (!readbackToCpu(surface, error)) return false;
        const auto size = static_cast<DWORD>(m_Cpu.size());
        HRESULT s = m_Api->CreateMemoryBuffer(size, &buffer);
        if (FAILED(s)) {
            error = "could not allocate the input buffer: " + hr(s);
            return false;
        }
        BYTE* dst = nullptr;
        DWORD max = 0, cur = 0;
        s = buffer->Lock(&dst, &max, &cur);
        if (FAILED(s)) {
            error = "could not lock the input buffer: " + hr(s);
            return false;
        }
        std::memcpy(dst, m_Cpu.data(), size);
        buffer->Unlock();
        buffer->SetCurrentLength(size);
    }

    HRESULT s = m_Api->CreateSample(&sample);
    if (FAILED(s)) {
        error = "could not create the input sample: " + hr(s);
        return false;
    }
    sample->AddBuffer(buffer.Get());
    // Timestamps are not optional: Microsoft's transform refuses a sample
    // without one, and the rate control of every transform paces on them. Our
    // own count rather than the caller's number: the trial pictures of init()
    // already used the first two, and a clock that runs backwards is the kind
    // of thing a rate controller silently punishes.
    (void)frameNumber;
    sample->SetSampleTime(static_cast<LONGLONG>(m_FramesIn) * m_FrameDuration);
    sample->SetSampleDuration(m_FrameDuration);
    return true;
}

bool MfEncoder::waitForEvent(MediaEventType wanted, unsigned timeoutMs, HRESULT& eventStatus)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return false;
        const auto left =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
        EventSink::Event ev;
        if (!m_Sink->next(ev, static_cast<unsigned>(left))) return false;
        if (ev.type == MEError) {
            eventStatus = FAILED(ev.status) ? ev.status : E_FAIL;
            return false;
        }
        if (ev.type == METransformNeedInput) ++m_InputCredits;
        if (ev.type == wanted) {
            eventStatus = S_OK;
            return true;
        }
    }
}

HRESULT MfEncoder::takeOutput(ComPtr<IMFSample>& sample, std::string& error)
{
    for (int attempt = 0; attempt < 2; ++attempt) {
        MFT_OUTPUT_DATA_BUFFER out = {};
        out.dwStreamID = m_OutputId;
        ComPtr<IMFSample> ours;
        if (!m_ProvidesSamples) {
            ComPtr<IMFMediaBuffer> buffer;
            if (FAILED(m_Api->CreateMemoryBuffer(m_OutputSize, &buffer)) ||
                FAILED(m_Api->CreateSample(&ours))) {
                error = "could not allocate the output sample";
                return E_OUTOFMEMORY;
            }
            ours->AddBuffer(buffer.Get());
            out.pSample = ours.Get();
        }

        DWORD status = 0;
        const HRESULT s = m_Transform->ProcessOutput(0, 1, &out, &status);
        if (out.pEvents) out.pEvents->Release();

        if (s == MF_E_TRANSFORM_STREAM_CHANGE) {
            // The transform wants to renegotiate its output type — accept its
            // own first proposal, once, and try again.
            ComPtr<IMFMediaType> type;
            if (SUCCEEDED(m_Transform->GetOutputAvailableType(m_OutputId, 0, &type)) && type)
                m_Transform->SetOutputType(m_OutputId, type.Get(), 0);
            // A sample the transform handed us here is its reference to drop;
            // one we allocated is `ours` and goes with the ComPtr.
            if (out.pSample && m_ProvidesSamples) out.pSample->Release();
            continue;
        }
        if (FAILED(s)) {
            if (s != MF_E_TRANSFORM_NEED_MORE_INPUT) error = "ProcessOutput failed: " + hr(s);
            if (out.pSample && m_ProvidesSamples) out.pSample->Release();
            return s;
        }
        if (m_ProvidesSamples) {
            sample.Attach(out.pSample); // the transform's reference becomes ours
        } else {
            sample = ours;
        }
        return S_OK;
    }
    error = "the transform kept changing its output type";
    return E_FAIL;
}

bool MfEncoder::encode(ID3D11Texture2D* surface, bool forceKeyframe, uint32_t frameNumber,
                       EncoderOutput& out, std::string& error)
{
    out = EncoderOutput{};
    if (!m_Transform) {
        error = "encoder not initialised";
        return false;
    }
    if (m_OutSample) {
        error = "previous output not released";
        return false;
    }

    if ((forceKeyframe || m_KeyframeNext) && m_CodecApi)
        setU32(m_CodecApi.Get(), CODECAPI_AVEncVideoForceKeyFrame, 1);
    m_KeyframeNext = false;

    ComPtr<IMFSample> input;
    if (!makeInputSample(surface, frameNumber, input, error)) return false;

    if (m_Async) {
        // A picture may only go in once the transform has asked for one.
        HRESULT st = S_OK;
        if (m_InputCredits <= 0 && !waitForEvent(METransformNeedInput, kOutputTimeoutMs, st)) {
            error = FAILED(st) ? "the transform reported an error: " + hr(st)
                               : "the transform never asked for a picture";
            return false;
        }
        --m_InputCredits;
    }

    HRESULT s = m_Transform->ProcessInput(m_InputId, input.Get(), 0);
    if (FAILED(s)) {
        error = "ProcessInput failed: " + hr(s);
        return false;
    }
    ++m_FramesIn;

    ComPtr<IMFSample> output;
    if (m_Async) {
        // Wait for the picture to come back. A transform that instead asks for
        // the next one and then goes quiet is pipelining by design: its output
        // will arrive with the next input, and the cost is one frame — said
        // once, and not waited for a second longer than needed.
        HRESULT st = S_OK;
        const int creditsBefore = m_InputCredits;
        const unsigned firstWait = m_FramesOut == 0 ? kOutputTimeoutMs : kPipelineHintMs;
        bool got = waitForEvent(METransformHaveOutput, firstWait, st);
        if (!got && FAILED(st)) {
            error = "the transform reported an error: " + hr(st);
            return false;
        }
        if (!got && m_InputCredits <= creditsBefore) {
            // Neither output nor a request for more: genuinely slow, or stuck.
            got = waitForEvent(METransformHaveOutput, kOutputTimeoutMs, st);
        }
        if (!got) {
            if (FAILED(st)) {
                error = "the transform reported an error: " + hr(st);
                return false;
            }
            if (m_InputCredits > creditsBefore) {
                if (!m_HeldFrameLogged) {
                    m_HeldFrameLogged = true;
                    log::warning("[native] " + m_Name +
                                 " holds one picture back (its output arrives with the next "
                                 "input) — one frame of encoder latency on this machine");
                }
                return true; // nothing to send this time; the frame is not lost
            }
            error =
                "the transform produced nothing within " + std::to_string(kOutputTimeoutMs) + " ms";
            return false;
        }
        s = takeOutput(output, error);
        if (FAILED(s)) return false;
    } else {
        s = takeOutput(output, error);
        if (s == MF_E_TRANSFORM_NEED_MORE_INPUT) {
            if (!m_HeldFrameLogged) {
                m_HeldFrameLogged = true;
                log::warning("[native] " + m_Name +
                             " holds one picture back despite low-latency mode — one frame of "
                             "encoder latency on this machine");
            }
            return true;
        }
        if (FAILED(s)) return false;
    }
    ++m_FramesOut;

    // The bitstream may be split across buffers; one contiguous view of it.
    ComPtr<IMFMediaBuffer> buffer;
    s = output->ConvertToContiguousBuffer(&buffer);
    if (FAILED(s)) {
        error = "could not read the output sample: " + hr(s);
        return false;
    }
    BYTE* data = nullptr;
    DWORD max = 0, length = 0;
    s = buffer->Lock(&data, &max, &length);
    if (FAILED(s)) {
        error = "could not lock the output buffer: " + hr(s);
        return false;
    }
    UINT32 clean = 0;
    output->GetUINT32(MFSampleExtension_CleanPoint, &clean);

    m_OutSample = output;
    m_OutBuffer = buffer;
    out.data = data;
    out.size = length;
    out.keyframe = clean != 0;
    out.avgQp = -1; // Media Foundation does not say
    return true;
}

void MfEncoder::releaseOutput()
{
    if (m_OutBuffer) m_OutBuffer->Unlock();
    m_OutBuffer.Reset();
    m_OutSample.Reset();
}

bool MfEncoder::setBitrate(int bitrateKbps, std::string& error)
{
    if (!m_CodecApi) {
        error = m_Name + " exposes no ICodecAPI";
        return false;
    }
    if (!setU32(m_CodecApi.Get(), CODECAPI_AVEncCommonMeanBitRate,
                static_cast<UINT32>(bitrateKbps) * 1000)) {
        error = m_Name + " refused the bitrate change";
        return false;
    }
    return true;
}

void MfEncoder::stop()
{
    if (m_Transform && m_FramesIn > 0)
        log::info("[native] " + m_Name + ": " + std::to_string(m_FramesIn) + " pictures in, " +
                  std::to_string(m_FramesOut) + " out");
    teardownTransform();
    m_Staging.Reset();
    m_Context.Reset();
    m_Device.Reset();
    m_Cpu.clear();
    if (m_Started && m_Api) m_Api->Shutdown();
    m_Started = false;
}

} // namespace mw::native::encode
