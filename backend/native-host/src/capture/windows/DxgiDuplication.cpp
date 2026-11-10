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
#include "../../core/PresentLattice.h"
#include "CursorShape.h"
#include "DisplayRotation.h"

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
    // A refusal is taken to pass unless it is one of those that stay.
    m_RefusalMayPass = true;

    LARGE_INTEGER frequency = {};
    if (!::QueryPerformanceFrequency(&frequency) || frequency.QuadPart == 0) {
        error = "no high-resolution timer on this machine";
        m_RefusalMayPass = false;
        return false;
    }
    m_QpcFrequency = frequency.QuadPart;
    // A new duplication is a new question: the display, or its driver, may not
    // be the one that painted the pointer in last time.
    m_PaintedPointer = PaintedPointer();

    ComPtr<IDXGIAdapter1> adapter;
    if (!openAdapterAndOutput(adapter, m_Output, error)) return false;

    // D3D_DRIVER_TYPE_UNKNOWN is required when an adapter is supplied — asking
    // for HARDWARE here would silently ignore the adapter and pick the default,
    // which on a multi-GPU machine is how a "zero-copy" pipeline quietly starts
    // copying across GPUs.
    const D3D_FEATURE_LEVEL wanted[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained = {};
    // VIDEO_SUPPORT is here for the encoder that will share this device: Intel's
    // oneVPL runtime asks it for an ID3D11VideoDevice, which a device created
    // without the flag does not have. It costs nothing on a GPU that ignores it.
    HRESULT hr = ::D3D11CreateDevice(
        adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
        D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT, wanted,
        static_cast<UINT>(std::size(wanted)), D3D11_SDK_VERSION, m_Device.ReleaseAndGetAddressOf(),
        &obtained, m_Context.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error = "could not create a D3D11 device on that GPU (" + hresultToString(hr) + ")";
        return false;
    }

    if (!duplicate(error)) return false;

    DXGI_OUTDUPL_DESC duplDesc = {};
    m_Duplication->GetDesc(&duplDesc);

    m_Width = static_cast<int>(duplDesc.ModeDesc.Width);
    m_Height = static_cast<int>(duplDesc.ModeDesc.Height);
    // The mode's refresh as a rational, for the lattice the stream's gate
    // locks on: 239.757 Hz where the session's own figure, after "Match my
    // screen", says 239.
    const DXGI_RATIONAL rate = duplDesc.ModeDesc.RefreshRate;
    m_RefreshPeriodNs = PresentLattice::periodNsFromRational(rate.Numerator, rate.Denominator);

    // ⚠️ ModeDesc.Format is the DISPLAY MODE's format, not the duplication's
    // output format. The two agree only because the list above never makes
    // DXGI convert: with FP16 named first it keeps the desktop's own format,
    // which is what ModeDesc reports, and the plain DuplicateOutput fallback
    // converts nothing either. Name a single 8-bit format here and this line
    // becomes a lie — ModeDesc goes on saying FP16 while BGRA8 arrives, the
    // converter builds an FP16 view over an 8-bit texture, and the session dies
    // at its first frame with "could not view the captured frame" (found
    // 04/09/2026, on every SDR stream from a machine with Windows HDR on).
    m_Format = duplDesc.ModeDesc.Format;

    // Where this display sits on the desktop, for aiming absolute mouse input.
    // Kept exactly as DXGI reports it — see DesktopRect on why the DPI
    // virtualization is wanted here and nowhere else.
    DXGI_OUTPUT_DESC outputDesc = {};
    if (SUCCEEDED(m_Output->GetDesc(&outputDesc))) {
        m_DesktopRect.left = static_cast<int>(outputDesc.DesktopCoordinates.left);
        m_DesktopRect.top = static_cast<int>(outputDesc.DesktopCoordinates.top);
        m_DesktopRect.right = static_cast<int>(outputDesc.DesktopCoordinates.right);
        m_DesktopRect.bottom = static_cast<int>(outputDesc.DesktopCoordinates.bottom);
    }

    // A desktop turned against its panel comes back as the panel scans it
    // out, on its side or upside down, and turning it is left to the caller
    // (issue #54): the converter turns it upright as it reads it. Everything
    // else goes by the desktop as it stands — so the mode is stood that way
    // too, and the turn kept for the converter (DisplayRotation.h).
    m_Turn = quarterTurns(static_cast<int>(duplDesc.Rotation));
    duplicationUprightSize(m_Width, m_Height, static_cast<int>(duplDesc.Rotation),
                           m_DesktopRect.width(), m_DesktopRect.height());
    m_TurnChecked = m_Turn == 0;

    // Calibrate the two clocks against each other, as close together as
    // possible — see qpcToMicroseconds.
    LARGE_INTEGER qpcNow = {};
    ::QueryPerformanceCounter(&qpcNow);
    m_QpcOrigin = qpcNow.QuadPart;
    m_SteadyOriginUs = steadyNowUs();

    char hz[48] = "";
    if (m_RefreshPeriodNs > 0)
        std::snprintf(hz, sizeof(hz), ", mode at %.3f Hz",
                      1e9 / static_cast<double>(m_RefreshPeriodNs));
    log::info("[native] duplication started: " + std::to_string(m_Width) + "x" +
              std::to_string(m_Height) +
              (m_Format == DXGI_FORMAT_R16G16B16A16_FLOAT ? " (HDR, FP16)"
               : m_Format == DXGI_FORMAT_B8G8R8A8_UNORM
                   ? " (SDR, BGRA8)"
                   : " (format " + std::to_string(static_cast<int>(m_Format)) + ")") +
              hz +
              (m_Turn != 0 ? ", the display turned " + std::to_string(m_Turn * 90) +
                                 "° against its panel: its pictures are turned upright in the "
                                 "conversion"
                           : ""));
    return true;
}

bool DxgiDuplication::duplicate(std::string& error)
{
    // DuplicateOutput1 lets the formats be named, and the list decides what an
    // HDR desktop becomes. Listing the float format ahead of the 8-bit one
    // keeps the desktop as it is: FP16 scRGB when it is HDR, BGRA8 when it is
    // not. Naming 8-bit alone would make DXGI hand an HDR desktop over "tone-
    // mapped" — clipped at 80 nits, in fact, which is what an SDR stream of a
    // desktop with the SDR brightness slider up looked like until 16/09/2026:
    // blown out. The converter takes scRGB on both kinds of session now (an
    // SDR one tone-maps it, see ColorConvert::init), so this list is the only
    // one there is. Machines and drivers without Output5 fall back to the
    // plain path.
    HRESULT hr = E_NOINTERFACE;
    ComPtr<IDXGIOutput5> output5;
    if (SUCCEEDED(m_Output.As(&output5))) {
        const DXGI_FORMAT formats[] = {DXGI_FORMAT_R16G16B16A16_FLOAT, DXGI_FORMAT_B8G8R8A8_UNORM};
        hr = output5->DuplicateOutput1(m_Device.Get(), 0, static_cast<UINT>(std::size(formats)),
                                       formats, m_Duplication.ReleaseAndGetAddressOf());
    }

    if (FAILED(hr)) {
        ComPtr<IDXGIOutput1> output1;
        if (FAILED(m_Output.As(&output1))) {
            error = "this display does not support Desktop Duplication";
            m_RefusalMayPass = false;
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
        m_RefusalMayPass = hr != DXGI_ERROR_UNSUPPORTED;
        return false;
    }
    m_RefusalMayPass = false;
    return true;
}

bool DxgiDuplication::retake(std::string& error)
{
    if (!m_Duplication || !m_Output) {
        error = "the duplication is not open";
        return false;
    }
    // Let go of the old one before asking for the new: DXGI refuses a second
    // duplication of an output this process already duplicates. The pointer,
    // the hidden-pointer override and PaintedPointer's verdict stay — the
    // display and its driver are the ones they were learned on.
    release();
    m_Duplication.Reset();
    if (!duplicate(error)) {
        m_RetakeLost = true;
        return false;
    }
    DXGI_OUTDUPL_DESC desc = {};
    m_Duplication->GetDesc(&desc);
    // Stood as start() stands it, so that a turned display compares upright.
    int width = static_cast<int>(desc.ModeDesc.Width);
    int height = static_cast<int>(desc.ModeDesc.Height);
    duplicationUprightSize(width, height, static_cast<int>(desc.Rotation), m_DesktopRect.width(),
                           m_DesktopRect.height());
    if (width != m_Width || height != m_Height || desc.ModeDesc.Format != m_Format) {
        error = "the display changed mode meanwhile";
        m_RetakeLost = true;
        return false;
    }
    // A half turn keeps the size: only the rotation says the picture would now
    // arrive upside down, to a converter built for the turn start() read.
    if (quarterTurns(static_cast<int>(desc.Rotation)) != m_Turn) {
        error = "the display was turned meanwhile";
        m_RetakeLost = true;
        return false;
    }
    m_Retaking = true;
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

void DxgiDuplication::samplePointerForVerdict()
{
    if (!m_PaintedPointer.undecided()) return;
    CURSORINFO info = {};
    info.cbSize = sizeof(info);
    if (!::GetCursorInfo(&info)) return;
    // The same test Win32Cursor makes: showing, and on THIS display. A pointer
    // moving on another monitor says nothing about this duplication.
    const bool onDisplay =
        (info.flags & CURSOR_SHOWING) != 0 && m_DesktopRect.valid() &&
        info.ptScreenPos.x >= m_DesktopRect.left && info.ptScreenPos.x < m_DesktopRect.right &&
        info.ptScreenPos.y >= m_DesktopRect.top && info.ptScreenPos.y < m_DesktopRect.bottom;
    m_PaintedPointer.noteSample(onDisplay, info.ptScreenPos.x, info.ptScreenPos.y);
    if (m_PaintedPointer.paintedIn())
        log::info("[native] Desktop Duplication paints the pointer into the picture on this "
                  "display (no hardware pointer from the driver) — it can be neither left to the "
                  "client nor magnified");
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
        // Even a shape that fails to read proves the pointer is kept apart.
        m_PaintedPointer.noteReported();
        if (SUCCEEDED(hr)) {
            decodeShape(shape, m_ShapeBuffer.data(), m_ShapeBuffer.size());
            changed = true;
        } else {
            log::warning("[native] could not read the cursor shape: " + hresultToString(hr));
        }
    }

    // LastMouseUpdateTime is zero when this frame carries no pointer news at
    // all, and the previous position stands — unless Windows is being asked
    // instead (below), in which case every frame is a chance to catch up.
    const bool news = info.LastMouseUpdateTime.QuadPart != 0;
    if (news || m_HiddenOverridden) {
        const bool wasVisible = m_Cursor.visible;
        const bool wasInImage = m_Cursor.inImage;
        const int oldX = m_Cursor.x;
        const int oldY = m_Cursor.y;

        // Where the pointer's image goes. Desktop Duplication gives its top-left
        // corner already, not the hotspot: subtracting the hotspot again put
        // every pointer that is not an arrow above and left of its place in the
        // picture, by its own hotspot — the text I-beam, the resize arrows
        // (N95, 19/10/2026, game mode). Microsoft's DesktopDuplication sample
        // draws the shape at this position as it is.
        bool visible = news && info.PointerPosition.Visible != FALSE;
        int px = news ? static_cast<int>(info.PointerPosition.Position.x) : m_Cursor.x;
        int py = news ? static_cast<int>(info.PointerPosition.Position.y) : m_Cursor.y;

        // Desktop Duplication reports the pointer HIDDEN, at (0, 0), for a
        // pointer Windows is still showing: seen on Windows 11 for the whole
        // of a title-bar drag, from half a second after the left button goes
        // down until it goes up (17/09/2026, from an iPhone). A client drawing
        // the pointer itself was told "hidden, at the corner" and, steering
        // from there on the next finger move, dragged the window into the
        // corner. Windows' own word settles it, the way Win32Cursor reads it
        // for WGC: showing and on this display means visible, at the place it
        // says — scaled from the DPI-virtualized desktop into captured pixels.
        //
        // And "hidden while Windows shows it" is not an error to paper over: it
        // is what Desktop Duplication says when the picture itself carries the
        // pointer. So the pointer is visible AND already in the image — see
        // CursorState::inImage, which is what keeps the drawing from happening
        // twice, here or on the client.
        if (!visible) {
            CURSORINFO ci = {};
            ci.cbSize = sizeof(ci);
            const bool showing =
                ::GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING) != 0 && m_DesktopRect.valid() &&
                ci.ptScreenPos.x >= m_DesktopRect.left && ci.ptScreenPos.x < m_DesktopRect.right &&
                ci.ptScreenPos.y >= m_DesktopRect.top && ci.ptScreenPos.y < m_DesktopRect.bottom;
            if (showing) {
                const int rw = m_DesktopRect.width();
                const int rh = m_DesktopRect.height();
                const double sx = rw > 0 && m_Width > 0 ? static_cast<double>(m_Width) / rw : 1.0;
                const double sy = rh > 0 && m_Height > 0 ? static_cast<double>(m_Height) / rh : 1.0;
                // Windows gives the hotspot, as for WGC (Win32Cursor).
                const int hx = static_cast<int>((ci.ptScreenPos.x - m_DesktopRect.left) * sx);
                const int hy = static_cast<int>((ci.ptScreenPos.y - m_DesktopRect.top) * sy);
                px = hx - m_CursorHotspotX;
                py = hy - m_CursorHotspotY;
                visible = true;
                if (!m_HiddenOverridden && !m_HiddenOverrideLogged) {
                    m_HiddenOverrideLogged = true;
                    log::info("[native] cursor: Desktop Duplication reports the pointer hidden "
                              "while Windows shows it at " +
                              std::to_string(hx) + "," + std::to_string(hy) +
                              " — Windows' word kept (once per session)");
                }
            }
            m_HiddenOverridden = showing;
        } else {
            m_HiddenOverridden = false;
        }

        m_Cursor.visible = visible;
        m_Cursor.inImage = m_HiddenOverridden;
        m_Cursor.x = px;
        m_Cursor.y = py;

        changed = changed || m_Cursor.visible != wasVisible || m_Cursor.inImage != wasInImage ||
                  (m_Cursor.visible && (m_Cursor.x != oldX || m_Cursor.y != oldY));
    }

    return changed;
}

AcquireStatus DxgiDuplication::acquire(int timeoutMs, CapturedFrame& frame)
{
    // Said by the caller already, with the reason retake() gave.
    if (m_RetakeLost) return AcquireStatus::Lost;
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
    // An HDR switch — on this display, or on another one the same GPU drives —
    // does not always answer ACCESS_LOST: the duplication can simply become
    // invalid, and every call on it then says so. Measured 15/09/2026 on the
    // reference bench, three runs of display-follow.ps1 in four, each ending the
    // session. A duplication that has delivered is recovered like any other
    // lost one; one that never has is not, or a call that can never succeed
    // would restart the capture for ever.
    if (hr == DXGI_ERROR_INVALID_CALL && m_Delivered) {
        log::info("[native] duplication invalidated (a display switched HDR or mode) — will "
                  "restart");
        return AcquireStatus::Lost;
    }
    if (FAILED(hr)) {
        log::warning("[native] AcquireNextFrame failed: " + hresultToString(hr));
        return AcquireStatus::Failed;
    }

    m_FrameHeld = true;
    m_Delivered = true;

    const bool cursorMoved = updateCursor(info);
    samplePointerForVerdict();

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

    // The first picture of a turned display, looked at once: the size the
    // turn swaps is the one Microsoft's sample, Sunshine and OBS all take it
    // to arrive at. Said when it does not, so that a driver handing the
    // picture over otherwise is found in the log rather than guessed at.
    if (!m_TurnChecked) {
        m_TurnChecked = true;
        D3D11_TEXTURE2D_DESC desc = {};
        m_AcquiredTexture->GetDesc(&desc);
        const bool odd = (m_Turn & 1) != 0;
        const int expectedWidth = odd ? m_Height : m_Width;
        const int expectedHeight = odd ? m_Width : m_Height;
        if (static_cast<int>(desc.Width) != expectedWidth ||
            static_cast<int>(desc.Height) != expectedHeight)
            log::warning("[native] the duplication of a display turned " +
                         std::to_string(m_Turn * 90) + "° hands its pictures over at " +
                         std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + ", not " +
                         std::to_string(expectedWidth) + "x" + std::to_string(expectedHeight) +
                         " — the stream may come stretched or turned");
    }

    // AccumulatedFrames counts every present folded into this one: one is
    // the frame itself, anything above it was presented and never acquired.
    if (info.AccumulatedFrames > 1) m_FoldedPresents += info.AccumulatedFrames - 1;

    frame.texture = m_AcquiredTexture.Get();
    frame.presentUs = qpcToMicroseconds(info.LastPresentTime.QuadPart);
    frame.capturedUs = steadyNowUs();
    frame.presentRawUs = frame.presentUs;
    frame.mouseUs = info.LastMouseUpdateTime.QuadPart != 0
                        ? qpcToMicroseconds(info.LastMouseUpdateTime.QuadPart)
                        : 0;
    frame.accumulated = static_cast<int>(info.AccumulatedFrames);
    frame.retaken = m_Retaking;
    m_Retaking = false;
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
    m_Output.Reset();
    m_Retaking = false;
    m_RetakeLost = false;
    m_Delivered = false;
    m_Context.Reset();
    m_Device.Reset();
    m_Width = 0;
    m_Height = 0;
    m_Turn = 0;
    m_TurnChecked = true;
    m_RefreshPeriodNs = 0;
}

} // namespace mw::native::capture
