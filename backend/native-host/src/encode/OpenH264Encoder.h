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

#include "EncoderOutput.h"
#include "mw/native/EncoderTuning.h"

#include <cstdint>
#include <string>
#include <vector>

class ISVCEncoder;

namespace mw::native::encode {

/// A picture in system memory, three planes, 8-bit 4:2:0 — what OpenH264 eats.
struct I420Picture
{
    const uint8_t* y = nullptr;
    const uint8_t* u = nullptr;
    const uint8_t* v = nullptr;
    int strideY = 0;
    int strideU = 0;
    int strideV = 0;
    int width = 0;
    int height = 0;
};

/// OpenH264 on the CPU: the encoder of last resort, and on Linux without a
/// render node the only one. Platform-neutral — it knows nothing of textures or
/// devices; each platform hands it a picture in system memory and owns the
/// copy that got it there (SoftwareEncoder on Windows, the CPU pipeline on
/// Linux).
///
/// ── The latency decisions, in OpenH264's vocabulary ─────────────────────────
///
/// OpenH264 was written for real-time calls, which is the right starting point:
/// it has no B-frames and no lookahead to switch off. What it does have to be
/// told is:
///
///   - **threads by slices, never by frames.** `iMultipleThreadIdc` with
///     `SM_FIXEDSLCNUM_SLICE` splits ONE picture across cores; a frame-level
///     pipeline would buy throughput with a whole frame of latency, which is
///     the one trade this engine never makes;
///   - **no frame skipping.** The rate control may not drop a picture to hold
///     the bitrate; cadence is this engine's business (EffectiveCadence), and a
///     frame that silently vanishes reads as a stall at the client;
///   - **CAVLC**, `LOW_COMPLEXITY`, no denoise, no scene-change or background
///     analysis, no adaptive quantization: every one of those is CPU spent on
///     a picture the browser will have replaced sixteen milliseconds later;
///   - CBR, one-frame VBV (through `iMaxBitrate` = target and the fixed
///     overshoot control), keyframes on request only.
///
/// The picture format is I420 — planar — where every other encoder here takes
/// NV12. The platform layer deinterleaves the chroma on the way to system
/// memory; it is a byte shuffle over a quarter of the picture, and the copy to
/// system memory was already being paid.
///
/// ── What it does not do (yet) ──────────────────────────────────────────────
///
/// No intra-refresh (OpenH264 has none) and no reference invalidation: it has
/// long-term references and a recovery request, which map onto ReferenceSlots
/// the way AMF's do, but that is a later step with its own measurement. A lost
/// frame costs a keyframe here.
class OpenH264Encoder
{
public:
    OpenH264Encoder() = default;
    ~OpenH264Encoder();

    OpenH264Encoder(const OpenH264Encoder&) = delete;
    OpenH264Encoder& operator=(const OpenH264Encoder&) = delete;

    /// @param threads how many slices — and cores — one picture is split
    ///                across. 0 picks from the machine (capped: past four the
    ///                slices cost more bits than the cores save time).
    bool init(int width, int height, int fps, int bitrateKbps, int threads,
              const EncoderTuning& tuning, std::string& error);

    /// Blocking. `out.size == 0` with a true return means the rate control
    /// produced nothing for this picture (never with frame skipping off, kept
    /// for honesty).
    bool encode(const I420Picture& picture, bool forceKeyframe, uint32_t frameNumber,
                EncoderOutput& out, std::string& error);

    void releaseOutput();
    void stop();
    bool setBitrate(int bitrateKbps, std::string& error);

    int threads() const { return m_Threads; }
    static const char* version();

private:
    ISVCEncoder* m_Encoder = nullptr;
    /// The bitstream of the last encode() when it had to be gathered from more
    /// than one layer buffer; a single-layer answer is handed out in place.
    std::vector<uint8_t> m_Gathered;
    int m_Width = 0;
    int m_Height = 0;
    int m_Fps = 60;
    int m_Threads = 1;
    int m_FramesIn = 0;
    /// The target in force, in bits per second: setBitrate() has to know which
    /// way it is moving (see there).
    int m_TargetBps = 0;
};

} // namespace mw::native::encode
