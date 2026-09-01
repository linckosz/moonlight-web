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

#include "ColorConvert.h"

#include "../../core/Log.h"

#include <d3dcompiler.h>

using Microsoft::WRL::ComPtr;

namespace mw::native::convert {
namespace {

// ── The conversion, in HLSL ─────────────────────────────────────────────────
//
// BT.709, limited ("TV") range — the same colour space the existing pipeline
// already negotiates for SDR streams, so the browser's decoder needs no change
// and a native stream looks identical to a Sunshine one on the same screen.
//
// The vertex shader builds a full-screen triangle from SV_VertexID alone: no
// vertex buffer, no input layout, nothing to bind. A triangle rather than a
// quad because a quad's diagonal makes the GPU shade the seam twice.
constexpr char kShaderSource[] = R"HLSL(
Texture2D<float4> Source       : register(t0);
Texture2D<float4> CursorPixels : register(t1);
Texture2D<float>  CursorInvert : register(t2);
SamplerState      Linear       : register(s0);
SamplerState      Nearest      : register(s1);

// xy: the cursor's top-left in source UV. zw: its size in source UV.
// Enabled is 0 or 1 rather than a branch on the size, so a zero-size shape
// cannot divide by zero on its way to being invisible.
cbuffer Overlay : register(b0)
{
    float4 CursorRect;
    float  CursorEnabled;
    float3 OverlayPad;
};

struct VsOut
{
    float4 position : SV_POSITION;
    float2 uv       : TEXCOORD0;
};

VsOut VsMain(uint id : SV_VertexID)
{
    VsOut o;
    // Two of these three vertices fall outside the viewport; the clip does the
    // rest. Cheaper than a quad and needs no buffer.
    o.uv = float2((id << 1) & 2, id & 2);
    o.position = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}

// The desktop with the mouse pointer drawn on it.
//
// Windows composites the cursor at scan-out, so the duplicated frame never
// contains it and we have to put it back. Doing that HERE rather than in a
// second pass costs one texture fetch on the pixels the cursor covers and
// nothing at all anywhere else — a separate overlay pass would mean another
// full-frame render target and another round trip through VRAM.
//
// Colour is sampled linearly so a cursor scaled down with the picture keeps its
// antialiased edges; the invert mask is sampled nearest, because a half-inverted
// pixel is not a thing and interpolating the flag would fringe the I-beam.
float3 Scene(float2 uv)
{
    float3 rgb = Source.Sample(Linear, uv).rgb;
    if (CursorEnabled < 0.5) return rgb;

    float2 c = (uv - CursorRect.xy) / CursorRect.zw;
    if (c.x < 0.0 || c.y < 0.0 || c.x > 1.0 || c.y > 1.0) return rgb;

    // Monochrome cursors carry no colour of their own: they are defined as
    // inverting the background, which is what keeps a text I-beam visible over
    // black text and over white paper alike.
    if (CursorInvert.SampleLevel(Nearest, c, 0) > 0.5) return 1.0 - rgb;

    float4 cursor = CursorPixels.SampleLevel(Linear, c, 0);
    return lerp(rgb, cursor.rgb, cursor.a);
}

// BT.709 luma weights.
static const float3 kLuma = float3(0.2126, 0.7152, 0.0722);

// Limited range: luma lives in 16..235, chroma in 16..240, both scaled to the
// 0..1 a UNORM target stores.
static const float kLumaScale  = 219.0 / 255.0;
static const float kLumaBias   =  16.0 / 255.0;
static const float kChromaScale = 224.0 / 255.0;
static const float kChromaBias = 128.0 / 255.0;

float PsLuma(VsOut i) : SV_TARGET
{
    float3 rgb = Scene(i.uv);
    return dot(rgb, kLuma) * kLumaScale + kLumaBias;
}

// 4:4:4, written as packed AYUV in one draw.
//
// NVENC's AYUV is a 32-bit word with V in the lowest byte, then U, then Y, then
// A — so in memory the bytes are [V][U][Y][A], and an R8G8B8A8 render-target
// view lands them as R=V, G=U, B=Y, A=A. Getting that order wrong produces a
// picture with the colours swapped rather than an error, which is why it is
// spelled out here.
float4 PsPacked444(VsOut i) : SV_TARGET
{
    float3 rgb = Scene(i.uv);
    float  y   = dot(rgb, kLuma);

    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;

    return float4(cr * kChromaScale + kChromaBias,  // R <- V
                  cb * kChromaScale + kChromaBias,  // G <- U
                  y  * kLumaScale   + kLumaBias,    // B <- Y
                  1.0);
}

float2 PsChroma(VsOut i) : SV_TARGET
{
    // Sampling once at the chroma texel's centre lets the sampler average the
    // 2x2 luma neighbourhood for us, which is what 4:2:0 wants anyway.
    float3 rgb = Scene(i.uv);
    float  y   = dot(rgb, kLuma);

    // The BT.709 denominators: 2*(1-Kb) and 2*(1-Kr).
    float cb = (rgb.b - y) / 1.8556;
    float cr = (rgb.r - y) / 1.5748;
    return float2(cb, cr) * kChromaScale + kChromaBias;
}
)HLSL";

bool compile(const char* entryPoint, const char* target, ComPtr<ID3DBlob>& blob, std::string& error)
{
    ComPtr<ID3DBlob> errors;
    const HRESULT hr = ::D3DCompile(
        kShaderSource, sizeof(kShaderSource) - 1, "ColorConvert.hlsl", nullptr, nullptr, entryPoint,
        target, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, blob.GetAddressOf(), errors.GetAddressOf());
    if (SUCCEEDED(hr)) return true;

    error = std::string("could not compile ") + entryPoint;
    if (errors && errors->GetBufferPointer()) {
        error += ": ";
        error += static_cast<const char*>(errors->GetBufferPointer());
    }
    return false;
}

} // namespace

ColorConvert::~ColorConvert() = default;

bool ColorConvert::init(ID3D11Device* device, DXGI_FORMAT sourceFormat, int sourceWidth,
                        int sourceHeight, int outputWidth, int outputHeight, Chroma chroma,
                        std::string& error)
{
    if (!device) {
        error = "no D3D11 device";
        return false;
    }

    // HDR arrives as FP16 in scRGB and needs a PQ transfer plus a P010 target,
    // not this matrix. Refusing outright beats converting it as if it were SDR,
    // which would look washed out and wrong rather than obviously broken.
    if (sourceFormat != DXGI_FORMAT_B8G8R8A8_UNORM && sourceFormat != DXGI_FORMAT_R8G8B8A8_UNORM) {
        error = "this build converts 8-bit SDR frames only (HDR capture needs the P010 path)";
        return false;
    }

    m_Device = device;
    m_Device->GetImmediateContext(m_Context.ReleaseAndGetAddressOf());
    m_Chroma = chroma;
    m_SourceFormat = sourceFormat;
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;

    // NV12 chroma is half resolution in both axes, so an odd dimension has no
    // representation at all. Round down rather than up: growing the image would
    // sample outside the captured area.
    m_OutputWidth = (outputWidth > 0 ? outputWidth : sourceWidth) & ~1;
    m_OutputHeight = (outputHeight > 0 ? outputHeight : sourceHeight) & ~1;
    if (m_OutputWidth <= 0 || m_OutputHeight <= 0) {
        error = "output size is degenerate";
        return false;
    }

    if (!createShaders(error)) return false;
    if (!createOutput(error)) return false;

    log::info("[native] colour conversion: " + std::to_string(m_SourceWidth) + "x" +
              std::to_string(m_SourceHeight) + " BGRA -> " + std::to_string(m_OutputWidth) + "x" +
              std::to_string(m_OutputHeight) +
              (m_Chroma == Chroma::C444 ? " AYUV 4:4:4" : " NV12 4:2:0") + " (BT.709 limited)");
    return true;
}

bool ColorConvert::createShaders(std::string& error)
{
    ComPtr<ID3DBlob> vs, luma, chroma, packed;
    if (!compile("VsMain", "vs_5_0", vs, error)) return false;
    if (!compile("PsLuma", "ps_5_0", luma, error)) return false;
    if (!compile("PsChroma", "ps_5_0", chroma, error)) return false;
    if (!compile("PsPacked444", "ps_5_0", packed, error)) return false;

    if (FAILED(m_Device->CreateVertexShader(vs->GetBufferPointer(), vs->GetBufferSize(), nullptr,
                                            m_VertexShader.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreatePixelShader(luma->GetBufferPointer(), luma->GetBufferSize(), nullptr,
                                           m_LumaShader.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreatePixelShader(chroma->GetBufferPointer(), chroma->GetBufferSize(),
                                           nullptr, m_ChromaShader.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreatePixelShader(packed->GetBufferPointer(), packed->GetBufferSize(),
                                           nullptr, m_PackedShader.ReleaseAndGetAddressOf()))) {
        error = "could not create the conversion shaders";
        return false;
    }

    // Linear filtering is what makes a downscale and the 4:2:0 chroma average
    // come out of the same sample. CLAMP so an edge texel never wraps.
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(m_Device->CreateSamplerState(&sampler, m_Sampler.ReleaseAndGetAddressOf()))) {
        error = "could not create the sampler";
        return false;
    }

    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    if (FAILED(m_Device->CreateSamplerState(&sampler, m_NearestSampler.ReleaseAndGetAddressOf()))) {
        error = "could not create the point sampler";
        return false;
    }

    // Where to draw the cursor, rewritten every frame. DYNAMIC because that is
    // exactly the access pattern: written by the CPU once per frame, read by
    // the GPU immediately after.
    D3D11_BUFFER_DESC overlay = {};
    overlay.ByteWidth = sizeof(float) * 8; // float4 + float + float3 padding
    overlay.Usage = D3D11_USAGE_DYNAMIC;
    overlay.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    overlay.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(
            m_Device->CreateBuffer(&overlay, nullptr, m_OverlayBuffer.ReleaseAndGetAddressOf()))) {
        error = "could not create the cursor constant buffer";
        return false;
    }
    return true;
}

bool ColorConvert::updateCursorResources(const capture::CursorState& cursor, std::string& error)
{
    if (cursor.width <= 0 || cursor.height <= 0) return true;
    if (m_CursorPixels && m_CursorShapeVersion == cursor.shapeVersion) return true;

    const UINT width = static_cast<UINT>(cursor.width);
    const UINT height = static_cast<UINT>(cursor.height);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    // IMMUTABLE and recreated per shape rather than DYNAMIC and rewritten: a
    // shape change is rare (the pointer keeps one for thousands of frames) and
    // the sizes differ between shapes anyway, so there is nothing to reuse.
    D3D11_SUBRESOURCE_DATA seed = {};
    seed.pSysMem = cursor.pixels.data();
    seed.SysMemPitch = width * 4;

    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    if (FAILED(m_Device->CreateTexture2D(&desc, &seed, m_CursorPixels.ReleaseAndGetAddressOf()))) {
        error = "could not upload the cursor image";
        return false;
    }

    seed.pSysMem = cursor.invert.data();
    seed.SysMemPitch = width;
    desc.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(m_Device->CreateTexture2D(&desc, &seed, m_CursorInvert.ReleaseAndGetAddressOf()))) {
        error = "could not upload the cursor mask";
        return false;
    }

    if (FAILED(m_Device->CreateShaderResourceView(m_CursorPixels.Get(), nullptr,
                                                  m_CursorPixelsView.ReleaseAndGetAddressOf())) ||
        FAILED(m_Device->CreateShaderResourceView(m_CursorInvert.Get(), nullptr,
                                                  m_CursorInvertView.ReleaseAndGetAddressOf()))) {
        error = "could not view the cursor textures";
        return false;
    }

    m_CursorShapeVersion = cursor.shapeVersion;
    return true;
}

bool ColorConvert::createOutput(std::string& error)
{
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = static_cast<UINT>(m_OutputWidth);
    desc.Height = static_cast<UINT>(m_OutputHeight);
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = m_Chroma == Chroma::C444 ? DXGI_FORMAT_AYUV : DXGI_FORMAT_NV12;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    // RENDER_TARGET so the plane (or packed) views can be written;
    // SHADER_RESOURCE because the encoder registers it as an input surface.
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

    if (FAILED(m_Device->CreateTexture2D(&desc, nullptr, m_Output.ReleaseAndGetAddressOf()))) {
        error = m_Chroma == Chroma::C444
                    ? "this GPU cannot render into an AYUV texture (4:4:4 unavailable)"
                    : "this GPU cannot render into an NV12 texture";
        return false;
    }

    D3D11_RENDER_TARGET_VIEW_DESC view = {};
    view.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;

    if (m_Chroma == Chroma::C444) {
        // One packed plane: an R8G8B8A8 view writes all four bytes of each
        // AYUV word at once, so 4:4:4 needs a single draw where 4:2:0 needs
        // two. See PsPacked444 for the byte order.
        view.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        if (FAILED(m_Device->CreateRenderTargetView(m_Output.Get(), &view,
                                                    m_PackedTarget.ReleaseAndGetAddressOf()))) {
            error = "could not view the AYUV surface";
            return false;
        }
        return true;
    }

    // The plane is selected by the view's FORMAT, which is the whole trick:
    // R8 addresses luma, R8G8 addresses the interleaved chroma at half size.
    view.Format = DXGI_FORMAT_R8_UNORM;
    if (FAILED(m_Device->CreateRenderTargetView(m_Output.Get(), &view,
                                                m_LumaTarget.ReleaseAndGetAddressOf()))) {
        error = "could not view the NV12 luma plane";
        return false;
    }

    view.Format = DXGI_FORMAT_R8G8_UNORM;
    if (FAILED(m_Device->CreateRenderTargetView(m_Output.Get(), &view,
                                                m_ChromaTarget.ReleaseAndGetAddressOf()))) {
        error = "could not view the NV12 chroma plane";
        return false;
    }
    return true;
}

bool ColorConvert::convert(ID3D11Texture2D* source, const capture::CursorState& cursor,
                           const CursorDraw& draw, std::string& error)
{
    if (!source || !m_Context || !m_Output) {
        error = "colour conversion is not initialized";
        return false;
    }

    if (!updateCursorResources(cursor, error)) return false;

    // Desktop Duplication does not promise the same texture object twice, and a
    // view left pointing at the previous one would sample a frozen image
    // forever — a freeze indistinguishable from a network stall.
    if (source != m_SourceViewFor) {
        D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
        srv.Format = m_SourceFormat;
        srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srv.Texture2D.MipLevels = 1;
        if (FAILED(m_Device->CreateShaderResourceView(source, &srv,
                                                      m_SourceView.ReleaseAndGetAddressOf()))) {
            error = "could not view the captured frame";
            return false;
        }
        m_SourceViewFor = source;
    }

    // The cursor rectangle, expressed in SOURCE uv because that is the space the
    // shader samples in. Drawing in output space instead would misplace the
    // pointer by the scale factor on any stream that is not native resolution.
    const bool drawCursor = cursor.visible && m_CursorPixelsView && cursor.width > 0 &&
                            cursor.height > 0 && m_SourceWidth > 0 && m_SourceHeight > 0;
    {
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (FAILED(m_Context->Map(m_OverlayBuffer.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
            error = "could not update the cursor position";
            return false;
        }
        // Magnified around the hotspot, not around the top-left: the pointer has
        // to keep aiming at the same pixel while it grows, or a bigger cursor
        // would also be a cursor that clicks somewhere else.
        const float magnify = draw.magnify > 1.0f ? draw.magnify : 1.0f;
        const float width = static_cast<float>(cursor.width) * magnify;
        const float height = static_cast<float>(cursor.height) * magnify;
        const float left = static_cast<float>(cursor.x + draw.hotspotX) -
                           static_cast<float>(draw.hotspotX) * magnify;
        const float top = static_cast<float>(cursor.y + draw.hotspotY) -
                          static_cast<float>(draw.hotspotY) * magnify;

        float* p = static_cast<float*>(mapped.pData);
        p[0] = left / static_cast<float>(m_SourceWidth);
        p[1] = top / static_cast<float>(m_SourceHeight);
        p[2] = width / static_cast<float>(m_SourceWidth);
        p[3] = height / static_cast<float>(m_SourceHeight);
        p[4] = drawCursor ? 1.0f : 0.0f;
        p[5] = p[6] = p[7] = 0.0f;
        m_Context->Unmap(m_OverlayBuffer.Get(), 0);
    }

    ID3D11ShaderResourceView* views[] = {m_SourceView.Get(), m_CursorPixelsView.Get(),
                                         m_CursorInvertView.Get()};
    ID3D11SamplerState* samplers[] = {m_Sampler.Get(), m_NearestSampler.Get()};
    ID3D11Buffer* constants[] = {m_OverlayBuffer.Get()};

    m_Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    m_Context->IASetInputLayout(nullptr);
    m_Context->VSSetShader(m_VertexShader.Get(), nullptr, 0);
    m_Context->PSSetShaderResources(0, 3, views);
    m_Context->PSSetSamplers(0, 2, samplers);
    m_Context->PSSetConstantBuffers(0, 1, constants);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(m_OutputWidth);
    viewport.Height = static_cast<float>(m_OutputHeight);
    viewport.MaxDepth = 1.0f;

    if (m_Chroma == Chroma::C444) {
        // One draw: luma and both chroma components go out together at full
        // resolution.
        ID3D11RenderTargetView* packedTarget[] = {m_PackedTarget.Get()};
        m_Context->OMSetRenderTargets(1, packedTarget, nullptr);
        m_Context->RSSetViewports(1, &viewport);
        m_Context->PSSetShader(m_PackedShader.Get(), nullptr, 0);
        m_Context->Draw(3, 0);
    } else {
        // Luma: full resolution.
        ID3D11RenderTargetView* lumaTarget[] = {m_LumaTarget.Get()};
        m_Context->OMSetRenderTargets(1, lumaTarget, nullptr);
        m_Context->RSSetViewports(1, &viewport);
        m_Context->PSSetShader(m_LumaShader.Get(), nullptr, 0);
        m_Context->Draw(3, 0);

        // Chroma: half resolution, which is what makes this 4:2:0.
        viewport.Width = static_cast<float>(m_OutputWidth / 2);
        viewport.Height = static_cast<float>(m_OutputHeight / 2);

        ID3D11RenderTargetView* chromaTarget[] = {m_ChromaTarget.Get()};
        m_Context->OMSetRenderTargets(1, chromaTarget, nullptr);
        m_Context->RSSetViewports(1, &viewport);
        m_Context->PSSetShader(m_ChromaShader.Get(), nullptr, 0);
        m_Context->Draw(3, 0);
    }

    // Unbind the source before returning: capture is about to release the
    // texture, and leaving it bound to the pipeline would keep it alive and
    // trip the next AcquireNextFrame.
    ID3D11ShaderResourceView* none[] = {nullptr, nullptr, nullptr};
    m_Context->PSSetShaderResources(0, 3, none);
    m_Context->OMSetRenderTargets(0, nullptr, nullptr);
    return true;
}

} // namespace mw::native::convert
