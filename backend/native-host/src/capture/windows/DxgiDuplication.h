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
#include "PaintedPointer.h"

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
    ///
    /// The desktop arrives as it is: BGRA8 from an SDR desktop, FP16 scRGB
    /// from an HDR one — format() says which, once started. An SDR session on
    /// an HDR desktop used to ask DXGI for 8-bit and let it tone-map, and
    /// what DXGI does is clip at 80 nits, which is below every window on a
    /// display whose SDR brightness slider has been touched. The converter
    /// tone-maps FP16 itself now, so there is nothing left to ask DXGI for.
    DxgiDuplication(uint64_t adapterLuid, unsigned outputIndex);
    ~DxgiDuplication() override;

    bool start(std::string& error) override;
    AcquireStatus acquire(int timeoutMs, CapturedFrame& frame) override;
    void release() override;
    void stop() override;
    /// The duplication let go and asked for again, of the same output on the
    /// same device: a new duplication's first frame is the whole desktop,
    /// whether anything moved or not. Its size and format must be the ones
    /// start() read, or the display changed meanwhile and the next acquire()
    /// says Lost — as it does when DXGI refuses the new duplication.
    bool retake(std::string& error) override;

    ID3D11Device* device() const override { return m_Device.Get(); }
    ID3D11DeviceContext* context() const override { return m_Context.Get(); }
    int width() const override { return m_Width; }
    int height() const override { return m_Height; }
    /// From the duplication's Rotation, read at each start(): the mode's size
    /// is stood the way the desktop is, the pictures come as the panel scans
    /// them out (DisplayRotation.h).
    int turn() const override { return m_Turn; }
    DXGI_FORMAT format() const override { return m_Format; }
    DesktopRect desktopRect() const override { return m_DesktopRect; }
    const CursorState& cursor() const override { return m_Cursor; }
    int cursorHotspotX() const override { return m_CursorHotspotX; }
    int cursorHotspotY() const override { return m_CursorHotspotY; }
    bool pointerPaintedIn() const override { return m_PaintedPointer.paintedIn(); }
    int64_t foldedPresents() const override { return m_FoldedPresents; }
    /// From the duplication's ModeDesc.RefreshRate, read at each start().
    int64_t refreshPeriodNs() const override { return m_RefreshPeriodNs; }

    /// Whether the last start() was refused for a reason that can pass, so
    /// that a later one may succeed: the secure desktop below SYSTEM
    /// (E_ACCESSDENIED), another program's duplication in the way, a display
    /// between two modes. False after a success, and for the refusals that
    /// stay — DXGI_ERROR_UNSUPPORTED above all, a display another GPU drives,
    /// the hybrid laptop Windows.Graphics.Capture exists for.
    bool refusalMayPass() const { return m_RefusalMayPass; }

private:
    /// Ask Win32 where the pointer is, for as long as PaintedPointer has no
    /// verdict. Costs one GetCursorInfo per acquire, and nothing afterwards.
    void samplePointerForVerdict();

    /// Find the adapter whose LUID matches, and its requested output.
    bool openAdapterAndOutput(Microsoft::WRL::ComPtr<IDXGIAdapter1>& adapter,
                              Microsoft::WRL::ComPtr<IDXGIOutput>& output, std::string& error);

    /// m_Duplication made of m_Output on m_Device: start()'s, and retake()'s.
    /// Sets m_RefusalMayPass.
    bool duplicate(std::string& error);

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

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;
    /// The output start() found, kept for retake(): asked again by index, it
    /// could be another display by then (OutputMatch.h).
    Microsoft::WRL::ComPtr<IDXGIOutput> m_Output;
    Microsoft::WRL::ComPtr<IDXGIOutputDuplication> m_Duplication;
    /// retake() was answered: the next frame is flagged retaken.
    bool m_Retaking = false;
    /// retake() let go of the duplication and got none back, or one of
    /// another mode: the next acquire() says Lost.
    bool m_RetakeLost = false;

    /// The texture handed out by the last successful acquire(). Held so
    /// release() can return it, and so a caller that forgets cannot deadlock
    /// the next acquire() silently.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> m_AcquiredTexture;
    bool m_FrameHeld = false;
    /// Whether this duplication has delivered a frame since start(): what
    /// tells a duplication a display change invalidated (it worked, then
    /// DXGI_ERROR_INVALID_CALL) from one that never could (see acquire()).
    bool m_Delivered = false;
    int64_t m_FoldedPresents = 0;

    int m_Width = 0;
    int m_Height = 0;
    /// See turn(). And whether the first picture of a turned duplication has
    /// had its size looked at yet — the one look acquire() takes, for the log.
    int m_Turn = 0;
    bool m_TurnChecked = true;
    int64_t m_RefreshPeriodNs = 0;
    DXGI_FORMAT m_Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    DesktopRect m_DesktopRect;

    CursorState m_Cursor;
    /// The hotspot of the current shape, subtracted from DXGI's position to get
    /// the image's top-left. Kept apart from CursorState because the consumer
    /// never needs it — it only wants to know where to draw.
    int m_CursorHotspotX = 0;
    int m_CursorHotspotY = 0;
    /// Desktop Duplication says the pointer is hidden while Windows shows it —
    /// see updateCursor(). While this holds, Windows' word is used instead.
    bool m_HiddenOverridden = false;
    bool m_HiddenOverrideLogged = false;
    /// Scratch for GetFramePointerShape, reused so a moving cursor does not
    /// allocate. DXGI tells us the size it needs before it fills it.
    std::vector<uint8_t> m_ShapeBuffer;
    /// Whether this duplication keeps the pointer apart at all. Reset by start().
    PaintedPointer m_PaintedPointer;
    /// See refusalMayPass().
    bool m_RefusalMayPass = false;

    /// Calibration for qpcToMicroseconds: ticks per second, plus one sample of
    /// both clocks taken at the same instant in start().
    int64_t m_QpcFrequency = 0;
    int64_t m_QpcOrigin = 0;
    int64_t m_SteadyOriginUs = 0;
};

} // namespace mw::native::capture
