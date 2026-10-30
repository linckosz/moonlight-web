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
#include "../CursorDraw.h"
#include "../ResampleCost.h"
#include "../ScaleFilter.h"
#include "MwsrStats.h"
#include "PassTimer.h"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <memory>
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
/// that captured it. At 1:1 that is all there is. A smaller stream is either
/// scaled inside the same pass by the sampler (ScaleFilter::Bilinear — free,
/// and aliased) or, on the tiers that can afford a few hundred microseconds,
/// resampled first by two 1-D Lanczos-2 passes into an output-sized picture
/// the conversion then reads 1:1 (ScaleFilter::Lanczos2). See the enum.
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

    /// How the picture is brought down to a smaller stream — see ScaleFilter.h.
    using ScaleFilter = convert::ScaleFilter;

    /// Whether init() will accept frames in @p format.
    ///
    /// FP16 scRGB is accepted since the HDR path landed (September 2026), and
    /// on both kinds of session: with `hdr` it becomes P010, without it the
    /// desktop is tone-mapped down to 8-bit here — see init().
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
    /// BT.2020 primaries with the PQ transfer, 10-bit limited range. An 8-bit
    /// source with `hdr` true is refused rather than silently misinterpreted:
    /// that mistake produces a picture that is merely wrong instead of an
    /// error.
    ///
    /// An FP16 source with `hdr` false is the SDR stream of an HDR desktop.
    /// DXGI could hand that over in 8-bit itself, but its idea of SDR is a
    /// clip at 80 nits, and a desktop whose "SDR content brightness" slider
    /// is up — that is, any desktop someone has looked at — arrives blown
    /// out. So the frames are taken as scRGB and **tone-mapped here**: SDR
    /// white brought to 1.0 (see setSdrWhite), identity below the knee, the
    /// HDR highlights folded into what is left, then the ordinary BT.709
    /// output. Same output formats as the 8-bit source; toneMapsToSdr() says
    /// which of the two this is.
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
              int outputWidth, int outputHeight, Chroma chroma, bool hdr, ScaleFilter filter,
              std::string& error);

    Chroma chroma() const { return m_Chroma; }

    /// The filter in effect: Bilinear whenever nothing is being scaled,
    /// whatever was asked — the resample pass has no work at 1:1.
    ScaleFilter scaleFilter() const { return m_Filter; }

    /// Give up the resample pass for the bilinear fetch of the conversion,
    /// between two frames, without rebuilding anything: the output texture —
    /// the one the encoder registered — stays the same. For a GPU that cannot
    /// afford Lanczos-2 at the stream's rate. False, and nothing changed, when
    /// there is no resample pass, or when the picture is letterboxed (the
    /// bilinear path stretches instead, a different picture mid-stream).
    bool dropResample();

    /// What the resample pass costs this GPU, once: true exactly once per
    /// init(), when enough frames have been timed, with the median GPU time in
    /// @p costUs. The call to act on it — see ResampleCost. The timing runs on
    /// the first few dozen frames of the pass and never again.
    bool takeResampleCost(int64_t& costUs);

    /// Whether the picture sits between bars: a source of another shape than
    /// the output, on the resample path. The stretch of the bilinear path is
    /// not letterboxing.
    bool letterboxed() const { return m_Letterboxed; }

    /// Whether the output is P010 in BT.2020 PQ rather than 8-bit BT.709.
    bool hdr() const { return m_Hdr; }

    /// Whether this is an SDR output made from an FP16 source — the tone map.
    bool toneMapsToSdr() const { return m_ToneMap; }

    /// Whether the source is FP16 scRGB, on either path — the ones that need
    /// setSdrWhite() kept current.
    bool scRgbSource() const { return m_Hdr || m_ToneMap; }

    /// Where the desktop's SDR white sits in scRGB: the display's
    /// DISPLAYCONFIG_SDR_WHITE_LEVEL over 1000, so 1.0 at the 80-nit default.
    /// The tone map brings that to 1.0, and both FP16 paths draw the pointer
    /// at it. Read from the display by the session, which also re-reads it
    /// while streaming: the slider is live. Values below 1.0 keep the default.
    void setSdrWhite(float scRgbWhite);
    float sdrWhite() const { return m_SdrWhite; }

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

    /// Let go of the view of the last captured texture, and with it of the
    /// texture, before the capture hands its surfaces over anew
    /// (IWindowsCapture::retake). The next convert() makes another.
    void forgetSource();

    /// The texture the last convert() wrote — NV12 or AYUV per chroma().
    /// Owned here and reused every frame: allocating one per frame would be a
    /// VRAM allocation on the hot path for no reason.
    ID3D11Texture2D* output() const { return m_Output.Get(); }

    int outputWidth() const { return m_OutputWidth; }
    int outputHeight() const { return m_OutputHeight; }

    // ── MWSR's statistics, for the sharpness by zone (plan B §12 c) ─────────

    /// Take MWSR's statistics (MwsrStats) of every picture convert() reads
    /// from now on: the full pass, or with @p light the light one (plan B
    /// §12 f.4). Only where the grid was measured: an 8-bit SDR source,
    /// reduced to a smaller stream of its own shape. False, with the reason,
    /// anywhere else: nothing is taken, and the stream goes without a grid.
    bool enableMwsrStats(std::string& why, bool light = false);

    /// No statistics any more: the stream goes on without a grid.
    void disableMwsrStats()
    {
        m_MwsrStats.reset();
        m_MwsrTicket = 0;
    }

    /// Whether the statistics taken are the light pass's.
    bool mwsrStatsLight() const { return m_MwsrStats && m_MwsrStats->light(); }

    /// What the statistics pass costs this GPU, once per enableMwsrStats():
    /// see MwsrStats::takeCost.
    bool takeMwsrStatsCost(int64_t& costUs) { return m_MwsrStats && m_MwsrStats->takeCost(costUs); }

    /// The cells of the picture the last convert() read, if the GPU has
    /// finished them: MwsrStats::kFloats into @p cells. Never waits.
    bool readMwsrCells(float* cells);

    /// The upscale the client makes of the stream, as the zones' decision
    /// takes it: the source's width over the output's.
    float mwsrScale() const
    {
        return m_OutputWidth > 0 ? static_cast<float>(m_SourceWidth) / m_OutputWidth : 0.0f;
    }

    /// Whether the picture is reduced in linear light (the resample pass),
    /// rather than by the conversion's bilinear fetch. Changes with
    /// dropResample().
    bool reducesInLinearLight() const { return m_Filter != ScaleFilter::Bilinear; }

private:
    bool createShaders(std::string& error);
    bool createOutput(std::string& error);
    /// The resample pass: its two shaders, the half-scaled intermediate and
    /// the output-sized picture the conversion reads. Only when scaling.
    bool createScaler(std::string& error);

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

    /// The resample pass, present only when m_Filter is not Bilinear.
    /// Horizontal into m_ScaledMid (output width, source height, FP16 linear),
    /// vertical into m_Scaled (output size; sRGB-encoded 8-bit on the 8-bit
    /// path — an _SRGB target encodes the linear result for free — FP16 on
    /// the scRGB paths). The conversion then samples m_ScaledView where it
    /// sampled the capture.
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ScaleHShader;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> m_ScaleVShader;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_ScaledMid;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_ScaledMidTarget;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ScaledMidView;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Scaled;
    Microsoft::WRL::ComPtr<ID3D11RenderTargetView> m_ScaledTarget;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> m_ScaledView;
    ScaleFilter m_Filter = ScaleFilter::Bilinear;
    bool m_Letterboxed = false;

    /// GPU timestamps around the resample pass, on its first few dozen frames.
    PassTimer m_ResampleTimer;
    /// MWSR's statistics pass, once enableMwsrStats() took it, and the ticket
    /// of the picture the last convert() read (0: none).
    std::unique_ptr<MwsrStats> m_MwsrStats;
    uint64_t m_MwsrTicket = 0;
    /// Where the picture lands in m_Scaled: the whole of it, or the fitted
    /// rectangle between the bars.
    float m_PictureX = 0.0f;
    float m_PictureY = 0.0f;
    float m_PictureWidth = 0.0f;
    float m_PictureHeight = 0.0f;

    Chroma m_Chroma = Chroma::C420;
    bool m_Hdr = false;
    /// FP16 in, 8-bit BT.709 out: the shaders were compiled with the tone map.
    bool m_ToneMap = false;
    float m_SdrWhite = 1.0f;
    DXGI_FORMAT m_SourceFormat = DXGI_FORMAT_UNKNOWN;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    int m_OutputWidth = 0;
    int m_OutputHeight = 0;
};

} // namespace mw::native::convert
