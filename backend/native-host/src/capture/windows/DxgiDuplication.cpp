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
#include "CursorShape.h"

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

DxgiDuplication::DxgiDuplication(uint64_t adapterLuid, unsigned outputIndex, bool hdr)
    : m_AdapterLuid(adapterLuid)
    , m_OutputIndex(outputIndex)
    , m_Hdr(hdr)
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

    // DuplicateOutput1 lets the formats be named, and the list decides what an
    // HDR desktop becomes. Listing the float format ahead of the 8-bit one
    // keeps it as it is, FP16 scRGB, for a consumer that will render HDR.
    // Naming 8-bit alone makes DXGI hand it over already tone-mapped to SDR —
    // exactly the picture an SDR stream should carry, and the only one the SDR
    // converter accepts. Which list is used follows what the session consumes,
    // not what the display happens to be doing: the previous "ask for HDR
    // first, always" got FP16 on every machine with Windows HDR on, and the
    // converter then refused it, so the session failed at the click. Machines
    // and drivers without Output5 fall back to the plain path.
    // Set when DuplicateOutput1 was given a single format and accepted it: the
    // frames are then that format whatever the display mode says. See where
    // m_Format is settled below — this is not a cosmetic detail.
    DXGI_FORMAT forcedFormat = DXGI_FORMAT_UNKNOWN;

    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(output.As(&output5))) {
        const DXGI_FORMAT hdrFormats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT,
                                          DXGI_FORMAT_B8G8R8A8_UNORM};
        const DXGI_FORMAT sdrFormats[] = {DXGI_FORMAT_B8G8R8A8_UNORM};
        const DXGI_FORMAT* formats = m_Hdr ? hdrFormats : sdrFormats;
        const UINT count = static_cast<UINT>(m_Hdr ? std::size(hdrFormats) : std::size(sdrFormats));
        hr = output5->DuplicateOutput1(m_Device.Get(), 0, count, formats,
                                       m_Duplication.ReleaseAndGetAddressOf());
        if (SUCCEEDED(hr) && count == 1) forcedFormat = formats[0];
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

    // ⚠️ ModeDesc.Format is the DISPLAY MODE's format, not the duplication's
    // output format, and on an HDR desktop the two differ.
    //
    // DuplicateOutput1 converts the desktop into the first format of the list
    // it can honour — so an SDR session, whose list is BGRA8 alone, really does
    // receive BGRA8 frames. ModeDesc goes on reporting the mode: FP16. Taking
    // it at its word had the converter build an FP16 shader-resource view over
    // a BGRA8 texture, CreateShaderResourceView refuse it, and the session die
    // at the first frame with "could not view the captured frame".
    //
    // That was every SDR stream from a machine with Windows HDR on — the exact
    // case B1 set out to fix and only half fixed: B1 stopped ASKING for FP16,
    // this stops MISREADING what came back (found 04/09/2026 while the HDR path
    // was being written, on a display put into HDR mode for the occasion).
    //
    // A single-entry list is the only case where the answer is knowable without
    // acquiring a frame, and it is also the only case where ModeDesc is wrong:
    // with the two-entry HDR list DXGI keeps the desktop's own format, which is
    // precisely what ModeDesc reports, and the plain DuplicateOutput fallback
    // converts nothing at all.
    m_Format = forcedFormat != DXGI_FORMAT_UNKNOWN ? forcedFormat : duplDesc.ModeDesc.Format;

    // Where this display sits on the desktop, for aiming absolute mouse input.
    // Kept exactly as DXGI reports it — see DesktopRect on why the DPI
    // virtualization is wanted here and nowhere else.
    DXGI_OUTPUT_DESC outputDesc = {};
    if (SUCCEEDED(output->GetDesc(&outputDesc))) {
        m_DesktopRect.left = static_cast<int>(outputDesc.DesktopCoordinates.left);
        m_DesktopRect.top = static_cast<int>(outputDesc.DesktopCoordinates.top);
        m_DesktopRect.right = static_cast<int>(outputDesc.DesktopCoordinates.right);
        m_DesktopRect.bottom = static_cast<int>(outputDesc.DesktopCoordinates.bottom);
    }

    // Calibrate the two clocks against each other, as close together as
    // possible — see qpcToMicroseconds.
    LARGE_INTEGER qpcNow = {};
    ::QueryPerformanceCounter(&qpcNow);
    m_QpcOrigin = qpcNow.QuadPart;
    m_SteadyOriginUs = steadyNowUs();

    log::info("[native] duplication started: " + std::to_string(m_Width) + "x" +
              std::to_string(m_Height) +
              (m_Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? " (HDR, FP16)"
               : m_Format == DXGI_FORMAT_B8G8R8A8_UNORM
                   ? " (SDR, BGRA8)"
                   : " (format " + std::to_string(static_cast<int>(m_Format)) + ")"));
    return true;
}

void DxgiDuplication::decodeShape(const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape, const uint8_t* data,
                                  size_t size)
{
    ShapeSource source;
    switch (shape.Type) {
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME:
        source.encoding = ShapeSource::Encoding::Monochrome;
        break;
    case DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR:
        source.encoding = ShapeSource::Encoding::MaskedColor;
        break;
    default: source.encoding = ShapeSource::Encoding::Color; break;
    }
    source.width = static_cast<int>(shape.Width);
    // A monochrome cursor packs two 1-bit masks into one image: the AND mask on
    // top, the XOR mask below. Its real height is half what DXGI reports.
    source.height = static_cast<int>(shape.Height) /
                    (source.encoding == ShapeSource::Encoding::Monochrome ? 2 : 1);
    source.pitch = static_cast<int>(shape.Pitch);
    source.hotspotX = static_cast<int>(shape.HotSpot.x);
    source.hotspotY = static_cast<int>(shape.HotSpot.y);
    source.data = data;
    source.size = size;

    // Kept even when the shape is refused, so the hotspot never describes a
    // shape other than the one on screen.
    m_CursorHotspotX = source.hotspotX;
    m_CursorHotspotY = source.hotspotY;

    decodeCursorShape(source, m_Cursor);
}

bool DxgiDuplication::updateCursor(const DXGI_OUTDUPL_FRAME_INFO& info)
{
    bool changed = false;

    // A shape only arrives when it actually changed, so this is rare — a cursor
    // keeps one shape for thousands of frames.
    if (info.PointerShapeBufferSize > 0) {
        if (m_ShapeBuffer.size() < info.PointerShapeBufferSize)
            m_ShapeBuffer.resize(info.PointerShapeBufferSize);

        DXGI_OUTDUPL_POINTER_SHAPE_INFO shape = {};
        UINT required = 0;
        const HRESULT hr = m_Duplication->GetFramePointerShape(
            static_cast<UINT>(m_ShapeBuffer.size()), m_ShapeBuffer.data(), &required, &shape);
        if (SUCCEEDED(hr)) {
            decodeShape(shape, m_ShapeBuffer.data(), m_ShapeBuffer.size());
            changed = true;
        } else {
            log::warning("[native] could not read the cursor shape: " + hresultToString(hr));
        }
    }

    // LastMouseUpdateTime is zero when this frame carries no pointer news at
    // all, and the previous position stands.
    if (info.LastMouseUpdateTime.QuadPart != 0) {
        const bool wasVisible = m_Cursor.visible;
        const int oldX = m_Cursor.x;
        const int oldY = m_Cursor.y;

        m_Cursor.visible = info.PointerPosition.Visible != FALSE;
        // Position is given for the hotspot; the image starts above and left of
        // it. Drawing at the hotspot would offset every cursor by its own
        // shape — an arrow would look right and a crosshair would not.
        m_Cursor.x = info.PointerPosition.Position.x - m_CursorHotspotX;
        m_Cursor.y = info.PointerPosition.Position.y - m_CursorHotspotY;

        changed = changed || m_Cursor.visible != wasVisible ||
                  (m_Cursor.visible && (m_Cursor.x != oldX || m_Cursor.y != oldY));
    }

    return changed;
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

    const bool cursorMoved = updateCursor(info);

    // A present time of zero means DXGI woke us for a pointer change only: not
    // one desktop pixel moved, so there is no new texture to encode and no
    // honest present time to stamp on it.
    //
    // It is still not nothing. We composite the cursor ourselves, so the frame
    // the viewer sees HAS changed — and reporting this as a plain timeout is
    // exactly what left the cursor frozen on a quiet screen. The caller is told
    // which of the two it is and re-encodes from its own copy.
    if (info.LastPresentTime.QuadPart == 0) {
        release();
        return cursorMoved ? AcquireStatus::PointerOnly : AcquireStatus::Timeout;
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
