/*
 * MoonlightWeb — native capture & encoding engine.
 * Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#pragma once

#include "IVideoEncoder.h"
#include "VplSession.h"

#include <vector>

namespace mw::native::encode {

/// Intel Quick Sync through oneVPL, configured for a screen stream.
///
/// ⚠️ **Written without hardware to test it on.** Every other encoder in this
/// tree was measured on the machine it targets; this one was not. It follows
/// the oneVPL documentation and mirrors the shape of the NVENC and AMF paths,
/// and it is built to fail loudly rather than quietly — but until it has
/// encoded a frame on a real Intel GPU, treat a success here as unproven.
///
/// ── The same latency decisions as the other two ────────────────────────────
///
/// AsyncDepth 1, no B-frames, an effectively infinite GOP with keyframes only
/// on demand, CBR with a one-frame buffer. See fillEncodeParams in
/// VplSession.h, where they live so the capability query and the encoder cannot
/// disagree about them.
///
/// ── What is deliberately absent ────────────────────────────────────────────
///
/// **No frame allocator.** The surfaces come from our own conversion pass, so
/// the encoder is handed a D3D11 texture per frame through mfxHDLPair and
/// oneVPL never allocates input of its own. That is what keeps the hand-off
/// zero-copy; it is also why IOPattern is IN_VIDEO_MEMORY.
///
/// **No 4:4:4.** The conversion pass produces AYUV for 4:4:4, which oneVPL does
/// not take as encoder input. The capability query says so, so the Selector
/// never routes such a session here, and init() refuses it outright rather than
/// silently encoding 4:2:0.
class VplEncoder final : public IVideoEncoder
{
public:
    VplEncoder() = default;
    ~VplEncoder() override;

    bool init(ID3D11Device* device, Codec codec, int width, int height, int fps, int bitrateKbps,
              bool yuv444, bool intraRefresh, std::string& error) override;

    bool encode(ID3D11Texture2D* surface, bool forceKeyframe, EncoderOutput& out,
                std::string& error) override;

    void releaseOutput() override;
    void stop() override;
    bool setBitrate(int bitrateKbps, std::string& error) override;

    bool intraRefreshEnabled() const override { return m_IntraRefresh; }

private:
    VplSession m_Session;
    mfxVideoParam m_Params = {};

    /// Storage for the intra-refresh extension buffer and the chain that points
    /// at it. Members rather than locals because oneVPL keeps the pointers: the
    /// parameter block is read again on Reset, and a dangling extension buffer
    /// there is a use-after-free the runtime cannot warn about.
    mfxExtCodingOption2 m_CodingOption2 = {};
    std::vector<mfxExtBuffer*> m_ExtBuffers;
    bool m_IntraRefresh = false;

    /// The output bitstream, reused every frame. Sized from what the encoder
    /// itself asks for, never guessed.
    std::vector<mfxU8> m_BitstreamData;
    mfxBitstream m_Bitstream = {};
    bool m_OutputHeld = false;

    Codec m_Codec = Codec::H264;
    int m_Width = 0;
    int m_Height = 0;
    int m_Fps = 60;
};

} // namespace mw::native::encode
