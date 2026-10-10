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

#include "encode/windows/d3d12/IVideoEncoder12.h"
#include "encode/windows/d3d12/PyroWaveEncoder12.h"
#include "platform/windows/d3d12/D3d12Device.h"

#include <wrl/client.h>

#include <memory>
#include <string>
#include <vector>

namespace mw::native::encode {

/// The POC Ultra encoder on the D3D12 route (docs/design/ultra-lan-poc.md,
/// U4): PyroWave, an intra-only wavelet codec, in place of HEVC. Bench key
/// enc12=pyrowave (with pipeline=d3d12), its rate ultrambps=<Mbit/s>.
///
/// The conversion's NV12 picture is copied into a buffer (both planes, as
/// their copyable footprints lay them out), then PyroWaveEncoder12 codes it on
/// a COMPUTE queue of its own. The CPU waits for the stream, as for every
/// encoder of the route. Every frame stands alone: each is a "keyframe", a
/// keyframe request costs nothing, and a lost frame needs no repair.
///
/// The frame goes out as the bytes of all its PyroWave packets back to back,
/// the start-of-frame header first: the page's decoder takes them in one call.
class UltraEncoder12 final : public IVideoEncoder12
{
public:
    UltraEncoder12() = default;
    ~UltraEncoder12() override;

    bool init(const std::shared_ptr<d3d12::D3d12Device>& device, Codec codec, int width, int height,
              int fps, int bitrateKbps, bool hdr, bool intraRefresh, const EncoderTuning& tuning,
              std::string& error) override;
    int codedWidth() const override { return m_Width; }
    int codedHeight() const override { return m_Height; }
    bool encode(ID3D12Resource* picture, ID3D12Fence* ready, uint64_t readyValue,
                bool forceKeyframe, uint32_t frameNumber, EncoderOutput& out,
                std::string& error) override;
    ID3D12Fence* inputReleased(uint64_t& value) const override;
    void releaseOutput() override {}
    void stop() override;
    bool setBitrate(int bitrateKbps, std::string& error) override;
    bool intraRefreshEnabled() const override { return false; }
    std::string describe() const override { return "PyroWave (D3D12)"; }

    /// The frame size the rate control aims at, in bytes, for @p mbps at
    /// @p fps (at least one packet's worth).
    static size_t targetBytes(int mbps, int fps);

private:
    std::shared_ptr<d3d12::D3d12Device> m_Device;
    d3d12::Queue m_Queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> m_Allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_List;
    d3d12::GpuFence m_Done;
    uint64_t m_DoneValue = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_Source;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> m_Timestamps;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_TimestampReadback;
    uint64_t m_TimestampFrequency = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT m_Planes[2] = {};
    PyroWaveEncoder12::SourceLayout m_Layout;
    PyroWaveEncoder12 m_Encoder;
    std::vector<std::vector<uint8_t>> m_Packets;
    std::vector<uint8_t> m_Frame;
    int m_Width = 0, m_Height = 0, m_Fps = 60, m_Mbps = 128;
    bool m_GpuTiming = false;
};

} // namespace mw::native::encode
