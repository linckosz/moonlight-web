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

#include "../../convert/linux/GlConvert.h"
#include "../EncoderOutput.h"
#include "mw/native/Capabilities.h"
#include "mw/native/EncoderTuning.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

// Hardware encoding on Linux through VA-API — AMD and Intel behind one API,
// which is the reason it exists. The Linux counterpart of NvencEncoder/AmfEncoder.
//
// ── The same latency decisions as the other three ───────────────────────────
//
// No B-frames (one reference, one frame in flight), an effectively infinite GOP
// with keyframes only on demand, CBR with a one-frame VBV that has the same
// floor RateControl.h gives every other encoder, and intra-refresh where the
// driver offers it. Every one of those was measured on Windows and none of
// them is negotiable here.
//
// ── The surface the encoder owns ────────────────────────────────────────────
//
// On D3D11 the converter writes an NV12 texture and the encoder registers it.
// Here it is the other way round, because VA-API allocates its own surfaces:
// the encoder creates the NV12 input surface, EXPORTS it as two DMA-BUF planes,
// and GlConvert renders into those. Same zero-copy result, opposite ownership —
// and the reason inputTarget() exists.
//
// ── What the driver writes for us ───────────────────────────────────────────
//
// SPS/PPS and slice headers are the driver's: Mesa's radeonsi (and Intel's iHD)
// generate them from the sequence and picture parameters, and re-emit the
// parameter sets on every IDR — which is what a browser that joins late or
// loses the first keyframe needs. Packed headers supplied by the application
// are supported by both and used by FFmpeg; they are NOT sent here until a
// measurement shows the driver's own are missing something (the VUI's
// bitstream_restriction, say — the B8 lesson from NVENC).

namespace mw::native::encode {

class VaapiEncoder
{
public:
    VaapiEncoder();
    ~VaapiEncoder();

    VaapiEncoder(const VaapiEncoder&) = delete;
    VaapiEncoder& operator=(const VaapiEncoder&) = delete;

    /// Open VA-API on @p renderNode and configure a @p codec encoder for
    /// @p width × @p height at @p fps and @p bitrateKbps. H.264 and HEVC; AV1
    /// is refused until it has been watched on hardware.
    bool init(const std::string& renderNode, Codec codec, int width, int height, int fps,
              int bitrateKbps, bool intraRefresh, const EncoderTuning& tuning, std::string& error);

    /// The input surface, as GlConvert wants it. Valid after init(); the fds
    /// belong to the encoder and live until stop().
    const convert::Nv12Target& inputTarget() const { return m_Input; }

    /// Encode whatever the converter last wrote into the input surface.
    /// Blocking: returns with the bitstream ready.
    bool encode(bool forceKeyframe, uint32_t frameNumber, EncoderOutput& out, std::string& error);

    /// Release the buffer handed out by the last encode(). Must be called
    /// before the next encode().
    void releaseOutput();

    /// Change the bitrate between two frames, no restart. The VBV follows,
    /// through the same floor as init().
    bool setBitrate(int bitrateKbps, std::string& error);

    /// Whether the stream really refreshes by intra-refresh: what the driver
    /// DOES, never what was asked.
    bool intraRefreshEnabled() const { return m_IntraRefresh; }
    int intraRefreshFrames() const { return m_IntraRefreshPeriod; }

    void stop();

private:
    struct Impl;
    std::unique_ptr<Impl> d;

    bool openDisplay(const std::string& renderNode, std::string& error);
    bool chooseProfile(Codec codec, std::string& error);
    bool createSurfaces(std::string& error);
    bool exportInput(std::string& error);
    bool renderRateControl(std::string& error);
    bool renderH264(bool idr, std::string& error);
    bool renderHevc(bool idr, std::string& error);

    Codec m_Codec = Codec::H264;
    int m_Width = 0;
    int m_Height = 0;
    int m_Fps = 60;
    int m_BitrateKbps = 20000;
    EncoderTuning m_Tuning;

    convert::Nv12Target m_Input;

    bool m_IntraRefresh = false;
    int m_IntraRefreshPeriod = 0;
    /// Which column band the rolling refresh is at, in macroblocks (H.264) or
    /// CTBs (HEVC). Advances every encoded frame.
    int m_RefreshPosition = 0;

    uint32_t m_FrameNum = 0;
    uint32_t m_IdrPicId = 0;
    bool m_HaveReference = false;
    bool m_OutputHeld = false;
    bool m_RateDirty = true;

    std::vector<uint8_t> m_Bitstream;
};

} // namespace mw::native::encode
