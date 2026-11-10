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

#include "../../../capture/windows/IWindowsCapture.h"
#include "../../../platform/windows/d3d12/D3d12Device.h"
#include "../../CursorDraw.h"
#include "../../ResampleCost.h"
#include "../../ScaleFilter.h"
#include "../ConvertShaders.h"

#include <d3d12.h>
#include <wrl/client.h>

#include <string>

namespace mw::native::convert {

/// ColorConvert on D3D12: the same HLSL, compiled the same way
/// (ConvertShaders), drawn with pixel shaders — so on WARP the two write the
/// same bytes, which is what test_color_convert12 holds them to.
///
/// ── What is different ──────────────────────────────────────────────────────
///
/// It records; it does not run. Every call takes the pipeline's command list
/// and adds to it — the copy of the desktop, the resample, the conversion,
/// the barriers — so a frame is one list on the pipeline's DIRECT queue, sent
/// with one ExecuteCommandLists, ordered against the capture and the encoder
/// by the pipeline's fences (DdaInterop).
///
/// The output is written at the size the encoder codes (a whole number of
/// blocks: 1920×1088 for a 1080p stream on the Arc), the picture in its
/// top-left corner; the band below and to its right is cleared to black once,
/// and cropped away by the SPS.
///
/// Every resource it hands over is left in COMMON at the end of a list: the
/// duplication's surface goes back to D3D11 and the output on to the
/// video-encode queue, and COMMON is the state both can take it from.
///
/// 4:2:0 only (NV12, or P010 for HDR): 4:4:4 stays on D3D11, where the D3D12
/// route refuses it.
class ColorConvert12
{
public:
    ColorConvert12() = default;
    ~ColorConvert12();

    ColorConvert12(const ColorConvert12&) = delete;
    ColorConvert12& operator=(const ColorConvert12&) = delete;

    /// ColorConvert::init's contract on @p device — @p sourceTurn included:
    /// the picture stood upright as it is read — plus the coded size (0 = the
    /// output size; never smaller, and even) and the @p queue the lists run
    /// on, whose timestamps time the resample pass.
    bool init(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT sourceFormat,
              int sourceWidth, int sourceHeight, int outputWidth, int outputHeight, int codedWidth,
              int codedHeight, bool hdr, ScaleFilter filter, std::string& error,
              int sourceTurn = 0);

    /// Records the conversion of @p source — the duplication's surface opened
    /// in D3D12, or held() — into @p list, @p cursor drawn on the way through.
    /// @p source is in COMMON before and after; so is output().
    bool recordConvert(ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                       const capture::CursorState& cursor, const CursorDraw& draw,
                       std::string& error);

    /// Records a copy of @p source (COMMON before and after) into this
    /// converter's own copy of the desktop, held(): the picture the frames
    /// re-sent without a capture are converted from again.
    bool recordCopy(ID3D12GraphicsCommandList* list, ID3D12Resource* source, std::string& error);

    ID3D12Resource* held() const { return m_Held.Get(); }
    void releaseHeld();

    /// The held copy handed from one converter to the next across a rebuild
    /// that keeps it (the load cap's): same device, same capture.
    Microsoft::WRL::ComPtr<ID3D12Resource> takeHeld();
    void setHeld(Microsoft::WRL::ComPtr<ID3D12Resource> held);

    /// Records output() cleared to limited-range black — 16/128 in 8 bits,
    /// 64/512 in P010's ten — what a capture restart shows. The same bytes as
    /// ColorConvert converting a black picture with no pointer. Drawn, not
    /// cleared: see recordBlack.
    bool recordClearBlack(ID3D12GraphicsCommandList* list, std::string& error);

    /// The tests' way to record a conversion alone: the next recordConvert()
    /// skips the one clear of the band outside the picture.
    void assumeOutputCleared() { m_OutputCleared[m_Target] = true; }

    /// A second output, the same size and format as the first (plan Phase
    /// 10, pipelined=1): the pipeline converts the next picture into one while
    /// the encoder still reads the other. Each has its band cleared on its
    /// first use.
    bool addOutput(std::string& error);
    /// 1, or 2 once addOutput() made the second.
    int outputs() const { return m_Outputs; }
    /// The output the next recordConvert() or recordClearBlack() writes, and
    /// output() names: 0 unless the pipeline chose the second.
    void selectOutput(int index) { m_Target = index > 0 && index < m_Outputs ? index : 0; }
    int selectedOutput() const { return m_Target; }

    /// The list recorded by the last recordConvert() has completed: its
    /// resample timing, if it had one, is read.
    void frameCompleted();

    /// See ColorConvert.
    ScaleFilter scaleFilter() const { return m_Geometry.filter; }
    bool dropResample();
    bool takeResampleCost(int64_t& costUs);
    bool letterboxed() const { return m_Geometry.letterboxed; }
    bool hdr() const { return m_Hdr; }
    bool toneMapsToSdr() const { return m_ToneMap; }
    bool scRgbSource() const { return m_Hdr || m_ToneMap; }
    void setSdrWhite(float scRgbWhite);
    float sdrWhite() const { return m_SdrWhite; }

    /// NV12 or P010, codedWidth() × codedHeight(), the picture in the top-left
    /// outputWidth() × outputHeight(): the selected output, or @p index.
    ID3D12Resource* output() const { return m_Output[m_Target].Get(); }
    ID3D12Resource* output(int index) const
    {
        return index >= 0 && index < m_Outputs ? m_Output[index].Get() : nullptr;
    }
    int outputWidth() const { return m_Geometry.outputWidth; }
    int outputHeight() const { return m_Geometry.outputHeight; }
    int codedWidth() const { return m_CodedWidth; }
    int codedHeight() const { return m_CodedHeight; }

private:
    void release();
    bool createPipeline(std::string& error);
    /// Output @p index and its two plane views.
    bool createOutput(int index, std::string& error);
    bool createScaler(std::string& error);
    bool createPso(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT target,
                   Microsoft::WRL::ComPtr<ID3D12PipelineState>& pso, std::string& error);
    bool updateCursorResources(ID3D12GraphicsCommandList* list, const capture::CursorState& cursor,
                               std::string& error);
    /// The next descriptor table of the ring: t0 = @p source viewed as
    /// @p format, t1-t2 the pointer (or null descriptors).
    D3D12_GPU_DESCRIPTOR_HANDLE table(ID3D12Resource* source, DXGI_FORMAT format, bool cursor);
    void transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                    D3D12_RESOURCE_STATES& state, D3D12_RESOURCE_STATES next);
    /// The whole output, both planes, at limited-range black (16/128 in 8
    /// bits, 64/512 in P010's ten), the output already a render target. Drawn,
    /// never ClearRenderTargetView: that clear on a P010 plane loses the
    /// device of the N95's UHD Graphics (driver 32.0.101.7088, 28/09/2026),
    /// where the same draw is fine. The same bytes as the clear everywhere.
    /// The root signature is set; the root constants are overwritten.
    void recordBlack(ID3D12GraphicsCommandList* list);

    Microsoft::WRL::ComPtr<ID3D12Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_RootSignature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_LumaPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ChromaPso;
    /// A plane at the value of the root constants (recordBlack).
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_FillLumaPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_FillChromaPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ScaleHPso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_ScaleVPso;
    Microsoft::WRL::ComPtr<ID3DBlob> m_VertexShader;

    /// Shader-visible: kRing frames × three tables (resample H, resample V,
    /// conversion) × three descriptors. Written as a frame is recorded; a
    /// frame's list is done long before its tables come round again.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_SrvHeap;
    UINT m_SrvStride = 0;
    UINT m_SrvNext = 0;
    /// Luma, chroma, the resample intermediate, the scaled picture.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_RtvHeap;
    UINT m_RtvStride = 0;

    static constexpr int kMaxOutputs = 2;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Output[kMaxOutputs];
    D3D12_RESOURCE_STATES m_OutputState[kMaxOutputs] = {D3D12_RESOURCE_STATE_COMMON,
                                                        D3D12_RESOURCE_STATE_COMMON};
    bool m_OutputCleared[kMaxOutputs] = {};
    int m_Outputs = 0;
    int m_Target = 0;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_ScaledMid;
    D3D12_RESOURCE_STATES m_ScaledMidState = D3D12_RESOURCE_STATE_COMMON;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Scaled;
    D3D12_RESOURCE_STATES m_ScaledState = D3D12_RESOURCE_STATE_COMMON;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_Held;
    D3D12_RESOURCE_STATES m_HeldState = D3D12_RESOURCE_STATE_COMMON;

    Microsoft::WRL::ComPtr<ID3D12Resource> m_CursorPixels;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_CursorInvert;
    /// The upload of the last shape, kept until the next one: the list that
    /// copies from it has long completed by then.
    Microsoft::WRL::ComPtr<ID3D12Resource> m_CursorUpload;
    uint64_t m_CursorShapeVersion = 0;

    d3d12::QueueTimer m_Timer;
    int m_TimingSlot = -1;
    ResampleCost m_ResampleCost;
    bool m_ResampleCostTaken = false;

    ConvertGeometry m_Geometry;
    bool m_Hdr = false;
    bool m_ToneMap = false;
    float m_SdrWhite = 1.0f;
    DXGI_FORMAT m_SourceFormat = DXGI_FORMAT_UNKNOWN;
    int m_SourceWidth = 0;
    int m_SourceHeight = 0;
    /// See ColorConvert::init: 0 to 3.
    int m_SourceTurn = 0;
    int m_CodedWidth = 0;
    int m_CodedHeight = 0;
};

} // namespace mw::native::convert
