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
#include "../RateControl.h"

#include <components/VideoEncoderAV1.h>
#include <components/VideoEncoderHEVC.h>
#include <components/VideoEncoderVCE.h>

#include <algorithm>
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
    /// Asks the encoder to attach its per-frame statistics to the output, and
    /// the one statistic read back: the average quantizer (a q-index on AV1).
    const wchar_t* statisticsFeedback;
    const wchar_t* statisticAvgQp;
    /// The bench's knobs. The quality preset's three values are per codec (the
    /// enums do not even agree on which way round the scale runs); adaptive
    /// quantization is a bool on H.264/HEVC (VBAQ) and a mode on AV1 (CAQ).
    const wchar_t* qualityPreset;
    amf_int64 qualitySpeed;
    amf_int64 qualityBalanced;
    amf_int64 qualityQuality;
    const wchar_t* preAnalysis;
    const wchar_t* adaptiveQuant;
    bool adaptiveQuantIsMode;
    /// Long-term references, for reference invalidation (class comment): the
    /// slot count and mode on the encoder, the mark and the forced bitfield on
    /// each input surface, and the driver's answers on the output buffer.
    const wchar_t* maxLtrFrames;
    const wchar_t* ltrMode;
    amf_int64 ltrModeResetUnused;
    const wchar_t* markLtrIndex;
    const wchar_t* forceLtrBitfield;
    const wchar_t* outputMarkedLtrIndex;
    const wchar_t* outputReferencedLtrBitfield;
    /// Parameter sets in the bitstream, which decide whether a browser can
    /// configure its decoder at all.
    ///
    /// NVENC has `repeatSPSPPS = 1` and that is the end of it. AMF has no such
    /// switch on by default: its own headers say `InsertSPS`/`InsertPPS`
    /// default to **false**, the HEVC and AV1 insertion modes default to
    /// **NONE**, and the H.264 spacing "depends on USAGE". So the parameter
    /// sets reach the wire once, if at all — and a client that misses that one
    /// frame never sees them again. That is not a rare case: whenever the data
    /// channel is not open yet the relay buffers a keyframe and sends a LATER
    /// one, so the browser's first frame is routinely not the first keyframe.
    /// It then warns "cannot extract SPS/PPS" on every delta and the picture
    /// never starts.
    ///
    /// HEVC and AV1 take a mode, set once at init. H.264 has no IDR-aligned
    /// mode — only a periodic spacing, which is not the same promise — so it
    /// asks per picture instead, which is exact: with an effectively infinite
    /// GOP every IDR is one this class forced.
    const wchar_t* headerInsertionMode; ///< null on H.264
    amf_int64 headerInsertionOnKeyframe;
    const wchar_t* insertSps; ///< null on HEVC/AV1
    const wchar_t* insertPps;
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
        AMF_VIDEO_ENCODER_STATISTICS_FEEDBACK,
        AMF_VIDEO_ENCODER_STATISTIC_AVERAGE_QP,
        AMF_VIDEO_ENCODER_QUALITY_PRESET,
        AMF_VIDEO_ENCODER_QUALITY_PRESET_SPEED,
        AMF_VIDEO_ENCODER_QUALITY_PRESET_BALANCED,
        AMF_VIDEO_ENCODER_QUALITY_PRESET_QUALITY,
        AMF_VIDEO_ENCODER_PRE_ANALYSIS_ENABLE,
        AMF_VIDEO_ENCODER_ENABLE_VBAQ,
        false,
        AMF_VIDEO_ENCODER_MAX_LTR_FRAMES,
        AMF_VIDEO_ENCODER_LTR_MODE,
        AMF_VIDEO_ENCODER_LTR_MODE_RESET_UNUSED,
        AMF_VIDEO_ENCODER_MARK_CURRENT_WITH_LTR_INDEX,
        AMF_VIDEO_ENCODER_FORCE_LTR_REFERENCE_BITFIELD,
        AMF_VIDEO_ENCODER_OUTPUT_MARKED_LTR_INDEX,
        AMF_VIDEO_ENCODER_OUTPUT_REFERENCED_LTR_INDEX_BITFIELD,
        nullptr, // no IDR-aligned mode on H.264: asked for per picture below
        0,
        AMF_VIDEO_ENCODER_INSERT_SPS,
        AMF_VIDEO_ENCODER_INSERT_PPS,
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
        AMF_VIDEO_ENCODER_HEVC_STATISTICS_FEEDBACK,
        AMF_VIDEO_ENCODER_HEVC_STATISTIC_AVERAGE_QP,
        AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET,
        AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_SPEED,
        AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_BALANCED,
        AMF_VIDEO_ENCODER_HEVC_QUALITY_PRESET_QUALITY,
        AMF_VIDEO_ENCODER_HEVC_PRE_ANALYSIS_ENABLE,
        AMF_VIDEO_ENCODER_HEVC_ENABLE_VBAQ,
        false,
        AMF_VIDEO_ENCODER_HEVC_MAX_LTR_FRAMES,
        AMF_VIDEO_ENCODER_HEVC_LTR_MODE,
        AMF_VIDEO_ENCODER_HEVC_LTR_MODE_RESET_UNUSED,
        AMF_VIDEO_ENCODER_HEVC_MARK_CURRENT_WITH_LTR_INDEX,
        AMF_VIDEO_ENCODER_HEVC_FORCE_LTR_REFERENCE_BITFIELD,
        AMF_VIDEO_ENCODER_HEVC_OUTPUT_MARKED_LTR_INDEX,
        AMF_VIDEO_ENCODER_HEVC_OUTPUT_REFERENCED_LTR_INDEX_BITFIELD,
        AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE,
        AMF_VIDEO_ENCODER_HEVC_HEADER_INSERTION_MODE_IDR_ALIGNED,
        nullptr,
        nullptr,
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
        AMF_VIDEO_ENCODER_AV1_STATISTICS_FEEDBACK,
        AMF_VIDEO_ENCODER_AV1_STATISTIC_AVERAGE_Q_INDEX,
        AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET,
        AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_SPEED,
        AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_BALANCED,
        AMF_VIDEO_ENCODER_AV1_QUALITY_PRESET_QUALITY,
        AMF_VIDEO_ENCODER_AV1_PRE_ANALYSIS_ENABLE,
        AMF_VIDEO_ENCODER_AV1_AQ_MODE,
        true,
        AMF_VIDEO_ENCODER_AV1_MAX_LTR_FRAMES,
        AMF_VIDEO_ENCODER_AV1_LTR_MODE,
        AMF_VIDEO_ENCODER_AV1_LTR_MODE_RESET_UNUSED,
        AMF_VIDEO_ENCODER_AV1_MARK_CURRENT_WITH_LTR_INDEX,
        AMF_VIDEO_ENCODER_AV1_FORCE_LTR_REFERENCE_BITFIELD,
        AMF_VIDEO_ENCODER_AV1_OUTPUT_MARKED_LTR_INDEX,
        AMF_VIDEO_ENCODER_AV1_OUTPUT_REFERENCED_LTR_INDEX_BITFIELD,
        AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE,
        AMF_VIDEO_ENCODER_AV1_HEADER_INSERTION_MODE_KEY_FRAME_ALIGNED,
        nullptr,
        nullptr,
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

/// Long-term reference slots asked of the driver: the same depth as NVENC's
/// DPB (NvencEncoder), for the same reason — enough places to predict from
/// once one of them is lost. The driver answers with what it can hold, and
/// that answer is what the table is sized to.
constexpr int kLtrSlots = 4;

/// How long QueryOutput may block waiting for a frame, in milliseconds.
///
/// A deadline, not a schedule: encoding takes a few milliseconds, and reaching
/// this means the encoder has stopped answering. Generous enough that a busy
/// GPU is never cut off mid-frame.
constexpr int kQueryTimeoutMs = 100;

/// Name the quality preset the encoder is running with, in the codec's own
/// scale — for the log, so a bench line says what the USAGE chose.
std::string qualityName(const CodecProperties& props, amf_int64 value)
{
    if (value == props.qualitySpeed) return "speed";
    if (value == props.qualityBalanced) return "balanced";
    if (value == props.qualityQuality) return "quality";
    return "value " + std::to_string(value);
}

} // namespace

AmfEncoder::~AmfEncoder()
{
    stop();
}

bool AmfEncoder::init(ID3D11Device* device, Codec codec, int width, int height, int fps,
                      int bitrateKbps, bool yuv444, bool hdr, bool intraRefresh,
                      const EncoderTuning& tuning, std::string& error)
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
    if (hdr) {
        // AMF does 10-bit HEVC (AMF_VIDEO_ENCODER_HEVC_PROFILE_MAIN_10) and the
        // surface would be the same P010 the shader writes, but none of it has
        // been exercised: there is no HDR display on an AMD machine here, and
        // shipping a colour path nobody has looked at is how you get a stream
        // that is subtly wrong for months. The capability query does not claim
        // 10-bit on this path, so the Selector routes HDR elsewhere or runs
        // SDR — this is the guard for a misrouted session, and the line to
        // delete first when an AMD HDR bench exists.
        error = "HDR is not implemented on the AMD encoder path";
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

    // What the usage chose for the knobs the bench can move, before it moves
    // them. Read back rather than assumed: AMF documents every one of these as
    // "default = depends on USAGE", and the bench compares against them.
    amf_int64 usageQuality = -1;
    amf_int64 usagePreAnalysis = -1;
    amf_int64 usageAq = -1;
    m_Encoder->GetProperty(props.qualityPreset, &usageQuality);
    m_Encoder->GetProperty(props.preAnalysis, &usagePreAnalysis);
    m_Encoder->GetProperty(props.adaptiveQuant, &usageAq);
    log::info(std::string("[native] AMF ultra-low-latency usage as the driver ships it: quality=") +
              qualityName(props, usageQuality) + " preanalysis=" +
              std::to_string(usagePreAnalysis) + " aq=" + std::to_string(usageAq));

    // ── The bench's overrides, where it gave any ────────────────────────────
    switch (tuning.amfQuality) {
    case EncoderTuning::AmfQuality::Speed:
        m_Encoder->SetProperty(props.qualityPreset, props.qualitySpeed);
        break;
    case EncoderTuning::AmfQuality::Balanced:
        m_Encoder->SetProperty(props.qualityPreset, props.qualityBalanced);
        break;
    case EncoderTuning::AmfQuality::Quality:
        m_Encoder->SetProperty(props.qualityPreset, props.qualityQuality);
        break;
    case EncoderTuning::AmfQuality::Default: break;
    }
    if (tuning.preAnalysis != EncoderTuning::Choice::Default)
        m_Encoder->SetProperty(props.preAnalysis, tuning.preAnalysis == EncoderTuning::Choice::On);
    if (tuning.spatialAq != EncoderTuning::Choice::Default) {
        const bool on = tuning.spatialAq == EncoderTuning::Choice::On;
        if (props.adaptiveQuantIsMode)
            m_Encoder->SetProperty(props.adaptiveQuant,
                                   amf_int64(on ? AMF_VIDEO_ENCODER_AV1_AQ_MODE_CAQ
                                                : AMF_VIDEO_ENCODER_AV1_AQ_MODE_NONE));
        else
            m_Encoder->SetProperty(props.adaptiveQuant, on);
    }
    m_VbvFrames = tuning.vbvFrames;

    // CBR. The VBV is what actually enforces low latency: it caps how far ahead
    // the encoder may spend, so no single frame can be so large that it takes
    // several frame times to transmit. See RateControl.h for why it has a floor
    // rather than being one frame at any refresh rate.
    const amf_int64 bitsPerSecond = static_cast<amf_int64>(bitrateKbps) * 1000;
    const amf_int64 vbvBitsNow =
        static_cast<amf_int64>(vbvBits(static_cast<uint32_t>(bitsPerSecond), m_Fps, m_VbvFrames));
    m_Encoder->SetProperty(props.rateControl, props.rateControlCbr);
    m_Encoder->SetProperty(props.targetBitrate, bitsPerSecond);
    m_Encoder->SetProperty(props.peakBitrate, bitsPerSecond);
    m_Encoder->SetProperty(props.vbvBufferSize, vbvBitsNow);

    m_Encoder->SetProperty(props.frameSize, ::AMFConstructSize(width, height));
    m_Encoder->SetProperty(props.frameRate, ::AMFConstructRate(m_Fps, 1));
    m_Encoder->SetProperty(props.gopSize, kEffectivelyInfiniteGop);

    // Parameter sets on every keyframe, never once (see CodecProperties). With
    // the GOP effectively infinite there are no IDRs but the ones this class
    // forces, so the per-picture route on H.264 covers all of them.
    if (props.headerInsertionMode)
        m_Encoder->SetProperty(props.headerInsertionMode, props.headerInsertionOnKeyframe);

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

    // Per-frame statistics on the output buffer, for the average QP the
    // benchmarks read as their quality proxy. A handful of integers per frame;
    // the encoder computes them anyway.
    m_Encoder->SetProperty(props.statisticsFeedback, true);

    // No B-frames: one would make the encoder hold a frame back to reference a
    // picture that has not been sent — a whole frame of latency. Only H.264
    // exposes the knob; the other two emit none under ultra-low-latency.
    if (codec == Codec::H264) m_Encoder->SetProperty(AMF_VIDEO_ENCODER_B_PIC_PATTERN, amf_int64(0));

    // ── Intra-refresh, where asked ──────────────────────────────────────────
    //
    // AMD expresses the wave as "how much of the picture to refresh per frame"
    // rather than as a period, so the count is derived from the frame size: a
    // full sweep in two seconds' worth of frames at the rate they are really
    // encoded (RateControl.h). Integer division rounds the slot DOWN, so the
    // sweep can only come out a little longer than the period, never shorter.
    m_IntraRefresh = false;
    const amf_int64 refreshPeriod = intraRefreshPeriodFrames(m_Fps);
    if (intraRefresh) {
        switch (codec) {
        case Codec::H264: {
            // 16×16 macroblocks.
            const amf_int64 total = ((width + 15) / 16) * ((height + 15) / 16);
            const amf_int64 perSlot = total / refreshPeriod;
            m_Encoder->SetProperty(AMF_VIDEO_ENCODER_INTRA_REFRESH_NUM_MBS_PER_SLOT,
                                   perSlot > 0 ? perSlot : 1);
            m_IntraRefresh = true;
            break;
        }
        case Codec::Hevc: {
            // 64×64 coding tree blocks.
            const amf_int64 total = ((width + 63) / 64) * ((height + 63) / 64);
            const amf_int64 perSlot = total / refreshPeriod;
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
            m_Encoder->SetProperty(AMF_VIDEO_ENCODER_AV1_INTRAREFRESH_STRIPES, refreshPeriod);
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

    // ── Long-term reference slots, for reference invalidation ───────────────
    // Asked before Init(), read back after it: "default = 0" and the driver
    // may grant fewer than asked. `dpb=1` from the bench is "one reference,
    // nothing to heal with" — the same "before" as on NVENC. RESET_UNUSED is
    // the mode the repair relies on: a forced bitfield drops the slots it does
    // not name, which are exactly the tainted ones.
    m_Ltr = ReferenceSlots(0);
    m_LostPending = false;
    m_Invalidations = 0;
    m_MarkRefusedLogged = m_ForceKeyframeNext = false;
    m_HealsLogged = 0;
    const int ltrAsked =
        tuning.dpbFrames > 0 ? (tuning.dpbFrames > 1 ? tuning.dpbFrames : 0) : kLtrSlots;
    if (ltrAsked > 0) {
        m_Encoder->SetProperty(props.maxLtrFrames, amf_int64(ltrAsked));
        m_Encoder->SetProperty(props.ltrMode, props.ltrModeResetUnused);
    }

    result = m_Encoder->Init(amf::AMF_SURFACE_NV12, width, height);
    if (result != AMF_OK) {
        error =
            std::string("could not initialize the AMD encoder: ") + AmfApi::resultToString(result);
        stop();
        return false;
    }

    amf_int64 ltrGranted = 0;
    if (ltrAsked > 0 && m_Encoder->GetProperty(props.maxLtrFrames, &ltrGranted) == AMF_OK &&
        ltrGranted > 0) {
        const int slots =
            static_cast<int>(std::min<amf_int64>(ltrGranted, ReferenceSlots::kMaxSlots));
        m_Ltr = ReferenceSlots(slots, ReferenceSlots::strideFor(m_Fps, slots));
    }

    // The knobs as the encoder holds them now — usage, then overrides, then
    // Init(), which may have corrected any of them.
    amf_int64 quality = -1, preAnalysis = -1, aq = -1;
    m_Encoder->GetProperty(props.qualityPreset, &quality);
    m_Encoder->GetProperty(props.preAnalysis, &preAnalysis);
    m_Encoder->GetProperty(props.adaptiveQuant, &aq);
    const std::string overrides = tuning.describe();
    log::info("[native] AMF ready: " + std::to_string(width) + "x" + std::to_string(height) + "@" +
              std::to_string(m_Fps) + " " + toString(codec) + " 4:2:0 CBR " +
              std::to_string(bitrateKbps) + " kbps, VBV " + std::to_string(vbvBitsNow / 8 / 1024) +
              " KB" +
              (m_IntraRefresh ? ", intra-refresh over " + std::to_string(refreshPeriod) + " frames"
                              : ", keyframes on demand") +
              ", quality=" + qualityName(props, quality) +
              " preanalysis=" + std::to_string(preAnalysis) + " aq=" + std::to_string(aq) +
              (m_Ltr.enabled() ? ", " + std::to_string(m_Ltr.count()) + " LTR slots every " +
                                     std::to_string(m_Ltr.stride()) +
                                     " frames with reference invalidation (reach " +
                                     std::to_string(m_Ltr.reachFrames()) + " frames)"
                               : ", no reference invalidation") +
              (overrides.empty() ? "" : " [bench: " + overrides + "]"));
    return true;
}

bool AmfEncoder::invalidateReference(uint32_t frameNumber, std::string& error)
{
    if (!m_Encoder) {
        error = "the encoder is not initialized";
        return false;
    }
    if (!m_Ltr.enabled()) {
        error = "this AMD encoder granted no long-term reference slots";
        return false;
    }
    // Everything from the lost frame on predicts from it; two losses reported
    // between two encodes fold into the older one. The check is made now, on
    // the table as it stands, because the caller forces a keyframe on a
    // refusal — and the table cannot change before the next encode() applies
    // the answer (both run on the capture thread).
    const uint32_t from = m_LostPending ? std::min(m_LostFrom, frameNumber) : frameNumber;
    if (m_Ltr.cleanSlotBefore(from) < 0) {
        error = "no long-term reference older than frame " + std::to_string(from) +
                " is held — the loss is beyond the " + std::to_string(m_Ltr.reachFrames()) +
                "-frame reach";
        m_LostPending = false; // the keyframe that follows heals it all
        return false;
    }
    m_LostFrom = from;
    m_LostPending = true;
    m_Invalidations++;
    return true;
}

bool AmfEncoder::encode(ID3D11Texture2D* surface, bool forceKeyframe, uint32_t frameNumber,
                        EncoderOutput& out, std::string& error)
{
    if (!m_Encoder || !m_Context) {
        error = "the encoder is not initialized";
        return false;
    }
    if (m_Output) {
        error = "the previous frame was not released";
        return false;
    }
    // A repair the driver refused last time: the picture that went out was
    // predicting from a frame the receiver never had, and a keyframe is the
    // only thing that ends that.
    if (m_ForceKeyframeNext) {
        m_ForceKeyframeNext = false;
        forceKeyframe = true;
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

    // ── The repair the receiver asked for, if any ───────────────────────────
    // Force this picture onto the newest slot that predates the loss; the
    // slots not named are dropped by the driver (RESET_UNUSED) and forgotten
    // here. A keyframe makes the question moot — it empties the DPB.
    amf_int64 forcedBits = 0;
    if (m_LostPending) {
        m_LostPending = false;
        if (!forceKeyframe) {
            const int slot = m_Ltr.cleanSlotBefore(m_LostFrom);
            if (slot >= 0) {
                forcedBits = static_cast<amf_int64>(ReferenceSlots::bitFor(slot));
                input->SetProperty(props.forceLtrBitfield, forcedBits);
                m_Ltr.dropFrom(m_LostFrom);
            } else {
                // Cannot happen — invalidateReference() checked the same table
                // — but a keyframe is the safe answer if it ever does.
                forceKeyframe = true;
            }
        }
    }
    if (forceKeyframe) {
        input->SetProperty(props.forcePictureType, props.forcePictureTypeIdr);
        // H.264 only: its parameter sets are asked for on the picture, there
        // being no IDR-aligned insertion mode to set once (see
        // CodecProperties). Without this the browser's decoder can never
        // configure itself from anything but the very first keyframe.
        if (props.insertSps) {
            input->SetProperty(props.insertSps, true);
            input->SetProperty(props.insertPps, true);
        }
    }

    // Mark this picture into its slot when it is its turn — always for a
    // keyframe, which has just emptied every slot.
    const int markSlot = m_Ltr.slotFor(frameNumber, forceKeyframe);
    if (markSlot >= 0) input->SetProperty(props.markLtrIndex, amf_int64(markSlot));

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

    amf_int64 avgQp = -1;
    if (data->GetProperty(props.statisticAvgQp, &avgQp) == AMF_OK && avgQp >= 0)
        out.avgQp = static_cast<int>(avgQp);

    // ── What the driver did with the slots ──────────────────────────────────
    // The table records the driver's answer, not the request: a keyframe
    // empties it, a confirmed mark fills one slot, and a forced reference the
    // driver ignored is said once — the picture went out predicting from a
    // lost frame, which is the corruption NVENC's path also tolerates until
    // the intra-refresh wave passes.
    if (m_Ltr.enabled()) {
        // Judged before this picture is recorded, so the table still holds the
        // references the driver had to choose from.
        if (forcedBits != 0) {
            amf_int64 used = 0;
            const bool known =
                data->GetProperty(props.outputReferencedLtrBitfield, &used) == AMF_OK;
            const auto usedBits = static_cast<uint64_t>(used);
            // A driver that picks a slot of its own is not necessarily wrong:
            // what makes a reference clean is the FRAME it holds, not its
            // index. Asked for the newest picture before the loss, given an
            // older one, the delta is merely larger. Given a picture from the
            // loss onwards — or no long-term reference at all — it predicts
            // from something the receiver never had, and only a keyframe ends
            // that.
            const bool obeyed = !known || (usedBits & static_cast<uint64_t>(forcedBits)) != 0;
            const bool clean = obeyed || m_Ltr.allBefore(usedBits, m_LostFrom);
            if (clean) {
                if (m_HealsLogged < 5 || m_HealsLogged % 50 == 0)
                    log::info("[native] AMF healed frame " + std::to_string(frameNumber) +
                              " with a delta from long-term " +
                              m_Ltr.describe(static_cast<uint64_t>(forcedBits)) +
                              (known ? " (driver referenced " + m_Ltr.describe(usedBits) + ")"
                                     : " (driver reports nothing)") +
                              ", " + std::to_string(m_Invalidations) + " invalidation(s) so far");
                m_HealsLogged++;
            } else {
                // The picture already went out predicting from a lost frame;
                // nothing takes it back. The next one is a keyframe, which is
                // what this whole path exists to avoid — so it is worth saying
                // every time, not once: it is a cost, not a quirk.
                m_ForceKeyframeNext = true;
                log::warning("[native] AMF ignored the forced long-term reference (asked " +
                             m_Ltr.describe(static_cast<uint64_t>(forcedBits)) + ", referenced " +
                             m_Ltr.describe(usedBits) + ") for a loss at frame " +
                             std::to_string(m_LostFrom) +
                             " — that delta predicts from a frame the receiver lost, so the next "
                             "picture is a keyframe");
            }
        }
        if (out.keyframe) m_Ltr.clear();
        if (markSlot >= 0) {
            amf_int64 markedIdx = -1;
            const bool answered =
                data->GetProperty(props.outputMarkedLtrIndex, &markedIdx) == AMF_OK;
            // "default = -1" is documented, but the driver stores it in 32 bits
            // and it arrives as 4294967295 — a plain `>= 0` reads "not marked"
            // as slot four billion, calls marked() with a cast that lands back
            // on -1, and the table quietly keeps a hole where a reference
            // should be. That hole is invisible until a loss, when the repair
            // names a slot the driver never filled. Anything outside the slots
            // granted is a refusal, whatever its bit pattern.
            const int idx = (answered && markedIdx >= 0 && markedIdx < m_Ltr.count())
                                ? static_cast<int>(markedIdx)
                                : -1;
            if (idx >= 0) {
                m_Ltr.marked(idx, frameNumber);
            } else if (!m_MarkRefusedLogged) {
                m_MarkRefusedLogged = true;
                log::warning("[native] AMF did not mark frame " + std::to_string(frameNumber) +
                             " as a long-term reference (asked slot " + std::to_string(markSlot) +
                             ", answered " +
                             (answered ? std::to_string(markedIdx) : std::string("nothing")) +
                             " of " + std::to_string(m_Ltr.count()) +
                             " slots) — a lost frame costs a keyframe until it does");
            }
        }
    }
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
    //
    // The VBV goes through the same floor as init(): the still-screen burst
    // calls this twice per still/motion transition, and a plain division by
    // the frame rate would leave the encoder with a budget 2.4× tighter at
    // 144 Hz than the one it started with — the soft first frame RateControl.h
    // exists to prevent, reintroduced on AMD by the very path meant to sharpen it.
    m_Encoder->SetProperty(props.targetBitrate, bitsPerSecond);
    m_Encoder->SetProperty(props.peakBitrate, bitsPerSecond);
    m_Encoder->SetProperty(
        props.vbvBufferSize,
        static_cast<amf_int64>(vbvBits(static_cast<uint32_t>(bitsPerSecond), m_Fps, m_VbvFrames)));
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
