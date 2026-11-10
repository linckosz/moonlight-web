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

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "ColorConvert12.h"

#include "../../../core/Log.h"
#include "../ColorConvert.h"

#include <climits>
#include <cstring>

using Microsoft::WRL::ComPtr;

namespace mw::native::convert {
namespace {

// Frames of descriptor tables in the ring. The pipeline waits for each frame
// before it records the next, so two would do; four leaves room.
constexpr UINT kRing = 4;
constexpr UINT kTablesPerFrame = 3;
constexpr UINT kTableSize = 3;

enum Rtv : UINT
{
    RtvLuma,
    RtvChroma,
    RtvScaledMid,
    RtvScaled,
    /// The second output's planes (addOutput).
    RtvLuma1,
    RtvChroma1,
    RtvCount,
};

Rtv lumaRtv(int output)
{
    return output == 1 ? RtvLuma1 : RtvLuma;
}

Rtv chromaRtv(int output)
{
    return output == 1 ? RtvChroma1 : RtvChroma;
}

D3D12_RESOURCE_DESC textureDesc(int width, int height, DXGI_FORMAT format,
                                D3D12_RESOURCE_FLAGS flags)
{
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = static_cast<UINT64>(width);
    desc.Height = static_cast<UINT>(height);
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = flags;
    return desc;
}

HRESULT committed(ID3D12Device* device, D3D12_HEAP_TYPE type, const D3D12_RESOURCE_DESC& desc,
                  D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& out)
{
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = type;
    return device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                           IID_PPV_ARGS(out.ReleaseAndGetAddressOf()));
}

D3D12_STATIC_SAMPLER_DESC staticSampler(UINT reg, D3D12_FILTER filter)
{
    // What ColorConvert's samplers are: CLAMP so an edge texel never wraps,
    // every mip level allowed (there is one).
    D3D12_STATIC_SAMPLER_DESC sampler = {};
    sampler.Filter = filter;
    sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
    sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_TRANSPARENT_BLACK;
    sampler.MinLOD = 0.0f;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    sampler.ShaderRegister = reg;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    return sampler;
}

void viewport(ID3D12GraphicsCommandList* list, float x, float y, float width, float height,
              int targetWidth, int targetHeight)
{
    D3D12_VIEWPORT vp = {};
    vp.TopLeftX = x;
    vp.TopLeftY = y;
    vp.Width = width;
    vp.Height = height;
    vp.MaxDepth = 1.0f;
    list->RSSetViewports(1, &vp);
    // D3D12 always scissors, and an unset rectangle is empty: the whole
    // target, the viewport does the clipping as it does in D3D11.
    const D3D12_RECT scissor = {0, 0, targetWidth, targetHeight};
    list->RSSetScissorRects(1, &scissor);
}

} // namespace

ColorConvert12::~ColorConvert12() = default;

void ColorConvert12::release()
{
    m_RootSignature.Reset();
    m_LumaPso.Reset();
    m_ChromaPso.Reset();
    m_FillLumaPso.Reset();
    m_FillChromaPso.Reset();
    m_ScaleHPso.Reset();
    m_ScaleVPso.Reset();
    m_VertexShader.Reset();
    m_SrvHeap.Reset();
    m_SrvStride = m_SrvNext = 0;
    m_RtvHeap.Reset();
    m_RtvStride = 0;
    for (int i = 0; i < kMaxOutputs; ++i) {
        m_Output[i].Reset();
        m_OutputState[i] = D3D12_RESOURCE_STATE_COMMON;
        m_OutputCleared[i] = false;
    }
    m_Outputs = 0;
    m_Target = 0;
    m_ScaledMid.Reset();
    m_Scaled.Reset();
    m_ScaledMidState = m_ScaledState = D3D12_RESOURCE_STATE_COMMON;
    releaseHeld();
    m_CursorPixels.Reset();
    m_CursorInvert.Reset();
    m_CursorUpload.Reset();
    m_CursorShapeVersion = 0;
    m_Timer.reset();
    m_TimingSlot = -1;
    m_ResampleCost.reset();
    m_ResampleCostTaken = false;
    m_Geometry = ConvertGeometry{};
    m_Hdr = m_ToneMap = false;
    m_SourceFormat = DXGI_FORMAT_UNKNOWN;
    m_SourceWidth = m_SourceHeight = 0;
    m_SourceTurn = 0;
    m_CodedWidth = m_CodedHeight = 0;
    m_Device.Reset();
}

bool ColorConvert12::init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT sourceFormat,
                          int sourceWidth, int sourceHeight, int outputWidth, int outputHeight,
                          int codedWidth, int codedHeight, bool hdr, ScaleFilter filter,
                          std::string& error, int sourceTurn)
{
    release();
    if (!device) {
        error = "no D3D12 device";
        return false;
    }
    if (!ColorConvert::supportsSource(sourceFormat)) {
        error = "no colour conversion for source format " +
                std::to_string(static_cast<int>(sourceFormat));
        return false;
    }
    // The same refusal as ColorConvert, for the same reason: see there.
    if (hdr && !ColorConvert::isHdrSource(sourceFormat)) {
        error = "HDR was asked for but the display delivers 8-bit SDR frames";
        return false;
    }
    if (!convertGeometry(sourceWidth, sourceHeight, outputWidth, outputHeight, filter, m_Geometry,
                         error))
        return false;

    m_CodedWidth = codedWidth > 0 ? codedWidth : m_Geometry.outputWidth;
    m_CodedHeight = codedHeight > 0 ? codedHeight : m_Geometry.outputHeight;
    if (m_CodedWidth < m_Geometry.outputWidth || m_CodedHeight < m_Geometry.outputHeight ||
        (m_CodedWidth & 1) || (m_CodedHeight & 1)) {
        error = "the coded size " + std::to_string(m_CodedWidth) + "x" +
                std::to_string(m_CodedHeight) + " cannot hold a " +
                std::to_string(m_Geometry.outputWidth) + "x" +
                std::to_string(m_Geometry.outputHeight) + " picture";
        return false;
    }

    m_Device = device;
    m_Hdr = hdr;
    m_ToneMap = !hdr && ColorConvert::isHdrSource(sourceFormat);
    m_SourceFormat = sourceFormat;
    m_SourceWidth = sourceWidth;
    m_SourceHeight = sourceHeight;
    m_SourceTurn = sourceTurn & 3;

    if (!createPipeline(error) || !createOutput(0, error)) return false;
    if (m_Geometry.filter != ScaleFilter::Bilinear && !createScaler(error)) return false;

    // What the resample pass costs is timed as on D3D11, on the queue the
    // lists run on. No timestamps, no measurement: the pass stays.
    std::string timing;
    if (m_Geometry.filter != ScaleFilter::Bilinear &&
        !m_Timer.init(device, queue, D3D12_COMMAND_LIST_TYPE_DIRECT, timing))
        log::info("[native] colour conversion (D3D12): the resample pass is not timed (" + timing +
                  ")");

    log::info(
        "[native] colour conversion (D3D12): " + std::to_string(m_SourceWidth) + "x" +
        std::to_string(m_SourceHeight) +
        (m_Hdr       ? " FP16 scRGB -> "
         : m_ToneMap ? " FP16 scRGB, tone-mapped -> "
                     : " BGRA -> ") +
        std::to_string(m_Geometry.outputWidth) + "x" + std::to_string(m_Geometry.outputHeight) +
        (m_Hdr ? " P010 4:2:0 (BT.2020 PQ, limited)" : " NV12 4:2:0 (BT.709 limited)") +
        (!m_Geometry.scaling                          ? ", 1:1"
         : m_Geometry.filter == ScaleFilter::Bilinear ? ", scaled bilinear in the pass"
                                                      : ", scaled Lanczos-2 (linear light)") +
        (m_Geometry.letterboxed
             ? ", letterboxed to " + std::to_string(static_cast<int>(m_Geometry.pictureWidth)) +
                   "x" + std::to_string(static_cast<int>(m_Geometry.pictureHeight))
             : "") +
        (m_CodedWidth != m_Geometry.outputWidth || m_CodedHeight != m_Geometry.outputHeight
             ? ", coded " + std::to_string(m_CodedWidth) + "x" + std::to_string(m_CodedHeight)
             : "") +
        (m_SourceTurn != 0
             ? ", stood upright from " + std::to_string(m_SourceTurn * 90) + "° (the " +
                   (m_Geometry.filter != ScaleFilter::Bilinear ? "resample" : "conversion") +
                   " reads it turned)"
             : ""));
    return true;
}

bool ColorConvert12::createPso(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT target,
                               ComPtr<ID3D12PipelineState>& pso, std::string& error)
{
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc = {};
    desc.pRootSignature = m_RootSignature.Get();
    desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    desc.BlendState.RenderTarget[0].SrcBlend = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlend = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOp = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].SrcBlendAlpha = D3D12_BLEND_ONE;
    desc.BlendState.RenderTarget[0].DestBlendAlpha = D3D12_BLEND_ZERO;
    desc.BlendState.RenderTarget[0].BlendOpAlpha = D3D12_BLEND_OP_ADD;
    desc.BlendState.RenderTarget[0].LogicOp = D3D12_LOGIC_OP_NOOP;
    desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    // No culling: the full-screen triangle is the only primitive there is.
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.DepthStencilState.DepthEnable = FALSE;
    desc.DepthStencilState.StencilEnable = FALSE;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1;
    desc.RTVFormats[0] = target;
    desc.SampleDesc.Count = 1;
    const HRESULT hr =
        m_Device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(pso.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        error = "could not create a D3D12 conversion pipeline state for format " +
                std::to_string(static_cast<int>(target)) + " (" + d3d12::hresultText(hr) + ")";
        return false;
    }
    return true;
}

bool ColorConvert12::createPipeline(std::string& error)
{
    // b0: the Overlay cbuffer as eight root constants — it changes every
    // frame, and a root constant needs no buffer to rename. t0-t2 in one
    // table. s0/s1 static: they never change.
    D3D12_DESCRIPTOR_RANGE range = {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = kTableSize;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0;
    params[0].Constants.Num32BitValues = 8;
    params[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[1].DescriptorTable.NumDescriptorRanges = 1;
    params[1].DescriptorTable.pDescriptorRanges = &range;
    params[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_STATIC_SAMPLER_DESC samplers[] = {
        staticSampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR),
        staticSampler(1, D3D12_FILTER_MIN_MAG_MIP_POINT),
    };
    D3D12_ROOT_SIGNATURE_DESC root = {};
    root.NumParameters = 2;
    root.pParameters = params;
    root.NumStaticSamplers = 2;
    root.pStaticSamplers = samplers;
    root.Flags = D3D12_ROOT_SIGNATURE_FLAG_DENY_VERTEX_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_HULL_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_DOMAIN_SHADER_ROOT_ACCESS |
                 D3D12_ROOT_SIGNATURE_FLAG_DENY_GEOMETRY_SHADER_ROOT_ACCESS;
    ComPtr<ID3DBlob> serialized, errors;
    HRESULT hr =
        ::D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, &errors);
    if (SUCCEEDED(hr))
        hr = m_Device->CreateRootSignature(0, serialized->GetBufferPointer(),
                                           serialized->GetBufferSize(),
                                           IID_PPV_ARGS(&m_RootSignature));
    if (FAILED(hr)) {
        error = "could not create the conversion root signature (" + d3d12::hresultText(hr) + ")";
        if (errors && errors->GetBufferPointer())
            error += ": " + std::string(static_cast<const char*>(errors->GetBufferPointer()));
        return false;
    }

    // The bytecode ColorConvert draws: the pair this session needs, with the
    // source the tone-map flag says.
    ComPtr<ID3DBlob> luma, chroma, fill;
    if (!compileConvertShader("VsMain", "vs_5_0", m_ToneMap, m_VertexShader, error) ||
        !compileConvertShader(m_Hdr ? "PsLumaHdr" : "PsLuma", "ps_5_0", m_ToneMap, luma, error) ||
        !compileConvertShader(m_Hdr ? "PsChromaHdr" : "PsChroma", "ps_5_0", m_ToneMap, chroma,
                              error) ||
        !compileConvertShader("PsFill", "ps_5_0", m_ToneMap, fill, error))
        return false;
    const DXGI_FORMAT lumaFormat = m_Hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    const DXGI_FORMAT chromaFormat = m_Hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    if (!createPso(m_VertexShader.Get(), luma.Get(), lumaFormat, m_LumaPso, error) ||
        !createPso(m_VertexShader.Get(), chroma.Get(), chromaFormat, m_ChromaPso, error) ||
        !createPso(m_VertexShader.Get(), fill.Get(), lumaFormat, m_FillLumaPso, error) ||
        !createPso(m_VertexShader.Get(), fill.Get(), chromaFormat, m_FillChromaPso, error))
        return false;

    D3D12_DESCRIPTOR_HEAP_DESC heap = {};
    heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    heap.NumDescriptors = kRing * kTablesPerFrame * kTableSize;
    heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    hr = m_Device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_SrvHeap));
    if (SUCCEEDED(hr)) {
        heap.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap.NumDescriptors = RtvCount;
        heap.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = m_Device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&m_RtvHeap));
    }
    if (FAILED(hr)) {
        error = "could not create the conversion descriptor heaps (" + d3d12::hresultText(hr) + ")";
        return false;
    }
    m_SrvStride =
        m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    m_RtvStride = m_Device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    return true;
}

bool ColorConvert12::createOutput(int index, std::string& error)
{
    const DXGI_FORMAT format = m_Hdr ? DXGI_FORMAT_P010 : DXGI_FORMAT_NV12;
    const HRESULT hr = committed(
        m_Device.Get(), D3D12_HEAP_TYPE_DEFAULT,
        textureDesc(m_CodedWidth, m_CodedHeight, format, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
        D3D12_RESOURCE_STATE_COMMON, m_Output[index]);
    if (FAILED(hr)) {
        error = std::string("this GPU cannot render into a D3D12 ") + (m_Hdr ? "P010" : "NV12") +
                " texture (" + d3d12::hresultText(hr) + ")";
        return false;
    }
    m_OutputState[index] = D3D12_RESOURCE_STATE_COMMON;
    m_OutputCleared[index] = false;

    // A view per plane, told apart by PlaneSlice — D3D11 told them apart by
    // the view's format alone.
    D3D12_RENDER_TARGET_VIEW_DESC view = {};
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    const D3D12_CPU_DESCRIPTOR_HANDLE base = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = base;
    rtv.ptr += static_cast<SIZE_T>(lumaRtv(index)) * m_RtvStride;
    view.Format = m_Hdr ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R8_UNORM;
    view.Texture2D.PlaneSlice = 0;
    m_Device->CreateRenderTargetView(m_Output[index].Get(), &view, rtv);
    rtv = base;
    rtv.ptr += static_cast<SIZE_T>(chromaRtv(index)) * m_RtvStride;
    view.Format = m_Hdr ? DXGI_FORMAT_R16G16_UNORM : DXGI_FORMAT_R8G8_UNORM;
    view.Texture2D.PlaneSlice = 1;
    m_Device->CreateRenderTargetView(m_Output[index].Get(), &view, rtv);
    m_Outputs = index + 1 > m_Outputs ? index + 1 : m_Outputs;
    return true;
}

bool ColorConvert12::addOutput(std::string& error)
{
    if (m_Outputs == 0) {
        error = "colour conversion is not initialized";
        return false;
    }
    return m_Outputs >= kMaxOutputs || createOutput(m_Outputs, error);
}

bool ColorConvert12::createScaler(std::string& error)
{
    const int pictureWidth = static_cast<int>(m_Geometry.pictureWidth);
    const int pictureHeight = static_cast<int>(m_Geometry.pictureHeight);
    // The 8-bit desktop is decoded to light on the way in; the FP16 paths are
    // light already. As ColorConvert::createScaler.
    const bool decode = !m_Hdr && !m_ToneMap;

    ComPtr<ID3DBlob> h, v;
    // The horizontal pass reads the capture, and stands it upright as it does.
    if (!compileScaleShader(true, decode, m_SourceWidth, pictureWidth, m_SourceHeight, h, error,
                            m_SourceTurn) ||
        !compileScaleShader(false, false, m_SourceHeight, pictureHeight, pictureWidth, v, error))
        return false;
    if (!createPso(m_VertexShader.Get(), h.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, m_ScaleHPso,
                   error) ||
        !createPso(m_VertexShader.Get(), v.Get(),
                   decode ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R16G16B16A16_FLOAT,
                   m_ScaleVPso, error))
        return false;

    // Picture-wide, source-high, linear light in FP16; then the output-sized
    // picture, TYPELESS on the 8-bit path so it is written through an _SRGB
    // view and read through a UNORM one.
    HRESULT hr = committed(m_Device.Get(), D3D12_HEAP_TYPE_DEFAULT,
                           textureDesc(pictureWidth, m_SourceHeight, DXGI_FORMAT_R16G16B16A16_FLOAT,
                                       D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
                           D3D12_RESOURCE_STATE_COMMON, m_ScaledMid);
    if (FAILED(hr)) {
        error = "could not create the resample intermediate (" + d3d12::hresultText(hr) + ")";
        return false;
    }
    hr = committed(
        m_Device.Get(), D3D12_HEAP_TYPE_DEFAULT,
        textureDesc(m_Geometry.outputWidth, m_Geometry.outputHeight,
                    decode ? DXGI_FORMAT_R8G8B8A8_TYPELESS : DXGI_FORMAT_R16G16B16A16_FLOAT,
                    D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET),
        D3D12_RESOURCE_STATE_COMMON, m_Scaled);
    if (FAILED(hr)) {
        error = "could not create the scaled picture (" + d3d12::hresultText(hr) + ")";
        return false;
    }
    m_ScaledMidState = m_ScaledState = D3D12_RESOURCE_STATE_COMMON;

    D3D12_RENDER_TARGET_VIEW_DESC view = {};
    view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    D3D12_CPU_DESCRIPTOR_HANDLE rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(RtvScaledMid) * m_RtvStride;
    view.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_Device->CreateRenderTargetView(m_ScaledMid.Get(), &view, rtv);
    rtv = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    rtv.ptr += static_cast<SIZE_T>(RtvScaled) * m_RtvStride;
    view.Format = decode ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R16G16B16A16_FLOAT;
    m_Device->CreateRenderTargetView(m_Scaled.Get(), &view, rtv);
    return true;
}

void ColorConvert12::setSdrWhite(float scRgbWhite)
{
    // As ColorConvert: below 1.0 does not exist, and 0 would divide by zero.
    m_SdrWhite = scRgbWhite >= 1.0f ? scRgbWhite : 1.0f;
}

void ColorConvert12::transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                                D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next)
{
    if (state == next) return;
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = state;
    barrier.Transition.StateAfter = next;
    list->ResourceBarrier(1, &barrier);
    state = next;
}

D3D12_GPU_DESCRIPTOR_HANDLE ColorConvert12::table(ID3D12Resource* source, DXGI_FORMAT format,
                                                  bool cursor)
{
    const UINT index = m_SrvNext;
    m_SrvNext = (m_SrvNext + 1) % (kRing * kTablesPerFrame);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = m_SrvHeap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += static_cast<SIZE_T>(index) * kTableSize * m_SrvStride;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = m_SrvHeap->GetGPUDescriptorHandleForHeapStart();
    gpu.ptr += static_cast<UINT64>(index) * kTableSize * m_SrvStride;

    D3D12_SHADER_RESOURCE_VIEW_DESC view = {};
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.Texture2D.MipLevels = 1;
    view.Format = format;
    m_Device->CreateShaderResourceView(source, &view, cpu);

    // A pointer that is not there is a null descriptor, typed as the shader
    // declares it: the shader does not sample it (CursorEnabled is 0), and a
    // null view reads zero if it did.
    cpu.ptr += m_SrvStride;
    view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    m_Device->CreateShaderResourceView(cursor ? m_CursorPixels.Get() : nullptr, &view, cpu);
    cpu.ptr += m_SrvStride;
    view.Format = DXGI_FORMAT_R8_UNORM;
    m_Device->CreateShaderResourceView(cursor ? m_CursorInvert.Get() : nullptr, &view, cpu);
    return gpu;
}

bool ColorConvert12::updateCursorResources(ID3D12GraphicsCommandList* list,
                                           const capture::CursorState& cursor, std::string& error)
{
    if (cursor.width <= 0 || cursor.height <= 0) return true;
    if (m_CursorPixels && m_CursorShapeVersion == cursor.shapeVersion) return true;

    // Recreated per shape, as on D3D11: a shape change is rare and the sizes
    // differ between shapes, so there is nothing to reuse. The pixels go up
    // through an upload buffer, copied on the list that first draws them.
    const D3D12_RESOURCE_DESC pixelsDesc = textureDesc(
        cursor.width, cursor.height, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    const D3D12_RESOURCE_DESC invertDesc =
        textureDesc(cursor.width, cursor.height, DXGI_FORMAT_R8_UNORM, D3D12_RESOURCE_FLAG_NONE);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT pixelsFoot = {}, invertFoot = {};
    UINT64 pixelsBytes = 0, invertBytes = 0;
    m_Device->GetCopyableFootprints(&pixelsDesc, 0, 1, 0, &pixelsFoot, nullptr, nullptr,
                                    &pixelsBytes);
    const UINT64 invertOffset = (pixelsBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                                ~static_cast<UINT64>(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    m_Device->GetCopyableFootprints(&invertDesc, 0, 1, invertOffset, &invertFoot, nullptr, nullptr,
                                    &invertBytes);

    D3D12_RESOURCE_DESC buffer = {};
    buffer.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer.Width = invertOffset + invertBytes;
    buffer.Height = 1;
    buffer.DepthOrArraySize = 1;
    buffer.MipLevels = 1;
    buffer.SampleDesc.Count = 1;
    buffer.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload, pixels, invert;
    HRESULT hr = committed(m_Device.Get(), D3D12_HEAP_TYPE_UPLOAD, buffer,
                           D3D12_RESOURCE_STATE_GENERIC_READ, upload);
    if (SUCCEEDED(hr))
        hr = committed(m_Device.Get(), D3D12_HEAP_TYPE_DEFAULT, pixelsDesc,
                       D3D12_RESOURCE_STATE_COPY_DEST, pixels);
    if (SUCCEEDED(hr))
        hr = committed(m_Device.Get(), D3D12_HEAP_TYPE_DEFAULT, invertDesc,
                       D3D12_RESOURCE_STATE_COPY_DEST, invert);
    uint8_t* mapped = nullptr;
    const D3D12_RANGE nothing = {0, 0};
    if (SUCCEEDED(hr)) hr = upload->Map(0, &nothing, reinterpret_cast<void**>(&mapped));
    if (FAILED(hr)) {
        error = "could not upload the cursor image (" + d3d12::hresultText(hr) + ")";
        return false;
    }
    const size_t width = static_cast<size_t>(cursor.width);
    for (int y = 0; y < cursor.height; ++y) {
        std::memcpy(mapped + pixelsFoot.Offset +
                        y * static_cast<size_t>(pixelsFoot.Footprint.RowPitch),
                    cursor.pixels.data() + y * width * 4, width * 4);
        std::memcpy(mapped + invertFoot.Offset +
                        y * static_cast<size_t>(invertFoot.Footprint.RowPitch),
                    cursor.invert.data() + y * width, width);
    }
    upload->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION from = {}, to = {};
    from.pResource = upload.Get();
    from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.SubresourceIndex = 0;
    from.PlacedFootprint = pixelsFoot;
    to.pResource = pixels.Get();
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    from.PlacedFootprint = invertFoot;
    to.pResource = invert.Get();
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    D3D12_RESOURCE_STATES pixelsState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_STATES invertState = D3D12_RESOURCE_STATE_COPY_DEST;
    transition(list, pixels.Get(), pixelsState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    transition(list, invert.Get(), invertState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    m_CursorUpload = upload;
    m_CursorPixels = pixels;
    m_CursorInvert = invert;
    m_CursorShapeVersion = cursor.shapeVersion;
    return true;
}

bool ColorConvert12::recordConvert(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                                   const capture::CursorState& cursor, const CursorDraw& draw,
                                   std::string& error)
{
    ID3D12Resource* output = m_Output[m_Target].Get();
    if (!list || !source || !output) {
        error = "colour conversion is not initialized";
        return false;
    }
    if (!updateCursorResources(list, cursor, error)) return false;

    ID3D12DescriptorHeap* heaps[] = {m_SrvHeap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(m_RootSignature.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D12_RESOURCE_STATES sourceState = D3D12_RESOURCE_STATE_COMMON;
    transition(list, source, sourceState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    const D3D12_CPU_DESCRIPTOR_HANDLE rtvBase = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
    const auto rtv = [&](Rtv which) {
        D3D12_CPU_DESCRIPTOR_HANDLE handle = rtvBase;
        handle.ptr += static_cast<SIZE_T>(which) * m_RtvStride;
        return handle;
    };

    // The resample pass, when there is one: the capture through the horizontal
    // filter into the intermediate, the intermediate through the vertical one
    // into the scaled picture; the conversion below reads THAT at 1:1. Bars,
    // if any, cleared to black first. As ColorConvert::convert.
    const bool resampled = m_Geometry.filter != ScaleFilter::Bilinear;
    if (resampled) {
        // A slot recorded but never read belonged to a list that did not
        // complete, or was not waited for: its stamps mean nothing.
        if (m_TimingSlot >= 0) {
            m_Timer.cancel(m_TimingSlot);
            m_TimingSlot = -1;
        }
        int slot = -1;
        if (m_Timer.ready() && m_ResampleCost.timeThisFrame(0)) slot = m_Timer.begin(list);

        transition(list, m_ScaledMid.Get(), m_ScaledMidState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE mid = rtv(RtvScaledMid);
        list->OMSetRenderTargets(1, &mid, FALSE, nullptr);
        viewport(list, 0.0f, 0.0f, m_Geometry.pictureWidth, static_cast<float>(m_SourceHeight),
                 static_cast<int>(m_Geometry.pictureWidth), m_SourceHeight);
        list->SetPipelineState(m_ScaleHPso.Get());
        list->SetGraphicsRootDescriptorTable(1, table(source, m_SourceFormat, false));
        list->DrawInstanced(3, 1, 0, 0);

        transition(list, m_ScaledMid.Get(), m_ScaledMidState,
                   D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        transition(list, m_Scaled.Get(), m_ScaledState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const D3D12_CPU_DESCRIPTOR_HANDLE scaled = rtv(RtvScaled);
        list->OMSetRenderTargets(1, &scaled, FALSE, nullptr);
        if (m_Geometry.letterboxed) {
            const float black[4] = {0.0f, 0.0f, 0.0f, 1.0f};
            list->ClearRenderTargetView(scaled, black, 0, nullptr);
        }
        viewport(list, m_Geometry.pictureX, m_Geometry.pictureY, m_Geometry.pictureWidth,
                 m_Geometry.pictureHeight, m_Geometry.outputWidth, m_Geometry.outputHeight);
        list->SetPipelineState(m_ScaleVPso.Get());
        list->SetGraphicsRootDescriptorTable(
            1, table(m_ScaledMid.Get(), DXGI_FORMAT_R16G16B16A16_FLOAT, false));
        list->DrawInstanced(3, 1, 0, 0);
        transition(list, m_Scaled.Get(), m_ScaledState, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

        if (slot >= 0) {
            m_Timer.end(list, slot);
            m_TimingSlot = slot;
        }
    }

    transition(list, output, m_OutputState[m_Target], D3D12_RESOURCE_STATE_RENDER_TARGET);
    // The band outside the picture, once: nothing ever draws there again.
    // Ahead of the pointer's constants, which the black overwrites.
    if (!m_OutputCleared[m_Target]) {
        recordBlack(list);
        m_OutputCleared[m_Target] = true;
    }

    // Where the pointer goes: the same constants ColorConvert writes.
    const OverlayConstants overlay =
        overlayConstants(cursor, draw, m_CursorPixels != nullptr, m_SourceWidth, m_SourceHeight,
                         resampled, m_Geometry, m_SdrWhite, m_SourceTurn);
    list->SetGraphicsRoot32BitConstants(0, 8, overlay.values, 0);
    const bool decodeScaled = !m_Hdr && !m_ToneMap;
    list->SetGraphicsRootDescriptorTable(
        1, resampled
               ? table(m_Scaled.Get(),
                       decodeScaled ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_R16G16B16A16_FLOAT,
                       m_CursorPixels != nullptr)
               : table(source, m_SourceFormat, m_CursorPixels != nullptr));

    const D3D12_CPU_DESCRIPTOR_HANDLE luma = rtv(lumaRtv(m_Target));
    const D3D12_CPU_DESCRIPTOR_HANDLE chroma = rtv(chromaRtv(m_Target));

    // Luma at full resolution, chroma at half: 4:2:0.
    list->OMSetRenderTargets(1, &luma, FALSE, nullptr);
    viewport(list, 0.0f, 0.0f, static_cast<float>(m_Geometry.outputWidth),
             static_cast<float>(m_Geometry.outputHeight), m_CodedWidth, m_CodedHeight);
    list->SetPipelineState(m_LumaPso.Get());
    list->DrawInstanced(3, 1, 0, 0);

    list->OMSetRenderTargets(1, &chroma, FALSE, nullptr);
    viewport(list, 0.0f, 0.0f, static_cast<float>(m_Geometry.outputWidth / 2),
             static_cast<float>(m_Geometry.outputHeight / 2), m_CodedWidth / 2, m_CodedHeight / 2);
    list->SetPipelineState(m_ChromaPso.Get());
    list->DrawInstanced(3, 1, 0, 0);

    transition(list, output, m_OutputState[m_Target], D3D12_RESOURCE_STATE_COMMON);
    transition(list, source, sourceState, D3D12_RESOURCE_STATE_COMMON);
    return true;
}

bool ColorConvert12::recordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                                std::string& error)
{
    if (!list || !source || !m_Device) {
        error = "colour conversion is not initialized";
        return false;
    }
    const D3D12_RESOURCE_DESC wanted = source->GetDesc();
    if (m_Held) {
        const D3D12_RESOURCE_DESC have = m_Held->GetDesc();
        if (have.Width != wanted.Width || have.Height != wanted.Height ||
            have.Format != wanted.Format)
            m_Held.Reset();
    }
    if (!m_Held) {
        const HRESULT hr =
            committed(m_Device.Get(), D3D12_HEAP_TYPE_DEFAULT,
                      textureDesc(static_cast<int>(wanted.Width), static_cast<int>(wanted.Height),
                                  wanted.Format, D3D12_RESOURCE_FLAG_NONE),
                      D3D12_RESOURCE_STATE_COMMON, m_Held);
        if (FAILED(hr)) {
            error = "could not keep a copy of the desktop (" + d3d12::hresultText(hr) + ")";
            return false;
        }
        m_HeldState = D3D12_RESOURCE_STATE_COMMON;
    }
    D3D12_RESOURCE_STATES sourceState = D3D12_RESOURCE_STATE_COMMON;
    transition(list, source, sourceState, D3D12_RESOURCE_STATE_COPY_SOURCE);
    transition(list, m_Held.Get(), m_HeldState, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(m_Held.Get(), source);
    transition(list, source, sourceState, D3D12_RESOURCE_STATE_COMMON);
    transition(list, m_Held.Get(), m_HeldState, D3D12_RESOURCE_STATE_COMMON);
    return true;
}

void ColorConvert12::releaseHeld()
{
    m_Held.Reset();
    m_HeldState = D3D12_RESOURCE_STATE_COMMON;
}

ComPtr<ID3D12Resource> ColorConvert12::takeHeld()
{
    ComPtr<ID3D12Resource> held = std::move(m_Held);
    m_HeldState = D3D12_RESOURCE_STATE_COMMON;
    return held;
}

void ColorConvert12::setHeld(ComPtr<ID3D12Resource> held)
{
    // Left in COMMON by every list that touched it: see recordCopy().
    m_Held = std::move(held);
    m_HeldState = D3D12_RESOURCE_STATE_COMMON;
}

bool ColorConvert12::recordClearBlack(ID3D12GraphicsCommandList* list, std::string& error)
{
    ID3D12Resource* output = m_Output[m_Target].Get();
    if (!list || !output) {
        error = "colour conversion is not initialized";
        return false;
    }
    list->SetGraphicsRootSignature(m_RootSignature.Get());
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    transition(list, output, m_OutputState[m_Target], D3D12_RESOURCE_STATE_RENDER_TARGET);
    recordBlack(list);
    m_OutputCleared[m_Target] = true;
    transition(list, output, m_OutputState[m_Target], D3D12_RESOURCE_STATE_COMMON);
    return true;
}

void ColorConvert12::recordBlack(ID3D12GraphicsCommandList* list)
{
    // Limited-range black: 16 and 128 of 255 in 8 bits; 64 and 512 of 1023 in
    // P010, whose ten bits sit at the top of each 16-bit word (64 << 6, 512
    // << 6). A UNORM target stores those codes exactly. The fill's value is
    // the Overlay's first four floats (PsFill); the rest is left at zero.
    const float black[8] = {m_Hdr ? 4096.0f / 65535.0f : 16.0f / 255.0f};
    const float grey[8] = {m_Hdr ? 32768.0f / 65535.0f : 128.0f / 255.0f,
                           m_Hdr ? 32768.0f / 65535.0f : 128.0f / 255.0f};
    const auto plane = [&](Rtv which, ID3D12PipelineState* pso, int width, int height,
                           const float* value) {
        D3D12_CPU_DESCRIPTOR_HANDLE target = m_RtvHeap->GetCPUDescriptorHandleForHeapStart();
        target.ptr += static_cast<SIZE_T>(which) * m_RtvStride;
        list->OMSetRenderTargets(1, &target, FALSE, nullptr);
        viewport(list, 0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), width,
                 height);
        list->SetPipelineState(pso);
        list->SetGraphicsRoot32BitConstants(0, 8, value, 0);
        list->DrawInstanced(3, 1, 0, 0);
    };
    plane(lumaRtv(m_Target), m_FillLumaPso.Get(), m_CodedWidth, m_CodedHeight, black);
    plane(chromaRtv(m_Target), m_FillChromaPso.Get(), m_CodedWidth / 2, m_CodedHeight / 2, grey);
}

void ColorConvert12::frameCompleted()
{
    if (m_TimingSlot < 0) return;
    d3d12::QueueTimer::Sample sample;
    if (m_Timer.read(m_TimingSlot, sample)) m_ResampleCost.add(sample.gpuUs);
    m_TimingSlot = -1;
}

bool ColorConvert12::dropResample()
{
    if (m_Geometry.filter == ScaleFilter::Bilinear || m_Geometry.letterboxed) return false;
    // recordConvert() branches on the filter alone; the scaler's textures go
    // with it — VRAM an iGPU shares with the desktop. The caller has waited
    // for the frame in flight.
    m_Geometry.filter = ScaleFilter::Bilinear;
    m_ScaleHPso.Reset();
    m_ScaleVPso.Reset();
    m_ScaledMid.Reset();
    m_Scaled.Reset();
    if (m_TimingSlot >= 0) m_Timer.cancel(m_TimingSlot);
    m_TimingSlot = -1;
    m_Timer.reset();
    return true;
}

bool ColorConvert12::takeResampleCost(int64_t& costUs)
{
    if (m_ResampleCostTaken || !m_ResampleCost.done()) return false;
    m_ResampleCostTaken = true;
    costUs = m_ResampleCost.medianUs();
    m_Timer.reset();
    return true;
}

} // namespace mw::native::convert
