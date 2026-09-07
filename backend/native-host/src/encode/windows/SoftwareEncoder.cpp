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

#include "SoftwareEncoder.h"

#include "../../core/Log.h"

#include <cstring>

namespace mw::native::encode {

bool SoftwareEncoder::init(ID3D11Device* device, Codec codec, int width, int height, int fps,
                           int bitrateKbps, bool yuv444, bool hdr, bool intraRefresh,
                           const EncoderTuning& tuning, std::string& error)
{
    (void)intraRefresh; // OpenH264 has none; reported false
    if (codec != Codec::H264) {
        error = std::string("OpenH264 encodes H.264 only, not ") + toString(codec);
        return false;
    }
    if (yuv444 || hdr) {
        // The Selector never grants either on the fallback tier; the guard
        // against a misrouted session, as on every other encoder.
        error = "OpenH264 encodes 8-bit 4:2:0 only";
        return false;
    }
    m_Device = device;
    m_Device->GetImmediateContext(&m_Context);
    m_Width = width;
    m_Height = height;
    m_Staging.Reset();
    m_Planes.assign(static_cast<size_t>(width) * height * 3 / 2, 0);
    return m_Core.init(width, height, fps, bitrateKbps, 0, tuning, error);
}

bool SoftwareEncoder::readback(ID3D11Texture2D* surface, I420Picture& picture, std::string& error)
{
    if (!m_Staging) {
        D3D11_TEXTURE2D_DESC desc = {};
        surface->GetDesc(&desc);
        if (desc.Format != DXGI_FORMAT_NV12) {
            error = "the software encoder expects an NV12 texture";
            return false;
        }
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        desc.MiscFlags = 0;
        const HRESULT hr = m_Device->CreateTexture2D(&desc, nullptr, &m_Staging);
        if (FAILED(hr)) {
            error = "could not create the staging texture";
            return false;
        }
    }

    // The GPU→CPU copy this path cannot avoid. Map waits for the copy: the
    // only way to hand the encoder a finished picture.
    m_Context->CopyResource(m_Staging.Get(), surface);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(m_Context->Map(m_Staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
        error = "could not map the staging texture";
        return false;
    }

    const auto* src = static_cast<const uint8_t*>(mapped.pData);
    const size_t pitch = mapped.RowPitch;
    const size_t w = static_cast<size_t>(m_Width);
    const size_t h = static_cast<size_t>(m_Height);
    uint8_t* y = m_Planes.data();
    uint8_t* u = y + w * h;
    uint8_t* v = u + (w / 2) * (h / 2);

    for (size_t row = 0; row < h; ++row)
        std::memcpy(y + w * row, src + pitch * row, w);

    // NV12 staging: the interleaved chroma plane follows the luma plane at the
    // same pitch. Split it in the same pass that reads it.
    const uint8_t* uv = src + pitch * h;
    for (size_t row = 0; row < h / 2; ++row) {
        const uint8_t* in = uv + pitch * row;
        uint8_t* outU = u + (w / 2) * row;
        uint8_t* outV = v + (w / 2) * row;
        for (size_t x = 0; x < w / 2; ++x) {
            outU[x] = in[2 * x];
            outV[x] = in[2 * x + 1];
        }
    }
    m_Context->Unmap(m_Staging.Get(), 0);

    picture.y = y;
    picture.u = u;
    picture.v = v;
    picture.strideY = static_cast<int>(w);
    picture.strideU = static_cast<int>(w / 2);
    picture.strideV = static_cast<int>(w / 2);
    picture.width = m_Width;
    picture.height = m_Height;
    return true;
}

bool SoftwareEncoder::encode(ID3D11Texture2D* surface, bool forceKeyframe, uint32_t frameNumber,
                             EncoderOutput& out, std::string& error)
{
    I420Picture picture;
    if (!readback(surface, picture, error)) return false;
    return m_Core.encode(picture, forceKeyframe, frameNumber, out, error);
}

void SoftwareEncoder::stop()
{
    m_Core.stop();
    m_Staging.Reset();
    m_Context.Reset();
    m_Device.Reset();
    m_Planes.clear();
}

} // namespace mw::native::encode
