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

#include "IVideoEncoder.h"
#include "MfApi.h"

#include <icodecapi.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <vector>

namespace mw::native::encode {

/// Media Foundation, the encoder of a machine whose GPU we have no SDK for.
///
/// ── What it is, and is not ──────────────────────────────────────────────────
///
/// This is the first rung of the fallback tier (Capabilities::fallbacks), and
/// it is two different things behind one interface. Enumerated with
/// MFT_ENUM_FLAG_HARDWARE it is a vendor's transform over fixed-function
/// silicon — Qualcomm's on a Snapdragon laptop, which is the machine that made
/// this class worth writing — and the frames stay on the GPU: the D3D11 texture
/// the conversion pass wrote goes in as it is, through a DXGI device manager.
/// Enumerated without the flag it is Microsoft's own software H.264 transform,
/// which takes CPU memory, so every frame first crosses to system RAM through
/// a staging texture. The class tells the two apart at init() by asking the
/// transform (`MF_SA_D3D11_AWARE`), never by its name, and says which it got.
///
/// ── The latency decisions ──────────────────────────────────────────────────
///
/// Same as the three vendor paths, translated into Media Foundation's
/// vocabulary: `AVLowLatencyMode` on (one frame in, one frame out — the
/// transform may not hold pictures back), no B-frames, CBR, a one-frame VBV,
/// a GOP long enough that no periodic keyframe is ever emitted, keyframes on
/// request only. Where a transform ignores one of these it is logged, not
/// treated as fatal: a hardware encoder that refuses an infinite GOP still
/// beats the CPU.
///
/// ── Asynchronous transforms ─────────────────────────────────────────────────
///
/// Hardware transforms are asynchronous: they do not return a frame from
/// ProcessOutput, they raise METransformHaveOutput when one is ready and
/// METransformNeedInput when they will take another. encode() is contractually
/// blocking, so it pumps those events on the calling thread until the frame it
/// fed comes back — with a timeout, because a transform that has decided to
/// buffer a picture would otherwise hold the capture thread forever. Such a
/// transform costs one frame of latency and one log line; it does not kill the
/// session.
///
/// ── What it does not do ─────────────────────────────────────────────────────
///
/// No intra-refresh and no reference invalidation: Media Foundation exposes
/// neither. A lost frame costs a keyframe here, as it did on every encoder
/// before E2. No HDR and no 4:4:4 — the Selector never grants them on the
/// fallback tier, and the transforms take NV12 alone.
class MfEncoder final : public IVideoEncoder
{
public:
    /// Both defined in the .cpp: EventSink is complete only there, and a
    /// constructor inlined elsewhere would have to know how to unwind it.
    MfEncoder();
    ~MfEncoder() override;

    bool init(ID3D11Device* device, Codec codec, int width, int height, int fps, int bitrateKbps,
              bool yuv444, bool hdr, bool intraRefresh, const EncoderTuning& tuning,
              std::string& error) override;

    bool encode(ID3D11Texture2D* surface, bool forceKeyframe, uint32_t frameNumber,
                EncoderOutput& out, std::string& error) override;

    void releaseOutput() override;
    void stop() override;
    bool setBitrate(int bitrateKbps, std::string& error) override;
    bool intraRefreshEnabled() const override { return false; }

private:
    /// Receives the transform's events on Media Foundation's own thread and
    /// hands them to encode(), which waits on the calling thread. The
    /// indirection exists for one reason: IMFMediaEventGenerator::GetEvent has
    /// no timeout, and a transform that never answers must not hold the capture
    /// thread forever.
    class EventSink;

    /// Open, configure, start and PROVE one transform — the next tier down is
    /// tried when a hardware one fails here.
    bool bringUp(Codec codec, int width, int height, int bitrateKbps, const EncoderTuning& tuning,
                 std::string& error);
    /// Encode a black picture and insist on getting something back.
    bool proveWithOnePicture(std::string& error);
    /// The DLL behind the current transform and its date, for the one message
    /// where that is the whole answer: "" when it cannot be resolved.
    std::string describeModule() const;
    void teardownTransform();
    bool openTransform(Codec codec, std::string& error);
    bool configureTypes(Codec codec, int width, int height, int fps, int bitrateKbps,
                        std::string& error);
    void applyCodecOptions(int bitrateKbps, int fps, bool hardware, const EncoderTuning& tuning);
    bool attachDevice(ID3D11Device* device);
    bool makeInputSample(ID3D11Texture2D* surface, uint32_t frameNumber,
                         Microsoft::WRL::ComPtr<IMFSample>& sample, std::string& error);
    bool readbackToCpu(ID3D11Texture2D* surface, std::string& error);
    /// Returns S_OK with a sample, MF_E_TRANSFORM_NEED_MORE_INPUT when the
    /// transform has nothing yet, or another failure.
    HRESULT takeOutput(Microsoft::WRL::ComPtr<IMFSample>& sample, std::string& error);
    bool waitForEvent(MediaEventType wanted, unsigned timeoutMs, HRESULT& eventStatus);

    const MfApi* m_Api = nullptr;
    bool m_Started = false;
    Microsoft::WRL::ComPtr<IMFTransform> m_Transform;
    Microsoft::WRL::ComPtr<ICodecAPI> m_CodecApi;
    Microsoft::WRL::ComPtr<IMFMediaEventGenerator> m_Events;
    Microsoft::WRL::ComPtr<EventSink> m_Sink;
    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> m_DeviceManager;
    UINT m_ResetToken = 0;

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
    /// The CPU path's staging texture and the contiguous NV12 it is copied into.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Staging;
    std::vector<uint8_t> m_Cpu;

    /// The sample handed out by the last encode(), locked until releaseOutput().
    Microsoft::WRL::ComPtr<IMFSample> m_OutSample;
    Microsoft::WRL::ComPtr<IMFMediaBuffer> m_OutBuffer;

    std::string m_Name;
    /// The transform's class id, kept from the activation object so the module
    /// behind it can be named if it misbehaves. See describeModule().
    GUID m_Clsid = {};
    bool m_Hardware = false;
    bool m_Async = false;
    bool m_D3dAware = false;
    bool m_ProvidesSamples = false;
    bool m_SkipHardware = false;
    DWORD m_OutputSize = 0;
    DWORD m_InputId = 0;
    DWORD m_OutputId = 0;
    /// Asynchronous transforms grant input one picture at a time; this counts
    /// the METransformNeedInput events not yet spent on a ProcessInput.
    int m_InputCredits = 0;

    int m_Width = 0;
    int m_Height = 0;
    int m_Fps = 60;
    LONGLONG m_FrameDuration = 0;
    bool m_HeldFrameLogged = false;
    /// The trial pictures of init() consumed the transform's first keyframe;
    /// the session's first real picture must be one again.
    bool m_KeyframeNext = false;
    int m_FramesIn = 0;
    int m_FramesOut = 0;
};

} // namespace mw::native::encode
