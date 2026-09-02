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

#include "IWindowsCapture.h"

#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace mw::native::capture {

/// DXGI Desktop Duplication — the primary capture path on Windows.
///
/// Chosen over Windows.Graphics.Capture for one reason that outweighs the
/// rest: `AcquireNextFrame` blocks until the display actually presents and
/// hands back `LastPresentTime`, a QPC stamp of that present. So the frame is
/// picked up the instant it exists, and the engine knows precisely when it
/// existed — which is what makes every latency number downstream a measurement
/// rather than an estimate.
///
/// Known limits, shared with every other capture API on Windows and with
/// Sunshine: the secure desktop (UAC prompt, lock screen) cannot be captured,
/// and a mode change invalidates the duplication. The latter arrives as
/// AcquireStatus::Lost and is recovered by calling start() again.
class DxgiDuplication final : public IWindowsCapture
{
public:
    /// @param adapterLuid  the adapter that drives the display, packed as in
    ///                     GpuInfo::nativeHandle. Opening the duplication on
    ///                     any other adapter is what would force a cross-GPU
    ///                     copy, so the caller must pass the right one.
    /// @param outputIndex  which output of that adapter, in DXGI's own order —
    ///                     the same order the probe enumerated displays in.
    /// @param hdr          whether the consumer wants the HDR desktop as it is,
    ///                     in FP16 scRGB. False asks DXGI for 8-bit only, and
    ///                     an HDR desktop then arrives already tone-mapped to
    ///                     SDR — which is what an SDR stream wants, and the
    ///                     only thing the SDR-only converter accepts. Asking
    ///                     for FP16 "just in case" is what made every session
    ///                     on a machine with Windows HDR on fail at the click.
    DxgiDuplication(uint64_t adapterLuid, unsigned outputIndex, bool hdr);
    ~DxgiDuplication() override;

    bool start(std::string& error) override;
    AcquireStatus acquire(int timeoutMs, CapturedFrame& frame) override;
    void release() override;
    void stop() override;

    ID3D11Device* device() const override { return m_Device.Get(); }
    ID3D11DeviceContext* context() const override { return m_Context.Get(); }
    int width() const override { return m_Width; }
    int height() const override { return m_Height; }
    DXGI_FORMAT format() const override { return m_Format; }
    DesktopRect desktopRect() const override { return m_DesktopRect; }
    const CursorState& cursor() const override { return m_Cursor; }
    int cursorHotspotX() const override { return m_CursorHotspotX; }
    int cursorHotspotY() const override { return m_CursorHotspotY; }

private:
    /// Find the adapter whose LUID matches, and its requested output.
    bool openAdapterAndOutput(Microsoft::WRL::ComPtr<IDXGIAdapter1>& adapter,
                              Microsoft::WRL::ComPtr<IDXGIOutput>& output, std::string& error);

    /// Fold this frame's pointer information into m_Cursor. Returns true when
    /// anything the consumer can see changed (position, visibility or shape).
    bool updateCursor(const DXGI_OUTDUPL_FRAME_INFO& info);

    /// Turn DXGI's shape encoding — monochrome, colour, or masked colour — into
    /// the single RGBA + invert-mask form CursorState carries.
    void decodeShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape, const uint8_t* data,
                     size_t size);

    /// Convert a QPC tick count to microseconds on the engine's steady clock.
    ///
    /// DXGI reports present times in QPC ticks, while every other timestamp in
    /// the engine comes from steady_clock. The two count at the same rate but
    /// from different origins, so a calibration pair is taken once at start()
    /// and every present is expressed relative to it. Without this the capture
    /// latency would be a difference between two unrelated epochs — a large,
    /// stable, entirely meaningless number.
    int64_t qpcToMicroseconds(int64_t qpc) const;

    const uint64_t m_AdapterLuid;
    const unsigned m_OutputIndex;
    const bool m_Hdr;

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_Duplication;

    /// The texture handed out by the last successful acquire(). Held so
    /// release() can return it, and so a caller that forgets cannot deadlock
    /// the next acquire() silently.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_AcquiredTexture;
    bool m_FrameHeld = false;

    int m_Width = 0;
    int m_Height = 0;
    DXGI_FORMAT m_Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    DesktopRect m_DesktopRect;

    CursorState m_Cursor;
    /// The hotspot of the current shape, subtracted from DXGI's position to get
    /// the image's top-left. Kept apart from CursorState because the consumer
    /// never needs it — it only wants to know where to draw.
    int m_CursorHotspotX = 0;
    int m_CursorHotspotY = 0;
    /// Scratch for GetFramePointerShape, reused so a moving cursor does not
    /// allocate. DXGI tells us the size it needs before it fills it.
    std::vector<uint8_t> m_ShapeBuffer;

    /// Calibration for qpcToMicroseconds: ticks per second, plus one sample of
    /// both clocks taken at the same instant in start().
    int64_t m_QpcFrequency = 0;
    int64_t m_QpcOrigin = 0;
    int64_t m_SteadyOriginUs = 0;
};

} // namespace mw::native::capture
