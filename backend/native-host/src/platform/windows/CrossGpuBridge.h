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

#include <cstdint>
#include <string>

namespace mw::native {

/// Carries a captured frame from the GPU that scanned it out to the GPU that
/// will encode it, when those are not the same adapter (design §6).
///
/// ── What this costs, and why it is still worth having ───────────────────────
///
/// D3D11 has no way to hand a texture from one adapter to another: shared
/// handles are per adapter, and a resource created on one device is invisible
/// to the other. The only road is through system memory — the frame is copied
/// into a staging texture on the source GPU, mapped, copied row by row into a
/// dynamic texture on the destination GPU, and unmapped. A 1440p BGRA8 frame is
/// 14.7 MB; at memory speed that is a few milliseconds, which is more than the
/// whole of the rest of the pipeline. Every zero-copy promise of the engine is
/// given up on this path, and the Selector avoids it whenever the display's own
/// GPU can encode at all.
///
/// It exists for two cases:
///
///  - a display driven by a GPU with no encoder — a laptop's panel on an iGPU
///    the driver exposes no encoder for, a virtual adapter — where the
///    alternative to the copy is no stream at all;
///  - the bench, which needs to measure an encoder that drives no display (an
///    iGPU beside a discrete card, here) and forces the GPU on purpose.
///
/// The time it takes is counted inside the frame's `convert` stage — the bridge
/// runs between the acquire and the colour pass — and logged on its own at the
/// end of the session, so a session that paid for it says so in numbers.
class CrossGpuBridge
{
public:
    /// @param encodeAdapterLuid the adapter to bring frames TO, packed as in
    ///                          GpuInfo::nativeHandle.
    explicit CrossGpuBridge(uint64_t encodeAdapterLuid);
    ~CrossGpuBridge();

    CrossGpuBridge(const CrossGpuBridge&) = delete;
    CrossGpuBridge& operator=(const CrossGpuBridge&) = delete;

    /// Create the destination device. Once per session; survives capture
    /// restarts, since the encoder's adapter does not change when the
    /// duplication does.
    bool open(std::string& error);

    /// The device the converter and encoder must be built on.
    ID3D11Device* device() const { return m_Device.Get(); }
    ID3D11DeviceContext* context() const { return m_Context.Get(); }

    /// Bring @p source — a texture on @p sourceDevice — onto this bridge's
    /// device. The result is owned here and valid until the next call; it is
    /// recreated when the source changes size, format or device (a capture
    /// restart), never per frame.
    ID3D11Texture2D* transfer(ID3D11Device* sourceDevice, ID3D11DeviceContext* sourceContext,
                              ID3D11Texture2D* source, std::string& error);

    /// How many transfers happened, and how long they took on average and at
    /// most, in microseconds — for the end-of-session log.
    int64_t transfers() const { return m_Transfers; }
    int64_t meanUs() const { return m_Transfers > 0 ? m_TotalUs / m_Transfers : 0; }
    int64_t maxUs() const { return m_MaxUs; }
    /// Bytes one frame costs to move, once the first has been.
    size_t bytesPerFrame() const { return m_BytesPerFrame; }

private:
    bool ensureBuffers(ID3D11Device* sourceDevice, const D3D11_TEXTURE2D_DESC& desc,
                       std::string& error);

    const uint64_t m_AdapterLuid;

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;

    /// Readback on the SOURCE device, and the device it belongs to — a capture
    /// restart hands out a new device, and a staging texture from the old one
    /// would be silently useless against it.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Staging;
    ID3D11Device* m_StagingDevice = nullptr;
    /// Upload on THIS device: what the converter reads.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_Upload;

    UINT m_Width = 0;
    UINT m_Height = 0;
    DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;
    size_t m_BytesPerPixel = 0;
    size_t m_BytesPerFrame = 0;

    int64_t m_Transfers = 0;
    int64_t m_TotalUs = 0;
    int64_t m_MaxUs = 0;
};

} // namespace mw::native
