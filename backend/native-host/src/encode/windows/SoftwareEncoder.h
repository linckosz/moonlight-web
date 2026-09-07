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

#include "../OpenH264Encoder.h"
#include "IVideoEncoder.h"

#include <wrl/client.h>

#include <vector>

namespace mw::native::encode {

/// OpenH264 behind the D3D11 encoder interface: the last rung of the Windows
/// fallback tier, below every Media Foundation transform.
///
/// The one thing this class does that the platform-neutral encoder cannot is
/// get the picture off the GPU: the converter's NV12 texture is copied to a
/// staging texture, mapped, and rewritten as three I420 planes in system
/// memory — the luma rows as they are, the interleaved chroma split in two.
/// That is the GPU→CPU copy every hardware path avoids and this one cannot; it
/// is measured in the session's `encode` stage, where it belongs, and it is
/// one copy, not two: the deinterleave happens in the same pass as the read.
class SoftwareEncoder final : public IVideoEncoder
{
public:
    SoftwareEncoder() = default;
    ~SoftwareEncoder() override = default;

    bool init(ID3D11Device* device, Codec codec, int width, int height, int fps, int bitrateKbps,
              bool yuv444, bool hdr, bool intraRefresh, const EncoderTuning& tuning,
              std::string& error) override;

    bool encode(ID3D11Texture2D* surface, bool forceKeyframe, uint32_t frameNumber,
                EncoderOutput& out, std::string& error) override;

    void releaseOutput() override { m_Core.releaseOutput(); }
    void stop() override;
    bool setBitrate(int bitrateKbps, std::string& error) override
    {
        return m_Core.setBitrate(bitrateKbps, error);
    }
    bool intraRefreshEnabled() const override { return false; }

private:
    bool readback(ID3D11Texture2D* surface, I420Picture& picture, std::string& error);

    OpenH264Encoder m_Core;
    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Staging;
    /// The three planes, one allocation: Y, then U, then V.
    std::vector<uint8_t> m_Planes;
    int m_Width = 0;
    int m_Height = 0;
};

} // namespace mw::native::encode
