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

#include "WgcCapture.h"

#include "../../core/Log.h"

#include <dxgi1_2.h>
#include <roapi.h>
#include <windows.foundation.h>
#include <windows.graphics.capture.h>
#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winstring.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <chrono>

namespace WGC = ABI::Windows::Graphics::Capture;
namespace WGD = ABI::Windows::Graphics::DirectX;
namespace WGDD = ABI::Windows::Graphics::DirectX::Direct3D11;

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
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08lX", static_cast<unsigned long>(hr));
    return buffer;
}

/// RoInitialize once per process, never undone.
///
/// Undoing it is what would be wrong: the frame pool and the capture item are
/// apartment-bound objects that may outlive any one session, and a matching
/// RoUninitialize on a session teardown would pull the runtime out from under a
/// second session starting on another thread. The cost of leaving it is one
/// reference for the life of a process that is already using the runtime.
bool ensureWinRt()
{
    static const bool ok = []() {
        const HRESULT hr = ::RoInitialize(RO_INIT_MULTITHREADED);
        // Already initialised — by Qt, by a shell extension, by anything — is a
        // success for our purposes. Only a hard failure is not.
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE || hr == S_FALSE;
    }();
    return ok;
}

/// Activation factory for a runtime class, by its canonical name.
template <typename T> HRESULT factory(const wchar_t* name, ComPtr<T>& out)
{
    if (!ensureWinRt()) return E_FAIL;
    Microsoft::WRL::Wrappers::HStringReference ref(name);
    return ::RoGetActivationFactory(ref.Get(), IID_PPV_ARGS(out.ReleaseAndGetAddressOf()));
}

/// The handler the frame pool calls when a frame lands.
///
/// A plain COM object rather than a C++/WinRT lambda: this engine links no WinRT
/// projection, and the projection's delegates are exception-based. All this has
/// to do is wake the capture thread — the frame itself is taken by whoever wakes
/// up, so nothing is held here.
/// FtmBase is not optional: the pool is free-threaded, so it invokes this from
/// whichever apartment the compositor's thread is in. Without the free-threaded
/// marshaller the delegate cannot be handed across, and add_FrameArrived
/// refuses the subscription outright rather than failing later at the call.
class FrameArrivedHandler
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>,
          ABI::Windows::Foundation::ITypedEventHandler<WGC::Direct3D11CaptureFramePool*,
                                                       IInspectable*>,
          Microsoft::WRL::FtmBase>
{
public:
    FrameArrivedHandler(std::mutex& mutex, std::condition_variable& cv, bool& flag)
        : m_Mutex(mutex)
        , m_Cv(cv)
        , m_Flag(flag)
    {}

    HRESULT STDMETHODCALLTYPE Invoke(WGC::IDirect3D11CaptureFramePool*, IInspectable*) override
    {
        {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Flag = true;
        }
        m_Cv.notify_all();
        return S_OK;
    }

private:
    std::mutex& m_Mutex;
    std::condition_variable& m_Cv;
    bool& m_Flag;
};

} // namespace

/// Everything that would drag WinRT headers into WgcCapture.h.
struct WgcCapture::Impl
{
    ComPtr<WGC::IGraphicsCaptureItem> item;
    ComPtr<WGC::IDirect3D11CaptureFramePool> pool;
    ComPtr<WGC::IGraphicsCaptureSession> session;
    ComPtr<WGDD::IDirect3DDevice> rtDevice;
    ComPtr<WGC::IDirect3D11CaptureFrame> frame;
    ComPtr<ABI::Windows::Foundation::ITypedEventHandler<WGC::Direct3D11CaptureFramePool*,
                                                        IInspectable*>>
        handler;
    EventRegistrationToken token = {};

    std::mutex mutex;
    std::condition_variable cv;
    bool arrived = false;
};

WgcCapture::WgcCapture(uint64_t adapterLuid, unsigned outputIndex)
    : m_AdapterLuid(adapterLuid)
    , m_OutputIndex(outputIndex)
    , d(std::make_unique<Impl>())
{}

WgcCapture::~WgcCapture()
{
    stop();
}

bool WgcCapture::available()
{
    ComPtr<WGC::IGraphicsCaptureSessionStatics> statics;
    if (FAILED(factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession, statics)))
        return false;
    boolean supported = false;
    return SUCCEEDED(statics->IsSupported(&supported)) && supported;
}

int64_t WgcCapture::qpcToMicroseconds(int64_t qpc) const
{
    if (m_QpcFrequency <= 0) return steadyNowUs();
    // Split before scaling. `qpc * 1000000` is a 64-bit overflow on a machine
    // that has been up for a few months — the counter is uptime × frequency,
    // and multiplying it by a million pushes it past int64 well within the life
    // of a desktop. The quotient/remainder form costs one extra division and
    // cannot overflow at all.
    const int64_t delta = qpc - m_QpcOrigin;
    const int64_t whole = delta / m_QpcFrequency;
    const int64_t part = delta % m_QpcFrequency;
    return m_SteadyOriginUs + whole * 1000000 + (part * 1000000) / m_QpcFrequency;
}

bool WgcCapture::start(std::string& error)
{
    stop();

    if (!ensureWinRt()) {
        error = "the Windows Runtime could not be initialized";
        return false;
    }
    if (!available()) {
        error = "Windows.Graphics.Capture is not available on this system";
        return false;
    }

    // ── The D3D11 device, on the adapter we were asked for ──────────────────
    //
    // Unlike Desktop Duplication this need not be the adapter that scans the
    // display out — the compositor delivers the frames wherever we ask. That is
    // the entire reason this backend exists.
    ComPtr<IDXGIFactory1> dxgi;
    HRESULT hr = ::CreateDXGIFactory1(IID_PPV_ARGS(dxgi.ReleaseAndGetAddressOf()));
    if (FAILED(hr)) {
        error = "could not create a DXGI factory (" + hresultToString(hr) + ")";
        return false;
    }

    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i = 0;
         dxgi->EnumAdapters1(i, adapter.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
        DXGI_ADAPTER_DESC1 desc = {};
        if (FAILED(adapter->GetDesc1(&desc))) continue;
        const uint64_t luid = (static_cast<uint64_t>(desc.AdapterLuid.HighPart) << 32) |
                              static_cast<uint32_t>(desc.AdapterLuid.LowPart);
        if (luid == m_AdapterLuid) break;
        adapter.Reset();
    }
    if (!adapter) {
        error = "the GPU this display belongs to is no longer present";
        return false;
    }

    // The monitor handle, taken from the same adapter/output pair Desktop
    // Duplication was asked for. DXGI still ENUMERATES an output it refuses to
    // duplicate — a hybrid laptop's panel is listed on the adapter it is wired
    // to and duplication of it fails all the same — so this lookup works
    // precisely in the case that brought us here.
    ComPtr<IDXGIOutput> output;
    if (FAILED(adapter->EnumOutputs(m_OutputIndex, output.ReleaseAndGetAddressOf())) || !output) {
        error = "that display is no longer attached to this GPU";
        return false;
    }
    DXGI_OUTPUT_DESC outputDesc = {};
    if (FAILED(output->GetDesc(&outputDesc)) || !outputDesc.Monitor) {
        error = "that display has no monitor handle";
        return false;
    }
    m_Monitor = outputDesc.Monitor;

    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    hr = ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                             D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                             D3D11_SDK_VERSION, m_Device.ReleaseAndGetAddressOf(), nullptr,
                             m_Context.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
        error = "could not create a D3D11 device on that GPU (" + hresultToString(hr) + ")";
        return false;
    }

    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(m_Device.As(&dxgiDevice))) {
        error = "the D3D11 device has no DXGI interface";
        return false;
    }
    ComPtr<IInspectable> inspectable;
    hr = ::CreateDirect3D11DeviceFromDXGIDevice(dxgiDevice.Get(),
                                                inspectable.ReleaseAndGetAddressOf());
    if (FAILED(hr) || FAILED(inspectable.As(&d->rtDevice))) {
        error =
            "could not hand the D3D11 device to the capture runtime (" + hresultToString(hr) + ")";
        return false;
    }

    // ── The capture item: this monitor ──────────────────────────────────────
    ComPtr<IGraphicsCaptureItemInterop> interop;
    if (FAILED(factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureItem, interop))) {
        error = "the capture-item factory is unavailable";
        return false;
    }
    hr = interop->CreateForMonitor(m_Monitor, IID_PPV_ARGS(d->item.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !d->item) {
        error = "this display cannot be captured (" + hresultToString(hr) + ")";
        return false;
    }

    ABI::Windows::Graphics::SizeInt32 size = {};
    if (FAILED(d->item->get_Size(&size)) || size.Width <= 0 || size.Height <= 0) {
        error = "the display reported no size";
        return false;
    }
    m_Width = size.Width;
    m_Height = size.Height;
    m_Format = DXGI_FORMAT_B8G8R8A8_UNORM;

    // ── The frame pool ──────────────────────────────────────────────────────
    //
    // Free-threaded: the alternative binds delivery to a DispatcherQueue, i.e.
    // to a thread with a message pump, and the capture thread has neither and
    // must not grow one.
    //
    // Two buffers, not more. The pool is a queue, and a queue is latency: a
    // deeper one would let the compositor run ahead of the encoder and every
    // frame we then pulled would already be old. Two is the minimum that lets
    // one frame be filled while the other is being read.
    ComPtr<WGC::IDirect3D11CaptureFramePoolStatics2> poolStatics;
    if (FAILED(factory(RuntimeClass_Windows_Graphics_Capture_Direct3D11CaptureFramePool,
                       poolStatics))) {
        error = "the frame-pool factory is unavailable";
        return false;
    }
    hr = poolStatics->CreateFreeThreaded(d->rtDevice.Get(),
                                         WGD::DirectXPixelFormat_B8G8R8A8UIntNormalized, 2, size,
                                         d->pool.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !d->pool) {
        error = "could not create the capture frame pool (" + hresultToString(hr) + ")";
        return false;
    }

    d->handler = Microsoft::WRL::Make<FrameArrivedHandler>(d->mutex, d->cv, d->arrived);
    if (FAILED(d->pool->add_FrameArrived(d->handler.Get(), &d->token))) {
        error = "could not subscribe to captured frames";
        return false;
    }

    hr = d->pool->CreateCaptureSession(d->item.Get(), d->session.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !d->session) {
        error = "could not create the capture session (" + hresultToString(hr) + ")";
        return false;
    }

    // The pointer is ours to draw: it has to stay separable from the desktop so
    // a client can magnify it, or draw its own instead. Older builds have no
    // such property and composite it regardless — hence the fallback note in
    // the log rather than a failure.
    ComPtr<WGC::IGraphicsCaptureSession2> session2;
    if (SUCCEEDED(d->session.As(&session2))) {
        session2->put_IsCursorCaptureEnabled(false);
    } else {
        log::info("[native] WGC: this Windows build composites the pointer into the picture — "
                  "client-drawn and magnified pointers are unavailable on this display");
    }

    // The yellow "you are being captured" border. Windows 11 lets it be turned
    // off; on Windows 10 it is not offered and the border stays.
    ComPtr<WGC::IGraphicsCaptureSession3> session3;
    if (SUCCEEDED(d->session.As(&session3))) session3->put_IsBorderRequired(false);

    if (FAILED(d->session->StartCapture())) {
        error = "the capture session refused to start";
        return false;
    }

    MONITORINFO monitorInfo = {};
    monitorInfo.cbSize = sizeof(monitorInfo);
    if (::GetMonitorInfoW(m_Monitor, &monitorInfo)) {
        m_DesktopRect.left = monitorInfo.rcMonitor.left;
        m_DesktopRect.top = monitorInfo.rcMonitor.top;
        m_DesktopRect.right = monitorInfo.rcMonitor.right;
        m_DesktopRect.bottom = monitorInfo.rcMonitor.bottom;
    }

    LARGE_INTEGER frequency = {};
    ::QueryPerformanceFrequency(&frequency);
    m_QpcFrequency = frequency.QuadPart;
    LARGE_INTEGER now = {};
    ::QueryPerformanceCounter(&now);
    m_QpcOrigin = now.QuadPart;
    m_SteadyOriginUs = steadyNowUs();

    log::info("[native] Windows.Graphics.Capture started: " + std::to_string(m_Width) + "x" +
              std::to_string(m_Height) + " (SDR, BGRA8)");
    return true;
}

AcquireStatus WgcCapture::acquire(int timeoutMs, CapturedFrame& frame)
{
    if (!d->pool || !d->session) return AcquireStatus::Failed;
    if (m_Holding) {
        // The contract is one release() per Ok, and breaking it here would leak
        // a pool buffer per frame rather than fail loudly.
        release();
    }

    // The pointer first, and unconditionally: it moves without the desktop
    // changing a pixel, and a capture that only looked at it when a frame
    // arrived would leave the cursor frozen on a still screen — the same bug
    // PointerOnly exists to avoid on the Desktop Duplication path.
    const bool cursorMoved = m_Cursor2.update(m_Cursor, m_DesktopRect, m_Width, m_Height);

    ComPtr<WGC::IDirect3D11CaptureFrame> next;
    if (FAILED(d->pool->TryGetNextFrame(next.ReleaseAndGetAddressOf()))) return AcquireStatus::Lost;

    if (!next) {
        // Nothing waiting: sleep on the handler rather than poll. A frame that
        // lands during the wait wakes us within microseconds of the compositor
        // publishing it, which is the closest this API gets to DDA's "wake on
        // present".
        std::unique_lock<std::mutex> lock(d->mutex);
        d->cv.wait_for(lock, std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs),
                       [this]() { return d->arrived; });
        d->arrived = false;
        lock.unlock();

        if (FAILED(d->pool->TryGetNextFrame(next.ReleaseAndGetAddressOf())))
            return AcquireStatus::Lost;
        if (!next) return cursorMoved ? AcquireStatus::PointerOnly : AcquireStatus::Timeout;
    }

    ComPtr<WGDD::IDirect3DSurface> surface;
    if (FAILED(next->get_Surface(surface.ReleaseAndGetAddressOf())) || !surface)
        return AcquireStatus::Lost;

    ComPtr<Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess> access;
    if (FAILED(surface.As(&access))) return AcquireStatus::Lost;

    ComPtr<ID3D11Texture2D> texture;
    if (FAILED(access->GetInterface(IID_PPV_ARGS(texture.ReleaseAndGetAddressOf())))) {
        return AcquireStatus::Lost;
    }

    // A mode change resizes the item, and the pool keeps handing out the old
    // size until it is recreated. Report it as Lost: the session's restart path
    // rebuilds capture, converter and encoder together, which is exactly what a
    // resolution change needs.
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    if (static_cast<int>(desc.Width) != m_Width || static_cast<int>(desc.Height) != m_Height)
        return AcquireStatus::Lost;

    ABI::Windows::Foundation::TimeSpan when = {};
    next->get_SystemRelativeTime(&when);

    d->frame = next;
    m_Holding = true;

    frame.texture = texture.Get();
    // SystemRelativeTime is the QPC value expressed in 100 ns units, on the
    // same counter the origin was taken from — so this is a real present time,
    // not the moment we noticed it. Every latency figure downstream is measured
    // from here, which is why it is not simply "now".
    //
    // Converted to QPC ticks by quotient and remainder, never by
    // `duration * frequency`: that product overflows int64 after a few weeks of
    // uptime, and the symptom is a present time in the FUTURE — a negative
    // capture latency, which reads as a broken clock rather than as arithmetic.
    const int64_t hundredNs = when.Duration;
    const int64_t qpc = (hundredNs / 10000000) * m_QpcFrequency +
                        ((hundredNs % 10000000) * m_QpcFrequency) / 10000000;
    frame.capturedUs = steadyNowUs();
    frame.presentUs = qpcToMicroseconds(qpc);

    // ⚠️ Clamped, and this is not defensive coding — it is a real difference
    // between the two backends.
    //
    // Desktop Duplication reports LastPresentTime: the moment the frame WAS put
    // on the display, always in the past. WGC's SystemRelativeTime is the
    // compositor's own stamp and leads it — measured here at up to ~9 ms ahead
    // of the moment the frame is pulled, which is the presentation it was
    // scheduled for rather than one that has happened.
    //
    // Left alone, that is a NEGATIVE capture latency, and it does not stay a
    // cosmetic oddity: the link governor derives the client's one-way delay
    // from (arrival − present), so a present in the future inflates the rise it
    // reads and has the host cut its own bitrate on a link that is fine.
    //
    // So the stamp is capped at "now". Capture latency on this path is
    // therefore a floor rather than a measurement — which is worth saying out
    // loud, and is one more reason DDA stays the primary.
    if (frame.presentUs > frame.capturedUs) frame.presentUs = frame.capturedUs;
    return AcquireStatus::Ok;
}

void WgcCapture::release()
{
    // Releasing the frame is what returns its buffer to the pool. Holding one
    // costs the pool a buffer, and holding both stalls the compositor.
    d->frame.Reset();
    m_Holding = false;
}

void WgcCapture::stop()
{
    release();
    if (d->pool && d->token.value != 0) {
        d->pool->remove_FrameArrived(d->token);
        d->token = {};
    }
    // Order matters: the session feeds the pool, and the pool holds the device.
    if (d->session) {
        ComPtr<ABI::Windows::Foundation::IClosable> closable;
        if (SUCCEEDED(d->session.As(&closable))) closable->Close();
        d->session.Reset();
    }
    if (d->pool) {
        ComPtr<ABI::Windows::Foundation::IClosable> closable;
        if (SUCCEEDED(d->pool.As(&closable))) closable->Close();
        d->pool.Reset();
    }
    d->handler.Reset();
    d->item.Reset();
    d->rtDevice.Reset();
    m_Context.Reset();
    m_Device.Reset();
    m_Width = 0;
    m_Height = 0;
}

} // namespace mw::native::capture
