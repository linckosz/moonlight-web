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

#include <d3d11.h>
#include <wrl/client.h>

#include <string>

namespace mw::native::convert {

/// Turns a captured desktop texture into the format the encoder wants, on the
/// GPU, without ever touching system memory.
///
/// ── Why a render pass and not a compute shader ──────────────────────────────
///
/// NV12 is planar, and D3D11 cannot bind an unordered-access view to one plane
/// of it — plane slices only arrived with D3D12. What D3D11 *does* allow is a
/// render-target view typed to a plane: an R8_UNORM view of an NV12 texture
/// addresses its luma, an R8G8_UNORM view its chroma. So the conversion is two
/// small draws (full-screen triangle, no vertex buffer) rather than one
/// dispatch. Same work for the GPU, and it keeps the output a single real NV12
/// texture the encoder can take as-is.
///
/// ── What this costs ─────────────────────────────────────────────────────────
///
/// One GPU pass, zero CPU copies, and the result stays in VRAM on the adapter
/// that captured it. The scale to a smaller requested resolution rides along in
/// the same pass, so asking for 1080p from a 1440p screen costs nothing extra.
class ColorConvert
{
public:
    /// Which chroma sampling the encoder should receive.
    ///
    /// MoonlightWeb already offers this choice for external hosts, and it is
    /// not cosmetic on a desktop: 4:2:0 keeps a quarter of the colour
    /// resolution, which is invisible on video and very visible on text, thin
    /// UI lines and coloured code.
    enum class Chroma
    {
        /// NV12 — two planes, chroma at half resolution in both axes.
        C420,
        /// AYUV — one packed plane at full resolution. Simpler to produce than
        /// NV12 (a single draw, no plane views) and roughly twice the bytes for
        /// the encoder to read.
        C444,
    };

    ColorConvert() = default;
    ~ColorConvert();

    ColorConvert(const ColorConvert&) = delete;
    ColorConvert& operator=(const ColorConvert&) = delete;

    /// Prepare the pipeline for @p sourceFormat frames of @p sourceWidth ×
    /// @p sourceHeight, producing @p chroma at @p outputWidth × @p outputHeight.
    ///
    /// For 4:2:0 the output dimensions are rounded down to even numbers: NV12
    /// chroma is half-resolution in both axes, so an odd size has no
    /// representation. 4:4:4 has no such constraint but is rounded the same way
    /// to keep one code path and to stay friendly to every encoder.
    bool init(ID3D11Device* device, DXGI_FORMAT sourceFormat, int sourceWidth, int sourceHeight,
              int outputWidth, int outputHeight, Chroma chroma, std::string& error);

    Chroma chroma() const { return m_Chroma; }

    /// Convert one frame. @p source is the texture from capture; the result is
    /// in output(), ready for the encoder to register.
    bool convert(ID3D11Texture2D* source, std::string& error);

    /// The texture the last convert() wrote — NV12 or AYUV per chroma().
    /// Owned here and reused every frame: allocating one per frame would be a
    /// VRAM allocation on the hot path for no reason.
    ID3D11Texture2D* output() const { return m_Output.Get(); }

    int outputWidth() const { return m_OutputWidth; }
    int outputHeight() const { return m_OutputHeight; }

private:
    bool createShaders(std::string& error);
    bool createOutput(std::string& error);

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_VertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_LumaShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ChromaShader;
    /// 4:4:4 needs only this one: a single draw writes all three components.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_PackedShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_Sampler;

    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Output;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_LumaTarget;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_ChromaTarget;
    /// The single target of the 4:4:4 path.
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_PackedTarget;

    /// A shader-resource view of the CAPTURED texture. Rebuilt whenever the
    /// texture changes identity: Desktop Duplication may hand back a different
    /// texture object from one frame to the next, and a stale view would sample
    /// the previous frame forever — a freeze that looks like a network fault.
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_SourceView;
    ID3D11Texture2D* m_SourceViewFor = nullptr;

    Chroma m_Chroma = Chroma::C420;
    DXGI_FORMAT m_SourceFormat = DXGI_FORMAT_UNKNOWN;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_OutputWidth = 0;
    int m_OutputHeight = 0;
};

} // namespace mw::native::convert
