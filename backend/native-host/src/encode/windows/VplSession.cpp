/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "VplSession.h"

#include "../../core/Log.h"

#include <cstring>

namespace mw::native::encode {
namespace {

/// Ask the dispatcher for a HARDWARE implementation only.
///
/// Without this filter oneVPL will happily hand back its software fallback,
/// which encodes on the CPU. That is not what "Intel encoder" means here, and
/// silently getting it would show up as a mysterious CPU spike rather than as
/// the honest "no hardware encoder on this machine".
bool requireHardware(const VplApi& api, mfxLoader loader)
{
    mfxConfig config = api.CreateConfig(loader);
    if (!config) return false;

    mfxVariant value = {};
    value.Type = MFX_VARIANT_TYPE_U32;
    value.Data.U32 = MFX_IMPL_TYPE_HARDWARE;
    return api.SetConfigFilterProperty(config,
                                       reinterpret_cast<const mfxU8*>("mfxImplDescription.Impl"),
                                       value) == MFX_ERR_NONE;
}

} // namespace

VplSession::~VplSession()
{
    close();
}

bool VplSession::open(ID3D11Device* device, std::string& error)
{
    close();

    if (!device) {
        error = "no D3D11 device";
        return false;
    }

    m_Api = VplApi::instance();
    if (!m_Api->available()) {
        error = m_Api->unavailableReason();
        return false;
    }

    m_Loader = m_Api->Load();
    if (!m_Loader) {
        error = "could not start the oneVPL dispatcher";
        return false;
    }

    if (!requireHardware(*m_Api, m_Loader)) {
        error = "could not ask oneVPL for a hardware implementation";
        close();
        return false;
    }

    // Index 0 among the implementations that passed the filter: with the
    // hardware filter applied, anything it returns is a real encoder, and the
    // D3D11 handle below is what pins it to the adapter we want.
    const mfxStatus created = m_Api->CreateSession(m_Loader, 0, &m_Session);
    if (created != MFX_ERR_NONE || !m_Session) {
        error =
            std::string("no Intel hardware encoder available: ") + VplApi::statusToString(created);
        m_Session = nullptr;
        close();
        return false;
    }

    // The step that decides WHICH GPU: the session now works on the adapter
    // this device was opened on, and the encoder can take its textures without
    // a copy.
    const mfxStatus bound =
        m_Api->SetHandle(m_Session, MFX_HANDLE_D3D11_DEVICE, static_cast<mfxHDL>(device));
    if (bound != MFX_ERR_NONE) {
        error = std::string("oneVPL could not use this GPU: ") + VplApi::statusToString(bound);
        close();
        return false;
    }

    return true;
}

void VplSession::close()
{
    if (m_Session && m_Api) {
        m_Api->Close(m_Session);
        m_Session = nullptr;
    }
    if (m_Loader && m_Api) {
        m_Api->Unload(m_Loader);
        m_Loader = nullptr;
    }
}

bool fillEncodeParams(mfxVideoParam& params, Codec codec, int width, int height, int fps,
                      int bitrateKbps)
{
    std::memset(&params, 0, sizeof(params));

    switch (codec) {
    case Codec::H264: params.mfx.CodecId = MFX_CODEC_AVC; break;
    case Codec::Hevc: params.mfx.CodecId = MFX_CODEC_HEVC; break;
    case Codec::Av1: params.mfx.CodecId = MFX_CODEC_AV1; break;
    default: return false;
    }

    if (fps <= 0) fps = 60;
    if (bitrateKbps <= 0) bitrateKbps = 20000;

    // The surfaces come from our own conversion pass, in video memory.
    params.IOPattern = MFX_IOPATTERN_IN_VIDEO_MEMORY;
    // No look-ahead in the pipeline: one frame in, one frame out.
    params.AsyncDepth = 1;

    // 7 = best speed. The brief weights latency at 70 % against 30 % quality,
    // and on Intel the target-usage scale costs far more time at the quality
    // end than it returns in picture.
    params.mfx.TargetUsage = MFX_TARGETUSAGE_7;
    params.mfx.RateControlMethod = MFX_RATECONTROL_CBR;

    // TargetKbps and friends are 16-bit, so anything at or above 65535 kbps
    // needs the multiplier — and MoonlightWeb genuinely offers up to 150 Mbps.
    // Without this a 100 Mbps request would silently wrap to a fraction of
    // itself, which looks like the encoder ignoring the bitrate setting.
    mfxU16 multiplier = 1;
    while (bitrateKbps / multiplier > 65000)
        ++multiplier;
    params.mfx.BRCParamMultiplier = multiplier;
    params.mfx.TargetKbps = static_cast<mfxU16>(bitrateKbps / multiplier);
    params.mfx.MaxKbps = params.mfx.TargetKbps;

    // One frame's worth of buffer, in KB, which is what actually enforces the
    // latency: no single frame may be so large that it takes several frame
    // times to transmit.
    const int frameKb = (bitrateKbps / fps) / 8;
    const int bufferKb = frameKb > 0 ? frameKb : 1;
    params.mfx.BufferSizeInKB =
        static_cast<mfxU16>((bufferKb / multiplier) > 0 ? (bufferKb / multiplier) : 1);
    params.mfx.InitialDelayInKB = params.mfx.BufferSizeInKB;

    // No B-frames, and no keyframe the client did not ask for.
    params.mfx.GopRefDist = 1;
    params.mfx.GopPicSize = 0xFFFF; // effectively infinite
    params.mfx.IdrInterval = 0xFFFF;
    params.mfx.NumRefFrame = 1;

    params.mfx.FrameInfo.FourCC = MFX_FOURCC_NV12;
    params.mfx.FrameInfo.ChromaFormat = MFX_CHROMAFORMAT_YUV420;
    params.mfx.FrameInfo.PicStruct = MFX_PICSTRUCT_PROGRESSIVE;
    params.mfx.FrameInfo.FrameRateExtN = static_cast<mfxU32>(fps);
    params.mfx.FrameInfo.FrameRateExtD = 1;
    params.mfx.FrameInfo.CropX = 0;
    params.mfx.FrameInfo.CropY = 0;
    params.mfx.FrameInfo.CropW = static_cast<mfxU16>(width);
    params.mfx.FrameInfo.CropH = static_cast<mfxU16>(height);
    // Intel hardware wants 16-aligned width and height for the surface itself;
    // the crop above is what the picture really is.
    params.mfx.FrameInfo.Width = static_cast<mfxU16>((width + 15) & ~15);
    params.mfx.FrameInfo.Height = static_cast<mfxU16>((height + 15) & ~15);

    return true;
}

void attachIntraRefresh(mfxVideoParam& params, mfxExtCodingOption2& option,
                        std::vector<mfxExtBuffer*>& buffers)
{
    std::memset(&option, 0, sizeof(option));
    option.Header.BufferId = MFX_EXTBUFF_CODING_OPTION2;
    option.Header.BufferSz = sizeof(option);

    // Vertical: the wave sweeps by columns of macroblocks. Either axis works;
    // vertical is the conventional choice and matches what the other two
    // vendors do by default.
    option.IntRefType = MFX_REFRESH_VERTICAL;
    option.IntRefCycleSize = static_cast<mfxU16>(kIntraRefreshPeriodFrames);
    // Leave the refreshed blocks at the frame's own quality: a positive delta
    // would make the healing band visibly coarser than what surrounds it, which
    // is precisely the artefact this is meant to avoid.
    option.IntRefQPDelta = 0;

    buffers.clear();
    buffers.push_back(reinterpret_cast<mfxExtBuffer*>(&option));
    params.ExtParam = buffers.data();
    params.NumExtParam = static_cast<mfxU16>(buffers.size());
}

} // namespace mw::native::encode
