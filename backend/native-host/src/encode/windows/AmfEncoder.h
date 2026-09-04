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

#include "AmfApi.h"
#include "IVideoEncoder.h"

#include <wrl/client.h>

namespace mw::native::encode {

/// AMF, configured for a screen-sharing stream rather than for a file.
///
/// The same latency decisions as the NVENC path, taken against the same wrong
/// defaults: ultra-low-latency usage, no B-frames, CBR with a one-frame VBV,
/// and a GOP long enough that no periodic keyframe is ever emitted.
///
/// ── Intra-refresh ───────────────────────────────────────────────────────────
///
/// Supported here, on all three codecs, through the property AMD gives each:
/// `IntraRefreshMBsNumberPerSlot` (H.264, in 16×16 macroblocks),
/// `HevcIntraRefreshCTBsNumberPerSlot` (in 64×64 CTBs), and
/// `Av1IntraRefreshMode` set to CONTINUOUS with a stripe count.
///
/// Enabled only when the caller asks, because the benefit belongs to the
/// receiver: a client that decodes through the damage repairs itself within one
/// cycle, while one that discards deltas and demands an IDR — MoonlightWeb's
/// default — collects nothing and still pays in slightly larger P-frames.
///
/// **Property names are per codec.** AMF has no shared namespace: the same
/// concept is `TargetBitrate`, `HevcTargetBitrate` or `Av1TargetBitrate`. They
/// are gathered in one table (see the .cpp) so the configuration logic is
/// written once rather than three times.
class AmfEncoder final : public IVideoEncoder
{
public:
    AmfEncoder() = default;
    ~AmfEncoder() override;

    bool init(ID3D11Device* device, Codec codec, int width, int height, int fps, int bitrateKbps,
              bool yuv444, bool intraRefresh, const EncoderTuning& tuning,
              std::string& error) override;

    bool encode(ID3D11Texture2D* surface, bool forceKeyframe, uint32_t frameNumber,
                EncoderOutput& out, std::string& error) override;

    void releaseOutput() override;
    void stop() override;
    bool setBitrate(int bitrateKbps, std::string& error) override;
    bool intraRefreshEnabled() const override { return m_IntraRefresh; }

private:
    const AmfApi* m_Api = nullptr;
    amf::AMFContextPtr m_Context;
    amf::AMFComponentPtr m_Encoder;

    /// The buffer handed out by the last encode(). Held so releaseOutput() can
    /// drop it and so a caller that forgets cannot silently corrupt the next
    /// frame by encoding over a buffer still being sent.
    amf::AMFBufferPtr m_Output;

    Codec m_Codec = Codec::H264;
    int m_Width = 0;
    int m_Height = 0;
    int m_Fps = 60;
    /// What the encoder was actually configured with — reported, not wished for.
    bool m_IntraRefresh = false;
    /// The bench's VBV override, so setBitrate() sizes the buffer by the rule
    /// init() used. 0 is the engine's own rule.
    int m_VbvFrames = 0;
};

} // namespace mw::native::encode
