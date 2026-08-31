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

#include "NvencApi.h"
#include "mw/native/Capabilities.h"

#include <d3d11.h>

#include <cstdint>
#include <string>

namespace mw::native::encode {

/// One encoded frame, pointing into the encoder's locked bitstream.
///
/// Valid only until the next call on this encoder: the buffer is unlocked as
/// soon as the caller is done with it, which is what keeps the GPU→CPU copy at
/// exactly one per frame instead of one plus an allocation.
struct NvencOutput
{
    const uint8_t* data = nullptr;
    size_t size = 0;
    bool keyframe = false;
};

/// NVENC, configured for a screen-sharing stream rather than for a file.
///
/// ── Every setting here is a latency decision ───────────────────────────────
///
/// The defaults NVENC ships are tuned for encoding video you will watch later.
/// Almost all of them are wrong for a stream someone is playing on:
///
///  - **Ultra-low-latency tuning, no B-frames.** A B-frame references a picture
///    that has not been sent yet, so the encoder must hold frames back. That is
///    a whole frame of delay bought for a few percent of bitrate.
///  - **Infinite GOP + intra-refresh.** A periodic keyframe is a bitrate spike
///    every couple of seconds, and MediaTrackRelay documents what those spikes
///    do to a congested link: loss, then PLI storms, then collapse. Refreshing
///    a band of the picture on every frame spreads that cost flat.
///  - **CBR.** The link has a budget; the encoder should spend it evenly rather
///    than saving up for a burst that arrives as loss.
///  - **SPS/PPS on every keyframe.** The browser's decoder configures itself
///    from the parameter sets. A client that joins late, or loses the first
///    keyframe, must be able to start from the next one.
class NvencEncoder
{
public:
    NvencEncoder() = default;
    ~NvencEncoder();

    NvencEncoder(const NvencEncoder&) = delete;
    NvencEncoder& operator=(const NvencEncoder&) = delete;

    /// @param device the D3D11 device the NV12 frames live on. Must be the
    ///               adapter that captured them, or every frame costs a
    ///               cross-GPU copy.
    bool init(ID3D11Device* device, Codec codec, int width, int height, int fps, int bitrateKbps,
              std::string& error);

    /// Encode one NV12 texture. Blocking: returns with the bitstream ready.
    bool encode(ID3D11Texture2D* nv12, bool forceKeyframe, NvencOutput& out, std::string& error);

    /// Release the bitstream handed out by the last encode(). Must be called
    /// before the next encode().
    void releaseOutput();

    void stop();

    /// Move the target bitrate without restarting the session (§9.3 — driven
    /// live from what the client reports). Applied from the next frame.
    bool setBitrate(int bitrateKbps, std::string& error);

    bool intraRefreshEnabled() const { return m_IntraRefresh; }

private:
    bool registerInput(ID3D11Texture2D* texture, std::string& error);

    const NvencApi* m_Api = nullptr;
    void* m_Encoder = nullptr;

    NV_ENC_INITIALIZE_PARAMS m_InitParams = {};
    NV_ENC_CONFIG m_Config = {};

    /// The registered input surface, and the texture it was registered for.
    /// Registration is per texture, and ColorConvert reuses one output texture
    /// for the whole session — so this is done once, not once per frame.
    NV_ENC_REGISTERED_PTR m_Registered = nullptr;
    ID3D11Texture2D* m_RegisteredFor = nullptr;

    NV_ENC_OUTPUT_PTR m_Bitstream = nullptr;
    bool m_OutputLocked = false;

    Codec m_Codec = Codec::H264;
    int m_Width = 0;
    int m_Height = 0;
    bool m_IntraRefresh = false;
    uint64_t m_FrameIndex = 0;
};

} // namespace mw::native::encode
