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

} // namespace

NvencEncoder::~NvencEncoder()
{
    stop();
}

bool NvencEncoder::init(ID3D11Device* device, Codec codec, int width, int height, int fps,
                        int bitrateKbps, bool yuv444, bool intraRefresh, std::string& error)
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
    // P4 is the middle of the speed/quality range. The brief asks for 70 %
    // latency, 30 % quality — P1 would be faster but visibly softer, and the
    // difference between P4 and P1 on a modern encoder is under a millisecond.
    NV_ENC_PRESET_CONFIG preset = {};
    preset.version = NV_ENC_PRESET_CONFIG_VER;
    preset.presetCfg.version = NV_ENC_CONFIG_VER;
    status =
        m_Api->fn().nvEncGetEncodePresetConfigEx(m_Encoder, codecGuid(codec), NV_ENC_PRESET_P4_GUID,
                                                 NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY, &preset);
    if (status != NV_ENC_SUCCESS) {
        error =
            std::string("could not read the encoder preset: ") + NvencApi::statusToString(status);
        stop();
        return false;
    }

    m_Config = preset.presetCfg;
    m_Config.version = NV_ENC_CONFIG_VER;

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
    m_Config.rcParams.vbvBufferSize = vbvBitsPerFrame(m_Config.rcParams.averageBitRate, fps);
    m_Config.rcParams.vbvInitialDelay = m_Config.rcParams.vbvBufferSize;

    // ── Codec-specific: parameter sets and intra-refresh ────────────────────
    // repeatSPSPPS matters for the browser: its decoder configures itself from
    // the parameter sets, so a client that joins late or loses the first
    // keyframe must be able to start from the next one.
    // The profile has to agree with the input format. Without this NVENC
    // accepts 4:4:4 input and encodes 4:2:0 from it — the extra chroma is
    // silently discarded, which looks exactly like the feature not working.
    if (yuv444) {
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
    m_InitParams.presetGUID = NV_ENC_PRESET_P4_GUID;
    m_InitParams.tuningInfo = NV_ENC_TUNING_INFO_ULTRA_LOW_LATENCY;
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

    log::info("[native] NVENC ready: " + std::to_string(width) + "x" + std::to_string(height) +
              "@" + std::to_string(fps) + " " + toString(codec) + " CBR " +
              std::to_string(bitrateKbps) + " kbps" +
              (m_IntraRefresh ? ", intra-refresh over " + std::to_string(refreshPeriod) + " frames"
                              : ", keyframes"));
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
        m_Config.rcParams.vbvBufferSize = vbvBitsPerFrame(
            m_Config.rcParams.averageBitRate, static_cast<int>(m_InitParams.frameRateNum));
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
