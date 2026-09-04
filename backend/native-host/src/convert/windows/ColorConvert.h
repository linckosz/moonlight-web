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

#include "../../capture/windows/IWindowsCapture.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <string>

namespace mw::native::convert {

/// How the pointer is placed on the picture, beyond the position and shape the
/// capture already reports.
///
/// Separate from CursorState because none of it comes from the capture: the
/// magnification is a client request, and the hotspot — which CursorState has
/// already subtracted out of its position — has to come back for the pointer to
/// grow around the point it aims with rather than around its top-left corner.
struct CursorDraw
{
    /// Multiplier on the drawn size. 1 is the size the pointer has on the
    /// desktop, which is what everything but a small screen wants.
    float magnify = 1.0f;

    /// The point inside the shape that IS the pointer position, in cursor
    /// pixels. Only consulted when magnifying: it is the fixed point of the
    /// growth, so the tip of an enlarged arrow stays exactly where the real one
    /// was.
    int hotspotX = 0;
    int hotspotY = 0;
};

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

    /// Whether init() will accept frames in @p format.
    ///
    /// Asked BEFORE the capture is opened, because the answer decides what the
    /// capture is asked for: an SDR session must not receive FP16 from an HDR
    /// desktop, it must receive the desktop tone-mapped to 8-bit by DXGI. That
    /// choice is made by what the CAPTURE is opened with, not here — this only
    /// says which formats have a shader.
    ///
    /// FP16 scRGB is accepted since the HDR path landed (September 2026); it is
    /// only usable with `hdr` set on init(), which is the pairing the session
    /// enforces.
    static bool supportsSource(DXGI_FORMAT format)
    {
        return format == DXGI_FORMAT_B8G8R8A8_UNORM || format == DXGI_FORMAT_R8G8B8A8_UNORM ||
               format == DXGI_FORMAT_R16G16B16A16_FLOAT;
    }

    /// Whether @p format carries HDR — i.e. requires the PQ path rather than
    /// the BT.709 one. The capture's answer is the truth here: asking DXGI for
    /// FP16 does not guarantee getting it (a display that left HDR mode hands
    /// back 8-bit), so the session reconciles what it wanted with this.
    static bool isHdrSource(DXGI_FORMAT format) { return format == DXGI_FORMAT_R16G16B16A16_FLOAT; }

    /// Prepare the pipeline for @p sourceFormat frames of @p sourceWidth ×
    /// @p sourceHeight, producing @p chroma at @p outputWidth × @p outputHeight.
    ///
    /// @p hdr selects the HDR path: an FP16 scRGB source becomes **P010**,
    /// BT.2020 primaries with the PQ transfer, 10-bit limited range. It must
    /// match the source format — FP16 with `hdr` false, or an 8-bit source with
    /// `hdr` true, is refused rather than silently misinterpreted, because both
    /// mistakes produce a picture that is merely wrong instead of an error.
    ///
    /// HDR is 4:2:0 only. The 10-bit 4:4:4 format (Y410) has no browser that
    /// displays it — Chrome 152 accepts `hvc1.4.156` and then renders green
    /// (measured 04/09/2026, doc §15/F0f) — so producing it would cost a shader
    /// nobody could watch.
    ///
    /// For 4:2:0 the output dimensions are rounded down to even numbers: NV12
    /// and P010 chroma are half-resolution in both axes, so an odd size has no
    /// representation. 4:4:4 has no such constraint but is rounded the same way
    /// to keep one code path and to stay friendly to every encoder.
    bool init(ID3D11Device* device, DXGI_FORMAT sourceFormat, int sourceWidth, int sourceHeight,
              int outputWidth, int outputHeight, Chroma chroma, bool hdr, std::string& error);

    Chroma chroma() const { return m_Chroma; }

    /// Whether the output is P010 in BT.2020 PQ rather than 8-bit BT.709.
    bool hdr() const { return m_Hdr; }

    /// Convert one frame. @p source is the texture from capture; the result is
    /// in output(), ready for the encoder to register.
    ///
    /// @p cursor is drawn into the picture on the way through. Pass a state with
    /// `visible == false` for none. The shape is re-uploaded only when its
    /// `shapeVersion` changes, so a cursor that merely moves costs nothing but
    /// a constant-buffer write — and @p draw, which only moves the rectangle the
    /// shader samples over, costs nothing at all.
    bool convert(ID3D11Texture2D* source, const capture::CursorState& cursor,
                 const CursorDraw& draw, std::string& error);

    /// The texture the last convert() wrote — NV12 or AYUV per chroma().
    /// Owned here and reused every frame: allocating one per frame would be a
    /// VRAM allocation on the hot path for no reason.
    ID3D11Texture2D* output() const { return m_Output.Get(); }

    int outputWidth() const { return m_OutputWidth; }
    int outputHeight() const { return m_OutputHeight; }

private:
    bool createShaders(std::string& error);
    bool createOutput(std::string& error);

    /// Re-upload the cursor's small textures, and tell the shader where to put
    /// them. Cheap on every frame but the ones where the shape changed.
    bool updateCursorResources(const capture::CursorState& cursor, std::string& error);

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;

    Microsoft::WRL::ComPtr<ID3D11VertexShader> m_VertexShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_LumaShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ChromaShader;
    /// The HDR pair. Compiled only for an HDR session: the PQ shaders are the
    /// expensive ones to compile and a desktop session never needs them.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_LumaHdrShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ChromaHdrShader;
    /// 4:4:4 needs only this one: a single draw writes all three components.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_PackedShader;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_Sampler;
    /// Point sampling for the cursor's invert mask: a half-inverted pixel is
    /// not a thing, and interpolating the flag fringes the I-beam.
    Microsoft::WRL::ComPtr<ID3D11SamplerState> m_NearestSampler;

    /// The cursor, as two small textures plus where to draw them. Recreated
    /// only when the shape changes — which, for a cursor being moved around, is
    /// approximately never.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_CursorPixels;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_CursorPixelsView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_CursorInvert;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_CursorInvertView;
    Microsoft::WRL::ComPtr<ID3D11Buffer> m_OverlayBuffer;
    uint64_t m_CursorShapeVersion = 0;

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
    bool m_Hdr = false;
    DXGI_FORMAT m_SourceFormat = DXGI_FORMAT_UNKNOWN;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_OutputWidth = 0;
    int m_OutputHeight = 0;
};

} // namespace mw::native::convert
