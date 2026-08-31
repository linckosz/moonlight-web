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

#include "DxgiDuplication.h"

#include "../../core/Log.h"

#include <chrono>

using Microsoft::WRL::ComPtr;

namespace mw::native::capture {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string hresultToString(HRESULT hr)
{
    char buffer[32] = {};
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

} // namespace

DxgiDuplication::DxgiDuplication(uint64_t adapterLuid, unsigned outputIndex)
    : m_AdapterLuid(adapterLuid)
    , m_OutputIndex(outputIndex)
{}

DxgiDuplication::~DxgiDuplication()
{
    stop();
}

int64_t DxgiDuplication::qpcToMicroseconds(int64_t qpc) const
{
    if (m_QpcFrequency <= 0) return steadyNowUs();
    const int64_t deltaTicks = qpc - m_QpcOrigin;
    // Scale before dividing would overflow on a long-running session; dividing
    // first would throw away sub-second precision. Split the difference by
    // taking whole seconds out first.
    const int64_t seconds = deltaTicks / m_QpcFrequency;
    const int64_t remainder = deltaTicks % m_QpcFrequency;
    return m_SteadyOriginUs + seconds * 1000000LL + (remainder * 1000000LL) / m_QpcFrequency;
}

bool DxgiDuplication::openAdapterAndOutput(ComPtr<IDXGIAdapter1>& adapter,
                                           ComPtr<IDXGIOutput>& output, std::string& error)
{
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(::CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        error = "DXGI is unavailable";
        return false;
    }

    ComPtr<IDXGIAdapter1> candidate;
    for (UINT i = 0;
         factory->EnumAdapters1(i, candidate.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND;
         ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(candidate->GetDesc1(&desc))) continue;

        const uint64_t luid =
            (static_cast<uint64_t>(static_cast<uint32_t>(desc.AdapterLuid.HighPart)) << 32) |
            static_cast<uint64_t>(desc.AdapterLuid.LowPart);
        if (luid != m_AdapterLuid) continue;

        // The LUID is the adapter's identity, so matching it means the
        // duplication and the encoder will sit on the same physical GPU as the
        // scanout — which is the entire zero-copy premise.
        adapter = candidate;
        if (FAILED(adapter->EnumOutputs(m_OutputIndex, output.ReleaseAndGetAddressOf()))) {
            error = "that display is no longer attached to its adapter";
            return false;
        }
        return true;
    }

    // A GPU can genuinely disappear between the probe and the launch: an
    // external enclosure unplugged, a driver reset, a hybrid switch.
    error = "the GPU that drives that display is no longer present";
    return false;
}

bool DxgiDuplication::start(std::string& error)
{
    stop();

    LARGE_INTEGER frequency = {};
    if (!::QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0) {
        error = "no high-resolution timer on this machine";
        return false;
    }
    m_QpcFrequency = frequency.QuadPart;

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput> output;
    if (!openAdapterAndOutput(adapter, output, error)) return false;

    // D3D_DRIVER_TYPE_UNKNOWN is required when an adapter is supplied — asking
    // for HARDWARE here would silently ignore the adapter and pick the default,
    // which on a multi-GPU machine is how a "zero-copy" pipeline quietly starts
    // copying across GPUs.
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained = {};
    HRESULT hr = ::D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, wanted,
        static_cast<UINT>(std::size(wanted)), D3D11_SDK_VERSION, m_Device.ReleaseAndGetAddressOf(),
        &obtained, m_Context.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error = "could not create a D3D11 device on that GPU (" + hresultToString(hr) + ")";
        return false;
    }

    // Ask for HDR first. DuplicateOutput1 lets the formats be named, and
    // listing the float format ahead of the 8-bit one is what keeps an HDR
    // desktop from being flattened to SDR before we ever see it. Machines and
    // drivers without Output5 fall back to the plain path.
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output.As(&output5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(m_Device.Get(), 0, static_cast<UINT>(std::size(formats)),
                                       formats, m_Duplication.ReleaseAndGetAddressOf());
    } else {
        hr = E_NOINTERFACE;
    }

    if (FAILED(hr)) {
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(output.As(&output1))) {
            error = "this display does not support Desktop Duplication";
            return false;
        }
        hr = output1->DuplicateOutput(m_Device.Get(), m_Duplication.ReleaseAndGetAddressOf());
    }

    if (FAILED(hr)) {
        // DXGI_ERROR_UNSUPPORTED is the one worth naming: it is what a hybrid
        // laptop returns when the display is driven through the other GPU, and
        // it is exactly the case Windows.Graphics.Capture exists to cover.
        if (hr == DXGI_ERROR_UNSUPPORTED)
            error = "Desktop Duplication is not supported for this display";
        else
            error = "could not start Desktop Duplication (" + hresultToString(hr) + ")";
        return false;
    }

    DXGI_OUTDUPL_DESC duplDesc = {};
    m_Duplication->GetDesc(&duplDesc);
    m_Width = static_cast<int>(duplDesc.ModeDesc.Width);
    m_Height = static_cast<int>(duplDesc.ModeDesc.Height);
    m_Format = duplDesc.ModeDesc.Format;

    // Calibrate the two clocks against each other, as close together as
    // possible — see qpcToMicroseconds.
    LARGE_INTEGER qpcNow = {};
    ::QueryPerformanceCounter(&qpcNow);
    m_QpcOrigin = qpcNow.QuadPart;
    m_SteadyOriginUs = steadyNowUs();

    log::info("[native] duplication started: " + std::to_string(m_Width) + "x" +
              std::to_string(m_Height) +
              (m_Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? " (HDR, FP16)" : " (SDR, BGRA8)"));
    return true;
}

AcquireStatus DxgiDuplication::acquire(int timeoutMs, CapturedFrame& frame)
{
    if (!m_Duplication) return AcquireStatus::Failed;

    // Holding two frames at once is not allowed by DXGI, and forgetting to
    // release is easy to do in an error path. Say so loudly rather than
    // returning an opaque failure from AcquireNextFrame.
    if (m_FrameHeld) {
        log::warning("[native] acquire() called while a frame was still held — releasing it");
        release();
    }

    DXGI_OUTDUPL_FRAME_INFO info = {};
    ComPtr<IDXGIResource> resource;
    const HRESULT hr = m_Duplication->AcquireNextFrame(static_cast<UINT>(timeoutMs), &info,
                                                       resource.GetAddressOf());

    if (hr == DXGI_ERROR_WAIT_TIMEOUT) return AcquireStatus::Timeout;
    if (hr == DXGI_ERROR_ACCESS_LOST) {
        log::info("[native] duplication lost (mode change or desktop switch) — will restart");
        return AcquireStatus::Lost;
    }
    if (FAILED(hr)) {
        log::warning("[native] AcquireNextFrame failed: " + hresultToString(hr));
        return AcquireStatus::Failed;
    }

    m_FrameHeld = true;

    // A present time of zero means DXGI woke us for a pointer change only: the
    // desktop image is unchanged, so there is nothing to encode. Treating it as
    // a frame would send a duplicate and, worse, stamp it with a bogus time.
    if (info.LastPresentTime.QuadPart == 0) {
        release();
        return AcquireStatus::Timeout;
    }

    if (FAILED(resource.As(&m_AcquiredTexture))) {
        release();
        log::warning("[native] duplicated frame was not a 2D texture");
        return AcquireStatus::Failed;
    }

    frame.texture = m_AcquiredTexture.Get();
    frame.presentUs = qpcToMicroseconds(info.LastPresentTime.QuadPart);
    frame.capturedUs = steadyNowUs();
    return AcquireStatus::Ok;
}

void DxgiDuplication::release()
{
    if (!m_FrameHeld) return;
    m_AcquiredTexture.Reset();
    if (m_Duplication) m_Duplication->ReleaseFrame();
    m_FrameHeld = false;
}

void DxgiDuplication::stop()
{
    release();
    m_Duplication.Reset();
    m_Context.Reset();
    m_Device.Reset();
    m_Width = 0;
    m_Height = 0;
}

} // namespace mw::native::capture
