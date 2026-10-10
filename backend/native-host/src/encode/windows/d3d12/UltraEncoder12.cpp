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

#include "encode/windows/d3d12/UltraEncoder12.h"

#include <algorithm>
#include <climits>

using Microsoft::WRL::ComPtr;

namespace mw::native::encode {

namespace {

constexpr uint32_t kWaitMs = 2000;

ComPtr<ID3D12Resource> makeBuffer(ID3D12Device* d, uint64_t size, D3D12_HEAP_TYPE heap,
                                  D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp = {};
    hp.Type = heap;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = size;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> r;
    if (FAILED(d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state, nullptr,
                                          IID_PPV_ARGS(&r))))
        return nullptr;
    return r;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES before,
                                  D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    return b;
}

} // namespace

UltraEncoder12::~UltraEncoder12()
{
    stop();
}

size_t UltraEncoder12::targetBytes(int mbps, int fps)
{
    const double bytes = double((std::max)(mbps, 1)) * 1e6 / 8.0 / double((std::max)(fps, 1));
    return (std::max)(size_t(bytes), size_t(4096));
}

bool UltraEncoder12::init(const std::shared_ptr<d3d12::D3d12Device>& device, Codec codec, int width,
                          int height, int fps, int bitrateKbps, bool hdr, bool intraRefresh,
                          const EncoderTuning& tuning, std::string& error)
{
    (void)codec;
    (void)bitrateKbps;
    (void)intraRefresh;
    if (hdr) {
        error = "PyroWave (Ultra) codes 8-bit SDR pictures only";
        return false;
    }
    m_Device = device;
    ID3D12Device* d = device->device();
    // 4:2:0 wants even sizes; the conversion writes at the size said here.
    m_Width = width & ~1;
    m_Height = height & ~1;
    m_Fps = (std::max)(fps, 1);
    // 128 Mbit/s = 134 KB a frame at 119 fps (design 6.42): 0.4 ms less on the
    // 1 Gbit/s wire than 170, text 24 dB instead of 28.
    m_Mbps = tuning.ultraMbps > 0 ? tuning.ultraMbps : 128;
    m_GpuTiming = tuning.gpuTiming;

    if (!m_Device->createQueue(d3d12::queueRequestFor(D3D12_COMMAND_LIST_TYPE_COMPUTE, tuning,
                                                      L"MoonlightWeb Ultra encode"),
                               m_Queue, error))
        return false;
    if (FAILED(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE,
                                         IID_PPV_ARGS(&m_Allocator))) ||
        FAILED(d->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, m_Allocator.Get(), nullptr,
                                    IID_PPV_ARGS(&m_List)))) {
        error = "Ultra: the compute command list could not be made";
        return false;
    }
    m_List->Close();
    if (!m_Done.create(d, false, error)) return false;

    // The NV12 picture's two planes as a copy lays them out in a buffer.
    D3D12_RESOURCE_DESC nv12 = {};
    nv12.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    nv12.Width = UINT64(m_Width);
    nv12.Height = UINT(m_Height);
    nv12.DepthOrArraySize = 1;
    nv12.MipLevels = 1;
    nv12.Format = DXGI_FORMAT_NV12;
    nv12.SampleDesc.Count = 1;
    UINT64 total = 0;
    d->GetCopyableFootprints(&nv12, 0, 2, 0, m_Planes, nullptr, nullptr, &total);
    m_Source = makeBuffer(d, total, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON);
    if (!m_Source) {
        error = "Ultra: the source buffer could not be made";
        return false;
    }
    m_Layout.yOffset = uint32_t(m_Planes[0].Offset);
    m_Layout.yPitch = m_Planes[0].Footprint.RowPitch;
    m_Layout.cbOffset = uint32_t(m_Planes[1].Offset);
    m_Layout.crOffset = uint32_t(m_Planes[1].Offset) + 1;
    m_Layout.cPitch = m_Planes[1].Footprint.RowPitch;
    m_Layout.interleaved = true;
    m_Layout.bytes = uint32_t(total);

    D3D12_QUERY_HEAP_DESC qh = {};
    qh.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qh.Count = 6;
    if (FAILED(d->CreateQueryHeap(&qh, IID_PPV_ARGS(&m_Timestamps)))) {
        error = "Ultra: no timestamp query heap";
        return false;
    }
    m_TimestampReadback =
        makeBuffer(d, 64, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    m_Queue.queue->GetTimestampFrequency(&m_TimestampFrequency);

    if (!m_Encoder.init(d, m_Width, m_Height, &error)) {
        error = "Ultra: " + error;
        return false;
    }
    return true;
}

bool UltraEncoder12::encode(ID3D12Resource* picture, ID3D12Fence* ready, uint64_t readyValue,
                            bool forceKeyframe, uint32_t frameNumber, EncoderOutput& out,
                            std::string& error)
{
    (void)forceKeyframe; // every frame stands alone
    (void)frameNumber;
    ID3D12CommandQueue* queue = m_Queue.queue.Get();
    if (ready && FAILED(queue->Wait(ready, readyValue))) {
        error = "Ultra: waiting for the conversion failed";
        return false;
    }
    if (FAILED(m_Allocator->Reset()) || FAILED(m_List->Reset(m_Allocator.Get(), nullptr))) {
        error = "Ultra: the command list could not be reset";
        return false;
    }
    // Both planes of the picture (left in COMMON by the conversion) into the
    // source buffer, then the encode reads it as a shader resource.
    D3D12_RESOURCE_BARRIER in[2] = {
        transition(picture, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
        transition(m_Source.Get(), D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
    m_List->ResourceBarrier(2, in);
    for (UINT plane = 0; plane < 2; ++plane) {
        D3D12_TEXTURE_COPY_LOCATION dst = {};
        dst.pResource = m_Source.Get();
        dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint = m_Planes[plane];
        D3D12_TEXTURE_COPY_LOCATION src = {};
        src.pResource = picture;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = plane;
        m_List->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    D3D12_RESOURCE_BARRIER mid[2] = {
        transition(picture, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON),
        transition(m_Source.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                   D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)};
    m_List->ResourceBarrier(2, mid);
    m_Encoder.record(m_List.Get(), m_Source.Get(), m_Layout, targetBytes(m_Mbps, m_Fps),
                     m_Timestamps.Get(), 0, m_TimestampReadback.Get());
    D3D12_RESOURCE_BARRIER back =
        transition(m_Source.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                   D3D12_RESOURCE_STATE_COMMON);
    m_List->ResourceBarrier(1, &back);
    if (FAILED(m_List->Close())) {
        error = "Ultra: the command list did not close (a barrier the driver refused)";
        return false;
    }
    ID3D12CommandList* lists[] = {m_List.Get()};
    queue->ExecuteCommandLists(1, lists);
    m_DoneValue = m_Done.signal(queue, error);
    if (m_DoneValue == 0) return false;
    if (m_Done.wait(m_DoneValue, kWaitMs, error) != d3d12::GpuFence::Wait::Done) {
        error = "Ultra: " + error;
        return false;
    }
    // One frame, its packets back to back: no packet boundary to keep here,
    // the transport cuts the frame its own way.
    std::string why;
    if (!m_Encoder.packets(SIZE_MAX, m_Packets, &why)) {
        error = "Ultra: " + why;
        return false;
    }
    m_Frame.clear();
    for (const auto& p : m_Packets)
        m_Frame.insert(m_Frame.end(), p.begin(), p.end());
    out = EncoderOutput{};
    out.data = m_Frame.data();
    out.size = m_Frame.size();
    out.keyframe = true;
    if (m_GpuTiming && m_TimestampFrequency)
        out.gpuEncodeUs = int64_t(
            PyroWaveEncoder12::stageTimes(m_TimestampReadback.Get(), 0, m_TimestampFrequency)
                .total() *
            1000.0);
    return true;
}

ID3D12Fence* UltraEncoder12::inputReleased(uint64_t& value) const
{
    value = m_DoneValue;
    return m_DoneValue ? m_Done.fence() : nullptr;
}

void UltraEncoder12::stop()
{
    if (m_DoneValue && m_Done) {
        std::string ignored;
        m_Done.wait(m_DoneValue, kWaitMs, ignored);
    }
}

bool UltraEncoder12::setBitrate(int bitrateKbps, std::string& error)
{
    // The rate is the bench key's (ultrambps): the session's adaptive bitrate
    // speaks for HEVC, not for an intra codec ten times its size.
    (void)bitrateKbps;
    (void)error;
    return true;
}

} // namespace mw::native::encode
