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

#pragma once

#include "../EncoderOutput.h"
#include "mw/native/Capabilities.h"
#include "mw/native/EncoderTuning.h"

#include <CoreVideo/CoreVideo.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Hardware encoding on macOS through VideoToolbox — the Apple Video Encoder
// behind every Mac since 2016 (H.264) and every Apple Silicon Mac (HEVC too).
// The macOS counterpart of NvencEncoder/AmfEncoder/VaapiEncoder.
//
// ── The same latency decisions as the other four ────────────────────────────
//
// Real-time mode, no frame reordering (so no B-frames and one frame in flight),
// a GOP with keyframes only on demand, CBR with the one-frame VBV of
// RateControl.h expressed as VideoToolbox's data-rate limit, and the encoder's
// own "speed over quality" switch on. Each encode() blocks until the bitstream
// is out: VideoToolbox is asynchronous by design, and the wait for exactly the
// frame just submitted is what keeps the loop's "one frame in flight" true.
//
// ── What VideoToolbox does not offer ────────────────────────────────────────
//
// No intra-refresh, no reference invalidation, and no per-frame QP report. A
// lost frame therefore costs a keyframe, SessionInfo says so, and the
// refinement burst converges on size and its pass cap alone (RefineConvergence
// is written for an encoder that reports no QP — AMF was the first).
//
// ── AVCC to Annex B ─────────────────────────────────────────────────────────
//
// VideoToolbox emits length-prefixed NAL units with the parameter sets kept
// aside in the format description; the browser (and every relay above this
// module) wants Annex B with the SPS/PPS/VPS ahead of each keyframe. The
// prefixes are 4 bytes and so are start codes, so a delta frame is rewritten IN
// PLACE in the encoder's own buffer and handed out without a copy. A keyframe —
// rare — is assembled into a scratch buffer with its parameter sets in front.

namespace mw::native::encode {

class VtEncoder
{
public:
    VtEncoder();
    ~VtEncoder();

    VtEncoder(const VtEncoder&) = delete;
    VtEncoder& operator=(const VtEncoder&) = delete;

    /// Configure a hardware @p codec encoder for @p width × @p height NV12 at
    /// @p fps and @p bitrateKbps. H.264 and HEVC (Main, 8-bit); AV1 is refused
    /// — no Apple encoder produces it.
    bool init(Codec codec, int width, int height, int fps, int bitrateKbps,
              const EncoderTuning& tuning, std::string& error);

    /// Encode @p pixels (the capture's NV12 buffer, read in place). Blocking:
    /// returns with the Annex B bitstream in @p out.
    bool encode(CVPixelBufferRef pixels, bool forceKeyframe, int64_t presentUs, EncoderOutput& out,
                std::string& error);

    /// Release the buffer handed out by the last encode(). Must be called
    /// before the next encode().
    void releaseOutput();

    /// Change the bitrate between two frames, no restart. The data-rate limit
    /// follows, through the same floor as init().
    bool setBitrate(int bitrateKbps, std::string& error);

    /// Never: see the file comment. Here so the session reads the same
    /// questions off every encoder.
    bool intraRefreshEnabled() const { return false; }
    int intraRefreshFrames() const { return 0; }

    void stop();

    /// Public because VideoToolbox's C callback has to reach it; opaque here.
    struct Impl;

private:
    std::unique_ptr<Impl> d;

    bool applyRate(int bitrateKbps, std::string& error);
    /// Rewrite one AVCC sample into Annex B, into the encoder's own buffer
    /// when possible (delta) or the scratch buffer (keyframe).
    bool toAnnexB(bool keyframe, EncoderOutput& out, std::string& error);

    Codec m_Codec = Codec::H264;
    int m_Width = 0;
    int m_Height = 0;
    int m_Fps = 60;
    int m_BitrateKbps = 20000;
    EncoderTuning m_Tuning;

    std::vector<uint8_t> m_Scratch;
    bool m_OutputHeld = false;
    uint32_t m_Encoded = 0;
};

} // namespace mw::native::encode
