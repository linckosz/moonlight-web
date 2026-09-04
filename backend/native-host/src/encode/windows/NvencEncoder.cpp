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

#include "NvencEncoder.h"

#include "../../core/Log.h"
#include "../RateControl.h"

#include <cstring>

namespace mw::native::encode {
namespace {

const GUID& codecGuid(Codec codec)
{
    switch (codec) {
    case Codec::Av1: return NV_ENC_CODEC_AV1_GUID;
    case Codec::Hevc: return NV_ENC_CODEC_HEVC_GUID;
    case Codec::H264: break;
    }
    return NV_ENC_CODEC_H264_GUID;
}

/// The engine's own preset: P1, the fastest, since the bench of 04/09/2026
/// (`docs/bench-native-host.md`).
///
/// It started as P4, on the belief that P1 would be visibly softer for under a
/// millisecond of gain. Measured on an RTX 5060 Ti at 1440p, 40 Mbit/s: P1
/// encodes a first-person shooter in 3.4 ms against 7.7 for P4 (p99 5 against
/// 11), at the SAME average QP (25) and with nothing to tell apart on the
/// decoded picture; a platform game costs +1 QP, and only scrolling text — sharp
/// high-contrast edges P4 predicts better — pays +5 QP while it moves. Latency
/// first (rule 3 of the module): the shooter is the target, the text is the
/// exception. P2 was slower AND softer, P3/P5/P6 equal P4, P7 slower still.
///
/// What stays: the ULL tuning's quarter-resolution multipass. Turning it off
/// buys 0.6 ms more but breaks the still-screen refinement burst (§9.1) — the
/// single-pass rate control of a picture that just stopped moving never spends
/// its budget, and the QP stalls at 29 instead of reaching 8.
constexpr int kDefaultPreset = 1;

const GUID& presetGuid(int preset)
{
    switch (preset) {
    case 1: return NV_ENC_PRESET_P1_GUID;
    case 2: return NV_ENC_PRESET_P2_GUID;
    case 3: return NV_ENC_PRESET_P3_GUID;
    case 5: return NV_ENC_PRESET_P5_GUID;
    case 6: return NV_ENC_PRESET_P6_GUID;
    case 7: return NV_ENC_PRESET_P7_GUID;
    default: return NV_ENC_PRESET_P4_GUID;
    }
}

const char* multiPassName(NV_ENC_MULTI_PASS mode)
{
    switch (mode) {
    case NV_ENC_TWO_PASS_QUARTER_RESOLUTION: return "quarter";
    case NV_ENC_TWO_PASS_FULL_RESOLUTION: return "full";
    default: return "off";
    }
}

} // namespace

NvencEncoder::~NvencEncoder()
{
    stop();
}

bool NvencEncoder::init(ID3D11Device* device, Codec codec, int width, int height, int fps,
                        int bitrateKbps, bool yuv444, bool intraRefresh,
                        const EncoderTuning& tuning, std::string& error)
{
    stop();

    m_Api = NvencApi::instance();
    if (!m_Api->available()) {
        error = m_Api->unavailableReason();
        return false;
    }
    if (!device || width <= 0 || height <= 0) {
        error = "invalid encoder parameters";
        return false;
    }

    m_Codec = codec;
    // AYUV is what the 4:4:4 conversion pass produces; NV12 the 4:2:0 one.
    m_BufferFormat = yuv444 ? NV_ENC_BUFFER_FORMAT_AYUV : NV_ENC_BUFFER_FORMAT_NV12;
    m_Width = width;
    m_Height = height;
    if (fps <= 0) fps = 60;
    if (bitrateKbps <= 0) bitrateKbps = 20000;

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS open = {};
    open.version = NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS_VER;
    open.deviceType = NV_ENC_DEVICE_TYPE_DIRECTX;
    open.device = device;
    open.apiVersion = NVENCAPI_VERSION;

    NVENCSTATUS status = m_Api->fn().nvEncOpenEncodeSessionEx(&open, &m_Encoder);
    if (status != NV_ENC_SUCCESS || !m_Encoder) {
        error =
            std::string("could not open an encode session: ") + NvencApi::statusToString(status);
        m_Encoder = nullptr;
        return false;
    }

    // Start from the driver's own preset rather than a hand-built config: the
    // preset carries the tuning for this GPU generation, and only the settings
    // that are wrong for a live stream are then overridden.
    //
    // The preset and the latency tuning are the engine's (kDefaultPreset — P1,
    // see there — and ultra-low-latency) unless the bench moved them — see
    // EncoderTuning.
    const int presetNumber = tuning.nvencPreset > 0 ? tuning.nvencPreset : kDefaultPreset;
    const GUID& presetId = presetGuid(presetNumber);
    const NV_ENC_TUNING_INFO tuningInfo = tuning.nvencTuning == EncoderTuning::Latency::Low
                                              ? NV_ENC_TUNING_INFO_LOW_LATENCY
                                              : NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    status = m_Api->fn().nvEncGetEncodePresetConfigEx(m_Encoder, codecGuid(codec), presetId,
                                                      tuningInfo, &preset);
    if (status != NV_ENC_SUCCESS) {
        error =
            std::string("could not read the encoder preset: ") + NvencApi::statusToString(status);
        stop();
        return false;
    }

    m_Config = preset.presetCfg;
    m_Config.version = NV_ENC_CONFIG_VER;

    // What the preset itself switched on, before anything here touches it.
    // Logged because it is the part of the configuration nobody wrote: a
    // preset is a bundle of decisions the driver took, and the bench compares
    // against them without being able to read them any other way.
    log::info(
        std::string("[native] NVENC preset P") + std::to_string(presetNumber) +
        (tuningInfo == NV_ENC_TUNING_INFO_LOW_LATENCY ? "/LL" : "/ULL") +
        " as the driver ships it: multipass=" + multiPassName(m_Config.rcParams.multiPass) +
        " aq=" + std::to_string(m_Config.rcParams.enableAQ) +
        " taq=" + std::to_string(m_Config.rcParams.enableTemporalAQ) + " lookahead=" +
        std::to_string(m_Config.rcParams.enableLookahead ? m_Config.rcParams.lookaheadDepth : 0) +
        " rc=" + std::to_string(static_cast<int>(m_Config.rcParams.rateControlMode)));

    // ── The bench's overrides, where it gave any ────────────────────────────
    // Lookahead is deliberately NOT among them: it holds N frames back by
    // definition, so it is N frame intervals of latency before any measurement
    // — disqualified by construction, not by a number.
    switch (tuning.nvencMultiPass) {
    case EncoderTuning::MultiPass::Off:
        m_Config.rcParams.multiPass = NV_ENC_MULTI_PASS_DISABLED;
        break;
    case EncoderTuning::MultiPass::QuarterRes:
        m_Config.rcParams.multiPass = NV_ENC_TWO_PASS_QUARTER_RESOLUTION;
        break;
    case EncoderTuning::MultiPass::FullRes:
        m_Config.rcParams.multiPass = NV_ENC_TWO_PASS_FULL_RESOLUTION;
        break;
    case EncoderTuning::MultiPass::Default: break;
    }
    if (tuning.spatialAq != EncoderTuning::Choice::Default)
        m_Config.rcParams.enableAQ = tuning.spatialAq == EncoderTuning::Choice::On ? 1u : 0u;
    if (tuning.temporalAq != EncoderTuning::Choice::Default)
        m_Config.rcParams.enableTemporalAQ =
            tuning.temporalAq == EncoderTuning::Choice::On ? 1u : 0u;
    m_VbvFrames = tuning.vbvFrames;

    // ── Never send a periodic keyframe ──────────────────────────────────────
    // A keyframe is a bitrate spike; on a congested link the spike causes the
    // loss that causes the request for another keyframe. Infinite GOP plus
    // intra-refresh spreads the same recovery information across every frame.
    m_Config.gopLength = NVENC_INFINITE_GOPLENGTH;
    // No B-frames: one would make the encoder hold a frame back to reference a
    // picture that has not been sent, buying a whole frame of latency.
    m_Config.frameIntervalP = 1;

    // ── Constant bitrate ────────────────────────────────────────────────────
    // The link has a budget; spending it evenly beats saving up for a burst
    // that arrives as packet loss.
    m_Config.rcParams.rateControlMode = NV_ENC_PARAMS_RC_CBR;
    m_Config.rcParams.averageBitRate = static_cast<uint32_t>(bitrateKbps) * 1000u;
    m_Config.rcParams.maxBitRate = m_Config.rcParams.averageBitRate;
    // The VBV is what actually enforces low latency: it caps how far ahead the
    // encoder may spend, so no single frame can be so large that it takes
    // several frame times to transmit. See RateControl.h for why it has a
    // floor rather than being one frame at any refresh rate.
    m_Config.rcParams.vbvBufferSize = vbvBits(m_Config.rcParams.averageBitRate, fps, m_VbvFrames);
    m_Config.rcParams.vbvInitialDelay = m_Config.rcParams.vbvBufferSize;

    // ── Codec-specific: parameter sets and intra-refresh ────────────────────
    // repeatSPSPPS matters for the browser: its decoder configures itself from
    // the parameter sets, so a client that joins late or loses the first
    // keyframe must be able to start from the next one.
    // The profile has to agree with the input format. Without this NVENC
    // accepts 4:4:4 input and encodes 4:2:0 from it — the extra chroma is
    // silently discarded, which looks exactly like the feature not working.
    if (yuv444) {
        if (codec == Codec::Av1) {
            // No 4:4:4 profile exists for AV1 in this SDK, and the capability
            // query says so per codec — the Selector routes 4:4:4 to HEVC or
            // H.264 instead. Refusing here keeps a misrouted session from
            // encoding 4:2:0 under a 4:4:4 label.
            error = "NVENC has no 4:4:4 path for AV1";
            return false;
        }
        m_Config.profileGUID = codec == Codec::Hevc ? NV_ENC_HEVC_PROFILE_FREXT_GUID
                                                    : NV_ENC_H264_PROFILE_HIGH_444_GUID;
    }

    // Intra-refresh only when the caller asked for it. Enabling it unasked
    // costs slightly larger P-frames at a fixed CBR budget for a benefit only a
    // receiver that decodes through damage can collect — see SessionConfig.
    m_IntraRefresh = intraRefresh;
    const uint32_t refreshEnabled = intraRefresh ? 1u : 0u;
    // The wave replaces the periodic keyframe: every frame carries a band of
    // intra blocks, and after one period the whole picture has been refreshed.
    // Two seconds' worth at the rate frames are really encoded — see
    // RateControl.h for why it is a duration and not a frame count.
    const auto refreshPeriod = static_cast<uint32_t>(intraRefreshPeriodFrames(fps));
    const auto refreshCount = static_cast<uint32_t>(intraRefreshCountFrames(fps));

    switch (codec) {
    case Codec::H264: {
        NV_ENC_CONFIG_H264& h264 = m_Config.encodeCodecConfig.h264Config;
        h264.chromaFormatIDC = yuv444 ? 3 : 1;
        h264.repeatSPSPPS = 1;
        h264.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        // Tell the decoder there is nothing to reorder. Without the VUI's
        // bitstream_restriction the browser's D3D11 H.264 decoder assumes the
        // worst — a DPB's worth of reordering, which at 1440p60 is a dozen
        // frames it holds back before showing any. Measured on the bench:
        // 200 ms of decode latency on a stream with no B-frames at all, against
        // 1 ms for HEVC, whose parameter sets carry the figure by default.
        h264.h264VUIParameters.bitstreamRestrictionFlag = 1;
        h264.enableIntraRefresh = refreshEnabled;
        h264.intraRefreshPeriod = refreshPeriod;
        h264.intraRefreshCnt = refreshCount;
        break;
    }
    case Codec::Hevc: {
        NV_ENC_CONFIG_HEVC& hevc = m_Config.encodeCodecConfig.hevcConfig;
        hevc.chromaFormatIDC = yuv444 ? 3 : 1;
        hevc.repeatSPSPPS = 1;
        hevc.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        hevc.enableIntraRefresh = refreshEnabled;
        hevc.intraRefreshPeriod = refreshPeriod;
        hevc.intraRefreshCnt = refreshCount;
        break;
    }
    case Codec::Av1: {
        NV_ENC_CONFIG_AV1& av1 = m_Config.encodeCodecConfig.av1Config;
        av1.idrPeriod = NVENC_INFINITE_GOPLENGTH;
        av1.enableIntraRefresh = refreshEnabled;
        av1.intraRefreshPeriod = refreshPeriod;
        av1.intraRefreshCnt = refreshCount;
        break;
    }
    }

    m_InitParams = {};
    m_InitParams.version = NV_ENC_INITIALIZE_PARAMS_VER;
    m_InitParams.encodeGUID = codecGuid(codec);
    m_InitParams.presetGUID = presetId;
    m_InitParams.tuningInfo = tuningInfo;
    m_InitParams.encodeWidth = static_cast<uint32_t>(width);
    m_InitParams.encodeHeight = static_cast<uint32_t>(height);
    m_InitParams.darWidth = static_cast<uint32_t>(width);
    m_InitParams.darHeight = static_cast<uint32_t>(height);
    m_InitParams.frameRateNum = static_cast<uint32_t>(fps);
    m_InitParams.frameRateDen = 1;
    m_InitParams.maxEncodeWidth = static_cast<uint32_t>(width);
    m_InitParams.maxEncodeHeight = static_cast<uint32_t>(height);
    // Synchronous: nvEncLockBitstream blocks until the frame is done. An async
    // session would need an event and a second thread to gain nothing here —
    // there is exactly one frame in flight by design.
    m_InitParams.enableEncodeAsync = 0;
    // Let the encoder decide picture types. We only ever override it to force
    // an IDR, which NV_ENC_PIC_FLAG_FORCEIDR does per frame.
    m_InitParams.enablePTD = 1;
    m_InitParams.encodeConfig = &m_Config;

    status = m_Api->fn().nvEncInitializeEncoder(m_Encoder, &m_InitParams);
    if (status != NV_ENC_SUCCESS) {
        error =
            std::string("could not initialize the encoder: ") + NvencApi::statusToString(status);
        stop();
        return false;
    }

    NV_ENC_CREATE_BITSTREAM_BUFFER buffer = {};
    buffer.version = NV_ENC_CREATE_BITSTREAM_BUFFER_VER;
    status = m_Api->fn().nvEncCreateBitstreamBuffer(m_Encoder, &buffer);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("could not create the bitstream buffer: ") +
                NvencApi::statusToString(status);
        stop();
        return false;
    }
    m_Bitstream = buffer.bitstreamBuffer;

    const std::string overrides = tuning.describe();
    log::info("[native] NVENC ready: " + std::to_string(width) + "x" + std::to_string(height) +
              "@" + std::to_string(fps) + " " + toString(codec) + " CBR " +
              std::to_string(bitrateKbps) + " kbps, VBV " +
              std::to_string(m_Config.rcParams.vbvBufferSize / 8 / 1024) + " KB" +
              (m_IntraRefresh ? ", intra-refresh over " + std::to_string(refreshPeriod) + " frames"
                              : ", keyframes") +
              ", P" + std::to_string(presetNumber) +
              (tuningInfo == NV_ENC_TUNING_INFO_LOW_LATENCY ? "/LL" : "/ULL") +
              " multipass=" + multiPassName(m_Config.rcParams.multiPass) +
              " aq=" + std::to_string(m_Config.rcParams.enableAQ) +
              " taq=" + std::to_string(m_Config.rcParams.enableTemporalAQ) +
              (overrides.empty() ? "" : " [bench: " + overrides + "]"));
    return true;
}

bool NvencEncoder::registerInput(ID3D11Texture2D* texture, std::string& error)
{
    if (m_Registered && m_RegisteredFor == texture) return true;

    if (m_Registered) {
        m_Api->fn().nvEncUnregisterResource(m_Encoder, m_Registered);
        m_Registered = nullptr;
        m_RegisteredFor = nullptr;
    }

    NV_ENC_REGISTER_RESOURCE resource = {};
    resource.version = NV_ENC_REGISTER_RESOURCE_VER;
    resource.resourceType = NV_ENC_INPUT_RESOURCE_TYPE_DIRECTX;
    resource.width = static_cast<uint32_t>(m_Width);
    resource.height = static_cast<uint32_t>(m_Height);
    resource.resourceToRegister = texture;
    resource.bufferFormat = m_BufferFormat;
    resource.bufferUsage = NV_ENC_INPUT_IMAGE;

    // This is the zero-copy step: NVENC takes the D3D11 texture the conversion
    // pass wrote, on the same adapter, with no staging buffer and no trip
    // through system memory.
    const NVENCSTATUS status = m_Api->fn().nvEncRegisterResource(m_Encoder, &resource);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("could not register the input surface: ") +
                NvencApi::statusToString(status);
        return false;
    }

    m_Registered = resource.registeredResource;
    m_RegisteredFor = texture;
    return true;
}

bool NvencEncoder::encode(ID3D11Texture2D* nv12, bool forceKeyframe, EncoderOutput& out,
                          std::string& error)
{
    if (!m_Encoder || !m_Bitstream) {
        error = "the encoder is not initialized";
        return false;
    }
    if (m_OutputLocked) {
        // The previous frame's buffer is still handed out. Encoding over it
        // would corrupt what the caller is sending.
        error = "the previous frame was not released";
        return false;
    }
    if (!registerInput(nv12, error)) return false;

    NV_ENC_MAP_INPUT_RESOURCE mapped = {};
    mapped.version = NV_ENC_MAP_INPUT_RESOURCE_VER;
    mapped.registeredResource = m_Registered;
    NVENCSTATUS status = m_Api->fn().nvEncMapInputResource(m_Encoder, &mapped);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("could not map the input surface: ") + NvencApi::statusToString(status);
        return false;
    }

    NV_ENC_PIC_PARAMS pic = {};
    pic.version = NV_ENC_PIC_PARAMS_VER;
    pic.inputBuffer = mapped.mappedResource;
    pic.bufferFmt = mapped.mappedBufferFmt;
    pic.inputWidth = static_cast<uint32_t>(m_Width);
    pic.inputHeight = static_cast<uint32_t>(m_Height);
    pic.outputBitstream = m_Bitstream;
    pic.pictureStruct = NV_ENC_PIC_STRUCT_FRAME;
    pic.inputTimeStamp = m_FrameIndex++;
    if (forceKeyframe)
        pic.encodePicFlags = NV_ENC_PIC_FLAG_FORCEIDR | NV_ENC_PIC_FLAG_OUTPUT_SPSPPS;

    status = m_Api->fn().nvEncEncodePicture(m_Encoder, &pic);
    // Unmap as soon as the encoder has consumed the surface: holding the map
    // would block the conversion pass from writing the next frame into it.
    m_Api->fn().nvEncUnmapInputResource(m_Encoder, mapped.mappedResource);

    if (status != NV_ENC_SUCCESS) {
        error = std::string("encode failed: ") + NvencApi::statusToString(status);
        return false;
    }

    NV_ENC_LOCK_BITSTREAM lock = {};
    lock.version = NV_ENC_LOCK_BITSTREAM_VER;
    lock.outputBitstream = m_Bitstream;
    // Blocking: in a synchronous session this is where the wait for the encoder
    // happens, and waiting here is exactly right — there is nothing else this
    // thread could usefully do with a frame half-encoded.
    lock.doNotWait = 0;

    status = m_Api->fn().nvEncLockBitstream(m_Encoder, &lock);
    if (status != NV_ENC_SUCCESS) {
        error =
            std::string("could not read the encoded frame: ") + NvencApi::statusToString(status);
        return false;
    }

    m_OutputLocked = true;
    out.data = static_cast<const uint8_t*>(lock.bitstreamBufferPtr);
    out.size = lock.bitstreamSizeInBytes;
    out.keyframe = lock.pictureType == NV_ENC_PIC_TYPE_IDR || lock.pictureType == NV_ENC_PIC_TYPE_I;
    out.avgQp = static_cast<int>(lock.frameAvgQP);
    return true;
}

void NvencEncoder::releaseOutput()
{
    if (!m_OutputLocked) return;
    m_Api->fn().nvEncUnlockBitstream(m_Encoder, m_Bitstream);
    m_OutputLocked = false;
}

bool NvencEncoder::setBitrate(int bitrateKbps, std::string& error)
{
    if (!m_Encoder || bitrateKbps <= 0) {
        error = "the encoder is not initialized";
        return false;
    }

    NV_ENC_RECONFIGURE_PARAMS reconfigure = {};
    reconfigure.version = NV_ENC_RECONFIGURE_PARAMS_VER;

    m_Config.rcParams.averageBitRate = static_cast<uint32_t>(bitrateKbps) * 1000u;
    m_Config.rcParams.maxBitRate = m_Config.rcParams.averageBitRate;
    if (m_InitParams.frameRateNum > 0) {
        m_Config.rcParams.vbvBufferSize =
            vbvBits(m_Config.rcParams.averageBitRate, static_cast<int>(m_InitParams.frameRateNum),
                    m_VbvFrames);
        m_Config.rcParams.vbvInitialDelay = m_Config.rcParams.vbvBufferSize;
    }
    reconfigure.reInitEncodeParams = m_InitParams;
    reconfigure.reInitEncodeParams.encodeConfig = &m_Config;

    const NVENCSTATUS status = m_Api->fn().nvEncReconfigureEncoder(m_Encoder, &reconfigure);
    if (status != NV_ENC_SUCCESS) {
        error = std::string("could not change the bitrate: ") + NvencApi::statusToString(status);
        return false;
    }
    return true;
}

void NvencEncoder::stop()
{
    if (!m_Encoder || !m_Api) {
        m_Encoder = nullptr;
        return;
    }

    releaseOutput();

    if (m_Registered) {
        m_Api->fn().nvEncUnregisterResource(m_Encoder, m_Registered);
        m_Registered = nullptr;
        m_RegisteredFor = nullptr;
    }
    if (m_Bitstream) {
        m_Api->fn().nvEncDestroyBitstreamBuffer(m_Encoder, m_Bitstream);
        m_Bitstream = nullptr;
    }

    m_Api->fn().nvEncDestroyEncoder(m_Encoder);
    m_Encoder = nullptr;
    m_FrameIndex = 0;
}

} // namespace mw::native::encode
