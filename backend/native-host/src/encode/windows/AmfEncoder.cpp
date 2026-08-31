/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "AmfEncoder.h"

#include "../../core/Log.h"

#include <components/VideoEncoderAV1.h>
#include <components/VideoEncoderHEVC.h>
#include <components/VideoEncoderVCE.h>

#include <thread>

namespace mw::native::encode {
namespace {

/// AMF names the same concept differently per codec — `TargetBitrate`,
/// `HevcTargetBitrate`, `Av1TargetBitrate`. Gathering the names here lets the
/// configuration below be written once instead of three times, and makes a
/// missing property obvious rather than buried in a third copy of the logic.
struct CodecProperties
{
    const wchar_t* component;
    const wchar_t* usage;
    amf_int64 usageUltraLowLatency;
    const wchar_t* rateControl;
    amf_int64 rateControlCbr;
    const wchar_t* targetBitrate;
    const wchar_t* peakBitrate;
    const wchar_t* vbvBufferSize;
    const wchar_t* frameSize;
    const wchar_t* frameRate;
    const wchar_t* gopSize;
    const wchar_t* forcePictureType;
    amf_int64 forcePictureTypeIdr;
    const wchar_t* outputDataType;
    amf_int64 outputDataTypeIdr;
    /// Makes QueryOutput block instead of returning AMF_REPEAT. Setting it is
    /// what turns the wait for a frame into an actual wait — see init().
    const wchar_t* queryTimeout;
};

const CodecProperties& propertiesFor(Codec codec)
{
    static const CodecProperties kH264 = {
        AMFVideoEncoderVCE_AVC,
        AMF_VIDEO_ENCODER_USAGE,
        AMF_VIDEO_ENCODER_USAGE_ULTRA_LOW_LATENCY,
        AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD,
        AMF_VIDEO_ENCODER_RATE_CONTROL_METHOD_CBR,
        AMF_VIDEO_ENCODER_TARGET_BITRATE,
        AMF_VIDEO_ENCODER_PEAK_BITRATE,
        AMF_VIDEO_ENCODER_VBV_BUFFER_SIZE,
        AMF_VIDEO_ENCODER_FRAMESIZE,
        AMF_VIDEO_ENCODER_FRAMERATE,
        AMF_VIDEO_ENCODER_IDR_PERIOD,
        AMF_VIDEO_ENCODER_FORCE_PICTURE_TYPE,
        AMF_VIDEO_ENCODER_PICTURE_TYPE_IDR,
        AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE,
        AMF_VIDEO_ENCODER_OUTPUT_DATA_TYPE_IDR,
        AMF_VIDEO_ENCODER_QUERY_TIMEOUT,
    };
    static const CodecProperties kHevc = {
        AMFVideoEncoder_HEVC,
        AMF_VIDEO_ENCODER_HEVC_USAGE,
        AMF_VIDEO_ENCODER_HEVC_USAGE_ULTRA_LOW_LATENCY,
        AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD,
        AMF_VIDEO_ENCODER_HEVC_RATE_CONTROL_METHOD_CBR,
        AMF_VIDEO_ENCODER_HEVC_TARGET_BITRATE,
        AMF_VIDEO_ENCODER_HEVC_PEAK_BITRATE,
        AMF_VIDEO_ENCODER_HEVC_VBV_BUFFER_SIZE,
        AMF_VIDEO_ENCODER_HEVC_FRAMESIZE,
        AMF_VIDEO_ENCODER_HEVC_FRAMERATE,
        AMF_VIDEO_ENCODER_HEVC_GOP_SIZE,
        AMF_VIDEO_ENCODER_HEVC_FORCE_PICTURE_TYPE,
        AMF_VIDEO_ENCODER_HEVC_PICTURE_TYPE_IDR,
        AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE,
        AMF_VIDEO_ENCODER_HEVC_OUTPUT_DATA_TYPE_IDR,
        AMF_VIDEO_ENCODER_HEVC_QUERY_TIMEOUT,
    };
    static const CodecProperties kAv1 = {
        AMFVideoEncoder_AV1,
        AMF_VIDEO_ENCODER_AV1_USAGE,
        AMF_VIDEO_ENCODER_AV1_USAGE_ULTRA_LOW_LATENCY,
        AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD,
        AMF_VIDEO_ENCODER_AV1_RATE_CONTROL_METHOD_CBR,
        AMF_VIDEO_ENCODER_AV1_TARGET_BITRATE,
        AMF_VIDEO_ENCODER_AV1_PEAK_BITRATE,
        AMF_VIDEO_ENCODER_AV1_VBV_BUFFER_SIZE,
        AMF_VIDEO_ENCODER_AV1_FRAMESIZE,
        AMF_VIDEO_ENCODER_AV1_FRAMERATE,
        AMF_VIDEO_ENCODER_AV1_GOP_SIZE,
        AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE,
        AMF_VIDEO_ENCODER_AV1_FORCE_FRAME_TYPE_KEY,
        AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE,
        AMF_VIDEO_ENCODER_AV1_OUTPUT_FRAME_TYPE_KEY,
        AMF_VIDEO_ENCODER_AV1_QUERY_TIMEOUT,
    };

    switch (codec) {
    case Codec::Av1: return kAv1;
    case Codec::Hevc: return kHevc;
    case Codec::H264: break;
    }
    return kH264;
}

/// Long enough that no periodic keyframe ever lands in a session anyone will
/// sit through. A keyframe is a bitrate spike, and on a congested link the
/// spike causes the loss that provokes the request for another — so they are
/// emitted only on demand, when a client says it cannot go on.
constexpr amf_int64 kEffectivelyInfiniteGop = 1 << 20;

/// Frames for one full intra-refresh sweep. Two seconds at 60 fps — long
/// enough that the per-frame cost stays small, short enough that a receiver
/// riding out a loss is whole again before anyone reads it as a fault.
/// Deliberately the same figure the NVENC path uses, so the two vendors behave
/// alike from the receiver's side.
constexpr amf_int64 kIntraRefreshPeriodFrames = 120;

/// How long QueryOutput may block waiting for a frame, in milliseconds.
///
/// A deadline, not a schedule: encoding takes a few milliseconds, and reaching
/// this means the encoder has stopped answering. Generous enough that a busy
/// GPU is never cut off mid-frame.
constexpr int kQueryTimeoutMs = 100;

} // namespace

AmfEncoder::~AmfEncoder()
{
    stop();
}

bool AmfEncoder::init(ID3D11Device* device, Codec codec, int width, int height, int fps,
                      int bitrateKbps, bool yuv444, bool intraRefresh, std::string& error)
{
    stop();

    m_Api = AmfApi::instance();
    if (!m_Api->available()) {
        error = m_Api->unavailableReason();
        return false;
    }
    if (!device || width <= 0 || height <= 0) {
        error = "invalid encoder parameters";
        return false;
    }
    if (yuv444) {
        // Not claimed by the capability query either, so the Selector should
        // never route a 4:4:4 session here. Refusing loudly beats encoding
        // 4:2:0 while the overlay says 4:4:4.
        error = "4:4:4 is not implemented on the AMD encoder path";
        return false;
    }

    m_Codec = codec;
    m_Width = width;
    m_Height = height;
    m_Fps = fps > 0 ? fps : 60;
    if (bitrateKbps <= 0) bitrateKbps = 20000;

    const CodecProperties& props = propertiesFor(codec);

    AMF_RESULT result = m_Api->factory()->CreateContext(&m_Context);
    if (result != AMF_OK || !m_Context) {
        error = std::string("could not create an AMF context: ") + AmfApi::resultToString(result);
        return false;
    }

    // Bind to the caller's device, which is the adapter that captured the
    // frame. This is what makes the surface hand-off below a zero-copy one.
    result = m_Context->InitDX11(device);
    if (result != AMF_OK) {
        error = std::string("AMF could not use this GPU: ") + AmfApi::resultToString(result);
        stop();
        return false;
    }

    result = m_Api->factory()->CreateComponent(m_Context, props.component, &m_Encoder);
    if (result != AMF_OK || !m_Encoder) {
        error = std::string("this GPU has no ") + toString(codec) +
                " encoder: " + AmfApi::resultToString(result);
        stop();
        return false;
    }

    // USAGE first, and deliberately: AMF documents it as configuring the whole
    // parameter set, so anything set before it is overwritten.
    m_Encoder->SetProperty(props.usage, props.usageUltraLowLatency);

    // CBR with a one-frame VBV. The VBV is what actually enforces low latency:
    // it caps how far ahead the encoder may spend, so no single frame can be so
    // large that it takes several frame times to transmit.
    const amf_int64 bitsPerSecond = static_cast<amf_int64>(bitrateKbps) * 1000;
    m_Encoder->SetProperty(props.rateControl, props.rateControlCbr);
    m_Encoder->SetProperty(props.targetBitrate, bitsPerSecond);
    m_Encoder->SetProperty(props.peakBitrate, bitsPerSecond);
    m_Encoder->SetProperty(props.vbvBufferSize, bitsPerSecond / m_Fps);

    m_Encoder->SetProperty(props.frameSize, ::AMFConstructSize(width, height));
    m_Encoder->SetProperty(props.frameRate, ::AMFConstructRate(m_Fps, 1));
    m_Encoder->SetProperty(props.gopSize, kEffectivelyInfiniteGop);

    // Make QueryOutput block rather than answer AMF_REPEAT.
    //
    // This is worth 12 ms per frame, measured. Polling with a 100 µs sleep
    // looked reasonable and was not: Windows' default timer resolution is
    // 15.6 ms, so every "100 µs" nap was really a full tick and the encoder
    // read 15.66 ms per frame on an RX 7600 — a number that says nothing about
    // the silicon and everything about the wait. Raising the process timer
    // resolution would fix the symptom while imposing a global side effect
    // from inside a library; letting AMF do the waiting fixes the cause.
    m_Encoder->SetProperty(props.queryTimeout, amf_int64(kQueryTimeoutMs));

    // No B-frames: one would make the encoder hold a frame back to reference a
    // picture that has not been sent — a whole frame of latency. Only H.264
    // exposes the knob; the other two emit none under ultra-low-latency.
    if (codec == Codec::H264) m_Encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf_int64(0));

    // ── Intra-refresh, where asked ──────────────────────────────────────────
    //
    // AMD expresses the wave as "how much of the picture to refresh per frame"
    // rather than as a period, so the count is derived from the frame size: a
    // full sweep in kIntraRefreshPeriodFrames frames.
    m_IntraRefresh = false;
    if (intraRefresh) {
        switch (codec) {
        case Codec::H264: {
            // 16×16 macroblocks.
            const amf_int64 total = ((width + 15) / 16) * ((height + 15) / 16);
            const amf_int64 perSlot = total / kIntraRefreshPeriodFrames;
            m_Encoder->SetProperty(AMF_VIDEO_ENCODER_INTRA_REFRESH_NUM_MBS_PER_SLOT,
                                   perSlot > 0 ? perSlot : 1);
            m_IntraRefresh = true;
            break;
        }
        case Codec::Hevc: {
            // 64×64 coding tree blocks.
            const amf_int64 total = ((width + 63) / 64) * ((height + 63) / 64);
            const amf_int64 perSlot = total / kIntraRefreshPeriodFrames;
            m_Encoder->SetProperty(AMF_VIDEO_ENCODER_HEVC_INTRA_REFRESH_NUM_CTBS_PER_SLOT,
                                   perSlot > 0 ? perSlot : 1);
            m_IntraRefresh = true;
            break;
        }
        case Codec::Av1: {
            m_Encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INTRA_REFRESH_MODE,
                                   amf_int64(AMF_VIDEO_ENCODER_AV1_INTRA_REFRESH_MODE__CONTINUOUS));
            // AV1 counts stripes rather than blocks: one stripe per frame of
            // the cycle sweeps the picture over the same period.
            m_Encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INTRAREFRESH_STRIPES,
                                   amf_int64(kIntraRefreshPeriodFrames));
            m_IntraRefresh = true;
            break;
        }
        }
    }

    // AV1 has an explicit latency mode, and the fastest one is the point here.
    if (codec == Codec::Av1)
        m_Encoder->SetProperty(
            AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE,
            amf_int64(AMF_VIDEO_ENCODER_AV1_ENCODING_LATENCY_MODE_LOWEST_LATENCY));

    result = m_Encoder->Init(amf::AMF_SURFACE_NV12, width, height);
    if (result != AMF_OK) {
        error =
            std::string("could not initialize the AMD encoder: ") + AmfApi::resultToString(result);
        stop();
        return false;
    }

    log::info("[native] AMF ready: " + std::to_string(width) + "x" + std::to_string(height) + "@" +
              std::to_string(m_Fps) + " " + toString(codec) + " 4:2:0 CBR " +
              std::to_string(bitrateKbps) + " kbps" +
              (m_IntraRefresh ? ", intra-refresh" : ", keyframes on demand"));
    return true;
}

bool AmfEncoder::encode(ID3D11Texture2D* surface, bool forceKeyframe, EncoderOutput& out,
                        std::string& error)
{
    if (!m_Encoder || !m_Context) {
        error = "the encoder is not initialized";
        return false;
    }
    if (m_Output) {
        error = "the previous frame was not released";
        return false;
    }

    // The zero-copy step: AMF wraps the very texture the conversion pass wrote,
    // on the same adapter, with no staging buffer.
    amf::AMFSurfacePtr input;
    AMF_RESULT result = m_Context->CreateSurfaceFromDX11Native(surface, &input, nullptr);
    if (result != AMF_OK || !input) {
        error = std::string("could not wrap the input surface: ") + AmfApi::resultToString(result);
        return false;
    }

    const CodecProperties& props = propertiesFor(m_Codec);
    if (forceKeyframe) input->SetProperty(props.forcePictureType, props.forcePictureTypeIdr);

    result = m_Encoder->SubmitInput(input);
    if (result != AMF_OK) {
        error = std::string("encode submit failed: ") + AmfApi::resultToString(result);
        return false;
    }

    // QueryTimeout (set in init) makes this block until the frame is ready, so
    // the wait costs exactly the encode and not a timer tick. A driver that
    // ignores the property still answers AMF_REPEAT, so the retry below stays —
    // it is now the exception rather than the mechanism.
    amf::AMFDataPtr data;
    for (int attempt = 0; attempt < 200; ++attempt) {
        result = m_Encoder->QueryOutput(&data);
        if (result == AMF_OK && data) break;
        if (result != AMF_REPEAT && result != AMF_OK) {
            error = std::string("encode failed: ") + AmfApi::resultToString(result);
            return false;
        }
        // Yield rather than sleep: a sleep here is what cost 15 ms a frame.
        std::this_thread::yield();
    }
    if (!data) {
        error = "the AMD encoder stopped producing frames";
        return false;
    }

    m_Output = amf::AMFBufferPtr(data);
    if (!m_Output) {
        error = "the encoder returned something that was not a buffer";
        return false;
    }

    amf_int64 dataType = 0;
    const bool haveType = data->GetProperty(props.outputDataType, &dataType) == AMF_OK;

    out.data = static_cast<const uint8_t*>(m_Output->GetNative());
    out.size = m_Output->GetSize();
    // When the encoder does not say, trust what was asked for: a forced
    // keyframe that reported itself as a delta would close the relay's delta
    // gate and freeze the picture.
    out.keyframe = haveType ? (dataType == props.outputDataTypeIdr) : forceKeyframe;
    return true;
}

void AmfEncoder::releaseOutput()
{
    m_Output = nullptr;
}

bool AmfEncoder::setBitrate(int bitrateKbps, std::string& error)
{
    if (!m_Encoder || bitrateKbps <= 0) {
        error = "the encoder is not initialized";
        return false;
    }

    const CodecProperties& props = propertiesFor(m_Codec);
    const amf_int64 bitsPerSecond = static_cast<amf_int64>(bitrateKbps) * 1000;

    // Applied live, with no restart — the basis for following the client's real
    // feedback frame by frame.
    m_Encoder->SetProperty(props.targetBitrate, bitsPerSecond);
    m_Encoder->SetProperty(props.peakBitrate, bitsPerSecond);
    m_Encoder->SetProperty(props.vbvBufferSize, bitsPerSecond / m_Fps);
    return true;
}

void AmfEncoder::stop()
{
    releaseOutput();
    if (m_Encoder) {
        m_Encoder->Terminate();
        m_Encoder = nullptr;
    }
    if (m_Context) {
        m_Context->Terminate();
        m_Context = nullptr;
    }
}

} // namespace mw::native::encode
