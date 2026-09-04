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
#include "Win32Cursor.h"

#include <d3d11.h>
#include <windows.h>
#include <wrl/client.h>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>

// Windows.Graphics.Capture — the fallback when Desktop Duplication refuses.
//
// ── When this is the only thing that works ──────────────────────────────────
//
// Desktop Duplication answers DXGI_ERROR_UNSUPPORTED when the display is not
// scanned out by the adapter it is asked of. That is the ordinary state of a
// hybrid laptop — the panel hangs off the iGPU while the dGPU is the one the
// game runs on — and there is no way around it from the DXGI side. WGC goes
// through the compositor instead and does not care which adapter scans out.
//
// It is a fallback and not the default on purpose: DDA wakes the caller ON the
// present, which is the single best latency property this engine has. WGC
// delivers frames through a pool the compositor fills, so it is a wake-up
// behind a queue rather than the present itself.
//
// ── Two things WGC does not give ────────────────────────────────────────────
//
// It has no pointer of its own to hand over — it can composite one into the
// picture and that is all — so the pointer is read from Win32 (see Win32Cursor)
// and composited by us, exactly as on the DDA path. And it has no "nothing
// changed" answer: the pool simply stays empty, which is reported as Timeout.
//
// ── Why no C++/WinRT ────────────────────────────────────────────────────────
//
// The engine is built without exceptions in mind and links no WinRT runtime;
// C++/WinRT's projection is exception-based by design. The three interfaces
// needed here are plain COM (ABI) types, so they are used as such: activation
// through RoGetActivationFactory, HRESULTs checked like any other, and no
// header that could throw across this boundary.

namespace mw::native::capture {

class WgcCapture : public IWindowsCapture
{
public:
    /// Same two arguments as DxgiDuplication, so the session can swap one for
    /// the other without knowing anything else about either.
    ///
    /// @param adapterLuid the GPU to create the D3D11 device on.
    /// @param outputIndex that adapter's index for the display. Used only to
    ///                    resolve the monitor handle: the capture itself goes
    ///                    through the compositor, which is why this backend
    ///                    works where Desktop Duplication answers UNSUPPORTED.
    WgcCapture(uint64_t adapterLuid, unsigned outputIndex);
    ~WgcCapture() override;

    /// Whether this machine has the API at all: Windows 10 1903 for capture,
    /// and the "can capture without a picker" support flag. Cheap, and answered
    /// without starting anything, so the probe can call it per boot.
    static bool available();

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
    int cursorHotspotX() const override { return m_Cursor2.hotspotX(); }
    int cursorHotspotY() const override { return m_Cursor2.hotspotY(); }

private:
    struct Impl;

    /// Convert a QPC-based system-relative time to the engine's steady clock.
    int64_t qpcToMicroseconds(int64_t qpc) const;

    uint64_t m_AdapterLuid = 0;
    unsigned m_OutputIndex = 0;
    HMONITOR m_Monitor = nullptr;

    Microsoft::WRL::ComPtr<ID3D11Device> m_Device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> m_Context;

    std::unique_ptr<Impl> d;

    int m_Width = 0;
    int m_Height = 0;
    DXGI_FORMAT m_Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    DesktopRect m_DesktopRect;

    CursorState m_Cursor;
    Win32Cursor m_Cursor2;

    int64_t m_QpcOrigin = 0;
    int64_t m_SteadyOriginUs = 0;
    int64_t m_QpcFrequency = 0;

    /// True between an Ok from acquire() and its release(), so the contract is
    /// enforced here rather than left to the caller's memory.
    bool m_Holding = false;
};

} // namespace mw::native::capture
