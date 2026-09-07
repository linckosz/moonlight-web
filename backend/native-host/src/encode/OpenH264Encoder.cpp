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

#include "OpenH264Encoder.h"

#include "../core/Log.h"
#include "RateControl.h"

#include <codec_api.h>
#include <codec_app_def.h>
#include <codec_ver.h>

#include <algorithm>
#include <cstring>
#include <thread>

namespace mw::native::encode {
namespace {

/// Past this many slices the extra slice headers and the broken prediction
/// across slice edges cost more bits than the cores save time — measured by
/// everyone who has tuned OpenH264 for real time, and the N95 this is for has
/// four cores anyway.
constexpr int kMaxThreads = 4;

int pickThreads(int asked)
{
    if (asked > 0) return std::min(asked, kMaxThreads);
    const unsigned hw = std::thread::hardware_concurrency();
    if (hw == 0) return 1;
    return static_cast<int>(std::min<unsigned>(hw, static_cast<unsigned>(kMaxThreads)));
}

/// The bitrate ceiling for a target: as close to CBR as OpenH264 allows. It
/// refuses a ceiling EQUAL to the target ("MaxSpatialBitrate should be larger
/// than SpatialBitrate", measured 07/09/2026) — so one percent above, never
/// less than a kilobit, is the tightest ceiling that is not an error.
int ceilingFor(int targetBps)
{
    const int margin = std::max(1000, targetBps / 100);
    return targetBps + margin;
}

/// OpenH264's own diagnostics, routed into the engine's log rather than to
/// stderr. Only warnings and errors are asked for (see init()).
void traceCallback(void*, int level, const char* message)
{
    if (!message) return;
    if (level <= WELS_LOG_ERROR)
        log::warning(std::string("[native] OpenH264: ") + message);
    else
        log::info(std::string("[native] OpenH264: ") + message);
}

} // namespace

OpenH264Encoder::~OpenH264Encoder()
{
    stop();
}

const char* OpenH264Encoder::version()
{
    static const std::string s = "OpenH264 " + std::to_string(OPENH264_MAJOR) + "." +
                                 std::to_string(OPENH264_MINOR) + "." +
                                 std::to_string(OPENH264_REVISION);
    return s.c_str();
}

bool OpenH264Encoder::init(int width, int height, int fps, int bitrateKbps, int threads,
                           const EncoderTuning& tuning, std::string& error)
{
    stop();
    if (width <= 0 || height <= 0 || (width % 2) || (height % 2)) {
        error = "OpenH264 needs an even picture size";
        return false;
    }
    if (WelsCreateSVCEncoder(&m_Encoder) != 0 || !m_Encoder) {
        error = "WelsCreateSVCEncoder failed";
        m_Encoder = nullptr;
        return false;
    }

    m_Width = width;
    m_Height = height;
    m_Fps = fps > 0 ? fps : 60;
    m_Threads = pickThreads(threads);

    int traceLevel = WELS_LOG_WARNING;
    m_Encoder->SetOption(ENCODER_OPTION_TRACE_LEVEL, &traceLevel);
    WelsTraceCallback cb = traceCallback;
    m_Encoder->SetOption(ENCODER_OPTION_TRACE_CALLBACK, &cb);

    SEncParamExt p;
    std::memset(&p, 0, sizeof(p));
    m_Encoder->GetDefaultParams(&p);

    // Real-time camera usage rather than screen-content: the screen-content
    // mode adds scroll and background analysis — CPU spent guessing what the
    // desktop is doing, on a machine that has none to spare. If it ever earns
    // its keep it will be because the bench said so.
    p.iUsageType = CAMERA_VIDEO_REAL_TIME;
    p.iPicWidth = width;
    p.iPicHeight = height;
    p.fMaxFrameRate = static_cast<float>(m_Fps);
    p.iTargetBitrate = bitrateKbps * 1000;
    p.iMaxBitrate = ceilingFor(p.iTargetBitrate);
    m_TargetBps = p.iTargetBitrate;
    p.iRCMode = RC_BITRATE_MODE;
    p.iTemporalLayerNum = 1;
    p.iSpatialLayerNum = 1;
    p.iComplexityMode = LOW_COMPLEXITY;
    // 0 = intra only on the first picture, then on request. A keyframe is a
    // bitrate spike, and on a congested link the spike causes the loss that
    // causes the next request.
    p.uiIntraPeriod = 0;
    p.iNumRefFrame = 1;
    p.eSpsPpsIdStrategy = CONSTANT_ID;
    p.bPrefixNalAddingCtrl = false;
    p.bEnableSSEI = false;
    p.iPaddingFlag = 0;
    // CAVLC: CABAC buys bits for CPU, and CPU is the scarce resource here.
    p.iEntropyCodingModeFlag = 0;
    // Never skip: cadence is the engine's business, and a picture that
    // silently vanishes reads as a stall at the client.
    p.bEnableFrameSkip = false;
    p.uiMaxNalSize = 0;
    p.bEnableLongTermReference = false;
    p.iLTRRefNum = 0;
    p.iLoopFilterDisableIdc = 0; // deblocking on: cheap, and it is most of the picture quality
    p.bEnableDenoise = false;
    p.bEnableBackgroundDetection = false;
    p.bEnableAdaptiveQuant = false;
    p.bEnableSceneChangeDetect = false;
    p.bEnableFrameCroppingFlag = true;
    p.bFixRCOverShoot = true;
    p.iIdrBitrateRatio = 100; // an IDR gets one frame's budget, like everywhere else here
    // Slice-level threading: one picture across the cores, no pipeline.
    p.iMultipleThreadIdc = static_cast<unsigned short>(m_Threads);
    p.bUseLoadBalancing = true;

    SSpatialLayerConfig& layer = p.sSpatialLayers[0];
    layer.iVideoWidth = width;
    layer.iVideoHeight = height;
    layer.fFrameRate = static_cast<float>(m_Fps);
    layer.iSpatialBitrate = p.iTargetBitrate;
    layer.iMaxSpatialBitrate = p.iMaxBitrate;
    // Decreasing the target below the ceiling is always accepted; the ceiling is
    // moved first when the target rises (setBitrate). Frame skipping stays off
    // even though OpenH264 warns the bitrate "can't be controlled" without it:
    // what it means is that a picture the QP cannot fit will overshoot rather
    // than vanish, and an overshoot is what the link governor is for — a
    // vanished picture reads as a stall at the client.
    layer.uiProfileIdc = PRO_BASELINE; // CAVLC is baseline anyway; say it in the SPS
    layer.uiLevelIdc = LEVEL_UNKNOWN;  // let it pick from the picture and the rate
    layer.sSliceArgument.uiSliceMode = m_Threads > 1 ? SM_FIXEDSLCNUM_SLICE : SM_SINGLE_SLICE;
    layer.sSliceArgument.uiSliceNum = static_cast<unsigned>(m_Threads);
    // BT.709 limited range, said in the VUI so the browser does not guess.
    layer.bVideoSignalTypePresent = true;
    layer.uiVideoFormat = VF_UNDEF;
    layer.bFullRange = false;
    layer.bColorDescriptionPresent = true;
    layer.uiColorPrimaries = CP_BT709;
    layer.uiTransferCharacteristics = TRC_BT709;
    layer.uiColorMatrix = CM_BT709;

    const int rv = m_Encoder->InitializeExt(&p);
    if (rv != cmResultSuccess) {
        error = "OpenH264 refused the parameters (" + std::to_string(rv) + ")";
        stop();
        return false;
    }

    const std::string overrides = tuning.describe();
    log::info(
        std::string("[native] ") + version() + " ready: " + std::to_string(width) + "x" +
        std::to_string(height) + "@" + std::to_string(m_Fps) + " H.264 baseline CBR " +
        std::to_string(bitrateKbps) + " kbps, VBV " +
        std::to_string(vbvBits(static_cast<uint32_t>(bitrateKbps) * 1000, m_Fps, tuning.vbvFrames) /
                       8 / 1024) +
        " KB, keyframes on request, " + std::to_string(m_Threads) +
        " slice thread(s), CAVLC, no frame skip (on the CPU)" +
        (overrides.empty() ? "" : " [bench: " + overrides + "]"));
    return true;
}

bool OpenH264Encoder::encode(const I420Picture& picture, bool forceKeyframe, uint32_t frameNumber,
                             EncoderOutput& out, std::string& error)
{
    out = EncoderOutput{};
    if (!m_Encoder) {
        error = "encoder not initialised";
        return false;
    }
    if (picture.width != m_Width || picture.height != m_Height) {
        error = "picture size does not match the encoder";
        return false;
    }

    if (forceKeyframe) m_Encoder->ForceIntraFrame(true);

    SSourcePicture src;
    std::memset(&src, 0, sizeof(src));
    src.iColorFormat = videoFormatI420;
    src.iPicWidth = m_Width;
    src.iPicHeight = m_Height;
    src.iStride[0] = picture.strideY;
    src.iStride[1] = picture.strideU;
    src.iStride[2] = picture.strideV;
    src.pData[0] = const_cast<unsigned char*>(picture.y);
    src.pData[1] = const_cast<unsigned char*>(picture.u);
    src.pData[2] = const_cast<unsigned char*>(picture.v);
    // Milliseconds; our own monotonic count so the rate control's clock never
    // runs backwards whatever the caller's numbering does.
    (void)frameNumber;
    src.uiTimeStamp = static_cast<long long>(m_FramesIn) * 1000 / m_Fps;

    SFrameBSInfo info;
    std::memset(&info, 0, sizeof(info));
    const int rv = m_Encoder->EncodeFrame(&src, &info);
    if (rv != cmResultSuccess) {
        error = "EncodeFrame failed (" + std::to_string(rv) + ")";
        return false;
    }
    ++m_FramesIn;

    if (info.eFrameType == videoFrameTypeSkip || info.iLayerNum <= 0) return true;

    // One layer is the normal case and is handed out in place — the buffer
    // belongs to the encoder until the next EncodeFrame, which is exactly the
    // EncoderOutput contract. More than one is gathered.
    size_t total = 0;
    for (int l = 0; l < info.iLayerNum; ++l) {
        const SLayerBSInfo& layer = info.sLayerInfo[l];
        for (int n = 0; n < layer.iNalCount; ++n)
            total += static_cast<size_t>(layer.pNalLengthInByte[n]);
    }
    if (info.iLayerNum == 1) {
        out.data = info.sLayerInfo[0].pBsBuf;
    } else {
        m_Gathered.resize(total);
        size_t at = 0;
        for (int l = 0; l < info.iLayerNum; ++l) {
            const SLayerBSInfo& layer = info.sLayerInfo[l];
            size_t bytes = 0;
            for (int n = 0; n < layer.iNalCount; ++n)
                bytes += static_cast<size_t>(layer.pNalLengthInByte[n]);
            std::memcpy(m_Gathered.data() + at, layer.pBsBuf, bytes);
            at += bytes;
        }
        out.data = m_Gathered.data();
    }
    out.size = total;
    out.keyframe = info.eFrameType == videoFrameTypeIDR || info.eFrameType == videoFrameTypeI;
    out.avgQp = -1; // OpenH264 does not say
    return true;
}

void OpenH264Encoder::releaseOutput()
{
    // Nothing to unlock: the bitstream lives in the encoder until the next
    // EncodeFrame, and the caller has promised to be done by then.
}

bool OpenH264Encoder::setBitrate(int bitrateKbps, std::string& error)
{
    if (!m_Encoder) {
        error = "encoder not initialised";
        return false;
    }
    // Four calls for one number, and every one is needed (welsEncoderExt.cpp):
    // SPATIAL_LAYER_ALL writes the encoder-wide figure and SPATIAL_LAYER_0 the
    // layer's own, and the check that refuses a target above its ceiling — or
    // a ceiling at or below its target — reads the layer's. So the ORDER
    // depends on the direction: rising, the ceiling moves first; falling, the
    // target does. Either other order trips the same check.
    const int targetBps = bitrateKbps * 1000;
    const int ceilingBps = ceilingFor(targetBps);
    const bool rising = targetBps > m_TargetBps;

    auto apply = [this, &error](ENCODER_OPTION option, int bps, const char* what) {
        for (const int layer :
             {static_cast<int>(SPATIAL_LAYER_ALL), static_cast<int>(SPATIAL_LAYER_0)}) {
            SBitrateInfo info;
            info.iLayer = static_cast<LAYER_NUM>(layer);
            info.iBitrate = bps;
            if (m_Encoder->SetOption(option, &info) != cmResultSuccess) {
                error = std::string("OpenH264 refused the ") + what;
                return false;
            }
        }
        return true;
    };

    if (rising) {
        if (!apply(ENCODER_OPTION_MAX_BITRATE, ceilingBps, "bitrate ceiling")) return false;
        if (!apply(ENCODER_OPTION_BITRATE, targetBps, "bitrate change")) return false;
    } else {
        if (!apply(ENCODER_OPTION_BITRATE, targetBps, "bitrate change")) return false;
        if (!apply(ENCODER_OPTION_MAX_BITRATE, ceilingBps, "bitrate ceiling")) return false;
    }
    m_TargetBps = targetBps;
    return true;
}

void OpenH264Encoder::stop()
{
    if (m_Encoder) {
        if (m_FramesIn > 0)
            log::info(std::string("[native] ") + version() + ": " + std::to_string(m_FramesIn) +
                      " pictures encoded on " + std::to_string(m_Threads) + " thread(s)");
        m_Encoder->Uninitialize();
        WelsDestroySVCEncoder(m_Encoder);
        m_Encoder = nullptr;
    }
    m_Gathered.clear();
    m_FramesIn = 0;
}

} // namespace mw::native::encode
