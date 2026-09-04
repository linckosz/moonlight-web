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

#include "mw/native/Capabilities.h"
#include "mw/native/EncoderTuning.h"

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace mw::native::encode {

/// One encoded frame, pointing into the encoder's own output buffer.
///
/// Valid only until the next call on this encoder: the buffer is released as
/// soon as the caller is done with it, which is what keeps the GPU→CPU copy at
/// exactly one per frame instead of one plus an allocation.
struct EncoderOutput
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool keyframe = false;

    /// The average quantizer the encoder reports for this frame, or -1 when it
    /// does not say. The objective proxy for quality in the benchmarks: at a
    /// fixed bitrate, a lower QP is a sharper picture. H.264/HEVC report a QP
    /// (0–51); AV1 reports a q-index (0–255) — same direction, other scale.
    int avgQp = -1;
};

/// A hardware encoder taking D3D11 textures.
///
/// The interface exists at the vendor boundary and nowhere else: capture and
/// colour conversion are identical whichever GPU is in the machine, and it is
/// only the encoder that differs. Every implementation takes the SAME NV12 (or
/// AYUV) texture the conversion pass wrote, on the same adapter — that is the
/// zero-copy contract, and an implementation that needs a staging copy has
/// broken it.
class IVideoEncoder
{
public:
    virtual ~IVideoEncoder() = default;

    IVideoEncoder(const IVideoEncoder&) = delete;
    IVideoEncoder& operator=(const IVideoEncoder&) = delete;

    /// @param device the D3D11 device the frames live on. Must be the adapter
    ///               that captured them.
    /// @param yuv444 encode 4:4:4 rather than 4:2:0. The caller must have
    ///               checked GpuInfo::supports444 first.
    /// @param intraRefresh encode with a moving band of intra blocks instead of
    ///                     relying on keyframes for recovery. Only ask when the
    ///                     receiver will decode through the damage; an encoder
    ///                     that cannot do it reports so from
    ///                     intraRefreshEnabled() rather than failing.
    /// @param tuning the bench's overrides of this encoder's own settings; a
    ///               default-constructed one IS the engine's choice, and is what
    ///               every session a browser starts passes.
    virtual bool init(ID3D11Device* device, Codec codec, int width, int height, int fps,
                      int bitrateKbps, bool yuv444, bool intraRefresh, const EncoderTuning& tuning,
                      std::string& error) = 0;

    /// Encode one texture. Blocking: returns with the bitstream ready.
    virtual bool encode(ID3D11Texture2D* surface, bool forceKeyframe, EncoderOutput& out,
                        std::string& error) = 0;

    /// Release the buffer handed out by the last encode(). Must be called
    /// before the next encode().
    virtual void releaseOutput() = 0;

    virtual void stop() = 0;

    /// Move the target bitrate without restarting the session — the basis for
    /// following the client's real feedback frame by frame.
    virtual bool setBitrate(int bitrateKbps, std::string& error) = 0;

    /// Whether the stream refreshes by intra-refresh rather than by periodic
    /// keyframes. Reported, not requested: not every encoder can do it.
    virtual bool intraRefreshEnabled() const = 0;

protected:
    IVideoEncoder() = default;
};

} // namespace mw::native::encode
