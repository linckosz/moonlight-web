/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "VplEncoder.h"

#include "../../core/Log.h"
#include "../RateControl.h"

#include <chrono>
#include <cstring>
#include <thread>

namespace mw::native::encode {
namespace {

/// How long to wait for one frame, in milliseconds. A deadline, not a schedule:
/// encoding takes a few milliseconds, and reaching this means the encoder has
/// stopped answering.
constexpr mfxU32 kSyncTimeoutMs = 100;

/// How many times to retry while the device reports itself busy.
///
/// MFX_WRN_DEVICE_BUSY is oneVPL's documented "ask again shortly", and it is
/// expected under load rather than exceptional. The wait between attempts is a
/// yield, not a sleep: on Windows the default timer granularity is 15.6 ms, so
/// a "short" sleep here would cost more than the encode — the mistake that made
/// the AMD path read 15.66 ms a frame until it was found.
constexpr int kBusyRetries = 200;

} // namespace

VplEncoder::~VplEncoder()
{
    stop();
}

bool VplEncoder::init(ID3D11Device* device, Codec codec, int width, int height, int fps,
                      int bitrateKbps, bool yuv444, bool intraRefresh, const EncoderTuning& tuning,
                      std::string& error)
{
    stop();

    if (!device || width <= 0 || height <= 0) {
        error = "invalid encoder parameters";
        return false;
    }
    if (yuv444) {
        // The capability query does not claim 4:4:4 either, so the Selector
        // should never route such a session here. Refusing loudly beats
        // encoding 4:2:0 while the overlay says 4:4:4.
        error = "4:4:4 is not implemented on the Intel encoder path";
        return false;
    }

    m_Codec = codec;
    m_Width = width;
    m_Height = height;
    m_Fps = fps > 0 ? fps : 60;
    m_Tuning = tuning;

    if (!m_Session.open(device, error)) return false;

    if (!fillEncodeParams(m_Params, codec, width, height, m_Fps, bitrateKbps, m_Tuning)) {
        error = "no oneVPL codec for this format";
        stop();
        return false;
    }

    // Query the BASE parameters, before any extension buffer is attached.
    //
    // Query's output block would otherwise need an extension chain of its own:
    // copying m_Params wholesale hands it our buffer as both input and output,
    // which some runtimes accept and others do not. Validating the plain
    // parameters and letting Init judge the extension is unambiguous.
    mfxVideoParam corrected = m_Params;
    const mfxStatus queried =
        m_Session.api()->EncodeQuery(m_Session.handle(), &m_Params, &corrected);
    if (queried == MFX_ERR_NONE || queried == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM ||
        queried == MFX_WRN_VIDEO_PARAM_CHANGED) {
        m_Params = corrected;
    } else {
        error = std::string("this GPU cannot encode ") + toString(codec) + ": " +
                VplApi::statusToString(queried);
        stop();
        return false;
    }

    auto initSucceeded = [](mfxStatus s) {
        return s == MFX_ERR_NONE || s == MFX_WRN_INCOMPATIBLE_VIDEO_PARAM ||
               s == MFX_WRN_VIDEO_PARAM_CHANGED;
    };

    m_IntraRefresh = false;
    if (intraRefresh) attachIntraRefresh(m_Params, m_CodingOption2, m_ExtBuffers, m_Fps);

    mfxStatus started = m_Session.api()->EncodeInit(m_Session.handle(), &m_Params);

    if (intraRefresh && initSucceeded(started)) {
        m_IntraRefresh = true;
    } else if (intraRefresh) {
        // Intra-refresh is an optimisation, not a requirement. A generation
        // that refuses it must still stream — falling back to keyframes costs
        // the receiver its self-repair, not its picture.
        log::warning(std::string("[native] oneVPL declined intra-refresh (") +
                     VplApi::statusToString(started) + ") — falling back to keyframes");
        m_ExtBuffers.clear();
        m_Params.ExtParam = nullptr;
        m_Params.NumExtParam = 0;
        started = m_Session.api()->EncodeInit(m_Session.handle(), &m_Params);
    }

    if (!initSucceeded(started)) {
        error = std::string("could not initialize the Intel encoder: ") +
                VplApi::statusToString(started);
        stop();
        return false;
    }

    // Ask the encoder how big a compressed frame can get rather than guessing:
    // an undersized bitstream buffer fails at the worst moment, on the largest
    // keyframe.
    mfxVideoParam actual = {};
    actual.mfx.CodecId = m_Params.mfx.CodecId;
    if (m_Session.api()->EncodeGetVideoParam(m_Session.handle(), &actual) == MFX_ERR_NONE &&
        actual.mfx.BufferSizeInKB > 0) {
        const mfxU32 multiplier =
            actual.mfx.BRCParamMultiplier > 0 ? actual.mfx.BRCParamMultiplier : 1;
        m_BitstreamData.resize(static_cast<size_t>(actual.mfx.BufferSizeInKB) * multiplier * 1024);
    }
    // A floor regardless: some runtimes report a buffer sized for the average
    // frame, and a keyframe is several times that.
    if (m_BitstreamData.size() < 4u * 1024u * 1024u) m_BitstreamData.resize(4u * 1024u * 1024u);

    std::memset(&m_Bitstream, 0, sizeof(m_Bitstream));
    m_Bitstream.Data = m_BitstreamData.data();
    m_Bitstream.MaxLength = static_cast<mfxU32>(m_BitstreamData.size());

    const std::string overrides = tuning.describe();
    log::info(
        "[native] oneVPL ready: " + std::to_string(width) + "x" + std::to_string(height) + "@" +
        std::to_string(m_Fps) + " " + toString(codec) + " 4:2:0 CBR " +
        std::to_string(bitrateKbps) + " kbps, VBV " +
        std::to_string(m_Params.mfx.BufferSizeInKB * m_Params.mfx.BRCParamMultiplier) + " KB" +
        (m_IntraRefresh
             ? ", intra-refresh over " + std::to_string(intraRefreshPeriodFrames(m_Fps)) + " frames"
             : ", keyframes on demand") +
        ", TU" + std::to_string(m_Params.mfx.TargetUsage) +
        (overrides.empty() ? "" : " [bench: " + overrides + "]") +
        " (UNVERIFIED — no Intel hardware has run this path yet)");
    return true;
}

bool VplEncoder::encode(ID3D11Texture2D* surface, bool forceKeyframe, uint32_t frameNumber,
                        EncoderOutput& out, std::string& error)
{
    // No reference invalidation on this path yet; the number is not needed.
    (void)frameNumber;
    if (!m_Session.isOpen()) {
        error = "the encoder is not initialized";
        return false;
    }
    if (m_OutputHeld) {
        error = "the previous frame was not released";
        return false;
    }
    if (!surface) {
        error = "no input surface";
        return false;
    }

    // The zero-copy hand-off: the texture our conversion pass wrote, named by
    // handle. Second element is the subresource index — 0, since the conversion
    // output is a plain non-array texture.
    mfxHDLPair handles = {};
    handles.first = static_cast<mfxHDL>(surface);
    handles.second = nullptr;

    mfxFrameSurface1 input = {};
    input.Info = m_Params.mfx.FrameInfo;
    input.Data.MemId = static_cast<mfxMemId>(&handles);

    mfxEncodeCtrl ctrl = {};
    mfxEncodeCtrl* ctrlPtr = nullptr;
    if (forceKeyframe) {
        ctrl.FrameType = MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR | MFX_FRAMETYPE_REF;
        ctrlPtr = &ctrl;
    }

    m_Bitstream.DataOffset = 0;
    m_Bitstream.DataLength = 0;

    mfxSyncPoint sync = nullptr;
    mfxStatus status = MFX_ERR_NONE;
    for (int attempt = 0; attempt < kBusyRetries; ++attempt) {
        status = m_Session.api()->EncodeFrameAsync(m_Session.handle(), ctrlPtr, &input,
                                                   &m_Bitstream, &sync);
        if (status == MFX_WRN_DEVICE_BUSY) {
            std::this_thread::yield();
            continue;
        }
        break;
    }

    if (status == MFX_ERR_MORE_DATA) {
        // The encoder swallowed the frame and wants another before it emits
        // anything. With AsyncDepth 1 and no B-frames this should not happen —
        // saying so is more useful than pretending a frame was produced.
        error = "the Intel encoder asked for more input than this pipeline provides";
        return false;
    }
    if (status != MFX_ERR_NONE || !sync) {
        error = std::string("encode failed: ") + VplApi::statusToString(status);
        return false;
    }

    const mfxStatus synced =
        m_Session.api()->SyncOperation(m_Session.handle(), sync, kSyncTimeoutMs);
    if (synced != MFX_ERR_NONE) {
        error =
            std::string("waiting for the encoded frame failed: ") + VplApi::statusToString(synced);
        return false;
    }

    m_OutputHeld = true;
    out.data = m_Bitstream.Data + m_Bitstream.DataOffset;
    out.size = m_Bitstream.DataLength;
    out.keyframe = (m_Bitstream.FrameType & (MFX_FRAMETYPE_I | MFX_FRAMETYPE_IDR)) != 0;
    return true;
}

void VplEncoder::releaseOutput()
{
    if (!m_OutputHeld) return;
    // Nothing to unlock: the buffer is ours. Resetting the fill marks it free
    // for the next frame, and clearing the flag is what lets encode() run again.
    m_Bitstream.DataOffset = 0;
    m_Bitstream.DataLength = 0;
    m_OutputHeld = false;
}

bool VplEncoder::setBitrate(int bitrateKbps, std::string& error)
{
    if (!m_Session.isOpen() || bitrateKbps <= 0) {
        error = "the encoder is not initialized";
        return false;
    }

    mfxVideoParam params;
    if (!fillEncodeParams(params, m_Codec, m_Width, m_Height, m_Fps, bitrateKbps, m_Tuning)) {
        error = "no oneVPL codec for this format";
        return false;
    }

    // Reset keeps the session and its surfaces; only the rate control changes.
    const mfxStatus status = m_Session.api()->EncodeReset(m_Session.handle(), &params);
    if (status != MFX_ERR_NONE && status != MFX_WRN_INCOMPATIBLE_VIDEO_PARAM &&
        status != MFX_WRN_VIDEO_PARAM_CHANGED) {
        error = std::string("could not change the bitrate: ") + VplApi::statusToString(status);
        return false;
    }
    m_Params = params;
    return true;
}

void VplEncoder::stop()
{
    releaseOutput();
    if (m_Session.isOpen()) m_Session.api()->EncodeClose(m_Session.handle());
    m_Session.close();
    m_BitstreamData.clear();
    std::memset(&m_Bitstream, 0, sizeof(m_Bitstream));
}

} // namespace mw::native::encode
