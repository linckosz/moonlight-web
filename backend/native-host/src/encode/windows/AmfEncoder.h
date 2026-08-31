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
/// ── Where it differs from NVENC, and why ────────────────────────────────────
///
/// **No intra-refresh — and NOT because AMF lacks it.** All three codecs expose
/// it: `IntraRefreshMBsNumberPerSlot` (H.264),
/// `HevcIntraRefreshCTBsNumberPerSlot`, and `Av1IntraRefreshMode` with a
/// CONTINUOUS mode. It is unimplemented here for a reason that has nothing to
/// do with AMD.
///
/// The benefit of intra-refresh is that a client which loses data heals itself
/// within one refresh cycle. MoonlightWeb never collects that: on any gap
/// DataChannelRelay sets `m_AwaitingIdr`, discards deltas and asks for a real
/// keyframe. The flat-bitrate half of the benefit is already had from the
/// infinite GOP above, which costs nothing.
///
/// So intra-refresh is currently paid for (slightly larger P-frames at a fixed
/// CBR budget) and never cashed in. NVENC has it because its API put it in
/// three plain fields; writing it a second and third time would duplicate a
/// no-op. Worth revisiting the day the receiver is taught to ride out a refresh
/// window instead of gating — that is the change that would make it earn its
/// keep, on all three vendors at once.
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
              bool yuv444, std::string& error) override;

    bool encode(ID3D11Texture2D* surface, bool forceKeyframe, EncoderOutput& out,
                std::string& error) override;

    void releaseOutput() override;
    void stop() override;
    bool setBitrate(int bitrateKbps, std::string& error) override;
    bool intraRefreshEnabled() const override { return false; }

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
};

} // namespace mw::native::encode
