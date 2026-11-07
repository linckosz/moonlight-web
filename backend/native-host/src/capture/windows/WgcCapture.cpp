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
#include <windows.security.authorization.appcapabilityaccess.h>
#include <winstring.h>
#include <wrl/implements.h>
#include <wrl/wrappers/corewrappers.h>

#include <chrono>
#include <thread>

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

/// GraphicsCaptureAccess.RequestAccessAsync(Borderless), waited for here: the
/// capture thread has no message pump, and a desktop process has its answer
/// within milliseconds. Without this request, put_IsBorderRequired(false)
/// succeeds and is ignored — the border stays, and nothing says so (the
/// property's documentation). What it answered, for the log.
std::string requestBorderlessAccess()
{
    namespace Access = ABI::Windows::Security::Authorization::AppCapabilityAccess;
    ComPtr<WGC::IGraphicsCaptureAccessStatics> access;
    HRESULT hr = factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureAccess, access);
    if (FAILED(hr)) return "no GraphicsCaptureAccess on this Windows (" + hresultToString(hr) + ")";
    ComPtr<
        __FIAsyncOperation_1_Windows__CSecurity__CAuthorization__CAppCapabilityAccess__CAppCapabilityAccessStatus>
        operation;
    hr = access->RequestAccessAsync(WGC::GraphicsCaptureAccessKind_Borderless,
                                    operation.ReleaseAndGetAddressOf());
    if (FAILED(hr) || !operation)
        return "the borderless access request was refused (" + hresultToString(hr) + ")";
    ComPtr<ABI::Windows::Foundation::IAsyncInfo> info;
    if (FAILED(operation.As(&info))) return "the borderless access request has no status";
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    for (;;) {
        ABI::Windows::Foundation::AsyncStatus status =
            ABI::Windows::Foundation::AsyncStatus::Started;
        if (FAILED(info->get_Status(&status)))
            return "the borderless access request's status could not be read";
        if (status != ABI::Windows::Foundation::AsyncStatus::Started) break;
        if (std::chrono::steady_clock::now() > deadline)
            return "the borderless access request did not answer within 3 s";
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    Access::AppCapabilityAccessStatus result = Access::AppCapabilityAccessStatus_DeniedBySystem;
    hr = operation->GetResults(&result);
    if (FAILED(hr)) return "the borderless access request failed (" + hresultToString(hr) + ")";
    switch (result) {
    case Access::AppCapabilityAccessStatus_Allowed: return "borderless access allowed";
    case Access::AppCapabilityAccessStatus_DeniedBySystem:
        return "borderless access denied by the system";
    case Access::AppCapabilityAccessStatus_DeniedByUser:
        return "borderless access denied by the user";
    case Access::AppCapabilityAccessStatus_NotDeclaredByApp:
        return "borderless access not declared by the app";
    case Access::AppCapabilityAccessStatus_UserPromptRequired:
        return "borderless access would need a prompt";
    }
    return "borderless access status " + std::to_string(static_cast<int>(result));
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

void WgcCapture::useDevice(ID3D11Device* device)
{
    m_Given = device;
}

bool WgcCapture::available(std::string* why)
{
    ComPtr<WGC::IGraphicsCaptureSessionStatics> statics;
    const HRESULT made =
        factory(RuntimeClass_Windows_Graphics_Capture_GraphicsCaptureSession, statics);
    if (FAILED(made)) {
        if (why)
            *why = "the capture session's factory is unavailable (" + hresultToString(made) + ")";
        return false;
    }
    boolean supported = false;
    const HRESULT asked = statics->IsSupported(&supported);
    if (FAILED(asked)) {
        if (why) *why = "IsSupported failed (" + hresultToString(asked) + ")";
        return false;
    }
    if (!supported) {
        if (why)
            *why = "IsSupported says no (Windows 10 1903 at least, in a session with a compositor)";
        return false;
    }
    if (why) why->clear();
    return true;
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
    std::string why;
    if (!available(&why)) {
        error = "Windows.Graphics.Capture is not available on this system: " + why;
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

    if (m_Given) {
        // The caller's device — on this adapter, or the compositor would hand
        // frames to a GPU the chain does not read.
        ComPtr<IDXGIDevice> givenDxgi;
        ComPtr<IDXGIAdapter> givenAdapter;
        DXGI_ADAPTER_DESC givenDesc = {};
        if (FAILED(m_Given.As(&givenDxgi)) || FAILED(givenDxgi->GetAdapter(&givenAdapter)) ||
            FAILED(givenAdapter->GetDesc(&givenDesc))) {
            error = "the device given to the capture names no adapter";
            return false;
        }
        const uint64_t givenLuid = (static_cast<uint64_t>(givenDesc.AdapterLuid.HighPart) << 32) |
                                   static_cast<uint32_t>(givenDesc.AdapterLuid.LowPart);
        if (givenLuid != m_AdapterLuid) {
            error = "the device given to the capture is on another GPU than the display";
            return false;
        }
        m_Device = m_Given;
        m_Device->GetImmediateContext(m_Context.ReleaseAndGetAddressOf());
    } else {
        const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
        hr = ::D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                 D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, ARRAYSIZE(levels),
                                 D3D11_SDK_VERSION, m_Device.ReleaseAndGetAddressOf(), nullptr,
                                 m_Context.ReleaseAndGetAddressOf());
        if (FAILED(hr)) {
            error = "could not create a D3D11 device on that GPU (" + hresultToString(hr) + ")";
            return false;
        }
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

    // The yellow "you are being captured" border. Windows 11 lets a session off
    // it, once the capture access without a border has been asked for
    // (GraphicsCaptureAccess, 22H2; a desktop process is allowed without a
    // prompt): put_IsBorderRequired(false) alone succeeds and is ignored. On
    // Windows 10 neither exists and the border stays. Said in the log, read
    // back from the session, because a border nobody asked for is what a
    // viewer sees first.
    m_BorderOff = false;
    ComPtr<WGC::IGraphicsCaptureSession3> session3;
    if (SUCCEEDED(d->session.As(&session3))) {
        const std::string access = requestBorderlessAccess();
        session3->put_IsBorderRequired(false);
        boolean required = true;
        if (SUCCEEDED(session3->get_IsBorderRequired(&required))) m_BorderOff = !required;
        log::info(std::string("[native] WGC: capture border ") +
                  (m_BorderOff ? "off" : "required") + " (" + access + ")");
    } else {
        log::info("[native] WGC: this Windows build draws its capture border (IsBorderRequired "
                  "needs Windows 11)");
    }

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

bool WgcCapture::retake(std::string& error)
{
    if (!d->session || !m_Device) {
        error = "the capture is not open";
        return false;
    }
    const ComPtr<ID3D11Device> given = m_Given;
    const int width = m_Width;
    const int height = m_Height;
    m_Given = m_Device;
    const bool started = start(error);
    m_Given = given;
    if (!started) {
        m_RetakeLost = true;
        return false;
    }
    if (m_Width != width || m_Height != height) {
        error = "the display changed size meanwhile";
        m_RetakeLost = true;
        return false;
    }
    m_Retaking = true;
    return true;
}

AcquireStatus WgcCapture::acquire(int timeoutMs, CapturedFrame& frame)
{
    // Said by the caller already, with the reason retake() gave.
    if (m_RetakeLost) return AcquireStatus::Lost;
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
        if (cursorMoved) return AcquireStatus::PointerOnly;

        // Nothing waiting: sleep on the handler rather than poll. A frame that
        // lands during the wait wakes us within microseconds of the compositor
        // publishing it, which is the closest this API gets to DDA's "wake on
        // present".
        //
        // In slices, though, with the pointer asked after each one. Nothing
        // wakes this wait when only the pointer moves — the compositor has no
        // frame to publish, the pointer being left out of it — so one long
        // sleep read the pointer once per timeout: ten times a second on a
        // still desktop, which is what a viewer saw as a pointer lagging and
        // jumping (Windows on ARM, 12/09/2026). DDA has no such problem, a
        // pointer move wakes it like a present.
        constexpr auto kPointerPoll = std::chrono::milliseconds(4);
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(timeoutMs < 0 ? 0 : timeoutMs);
        bool arrived = false;
        for (;;) {
            const auto now = std::chrono::steady_clock::now();
            const std::chrono::steady_clock::duration left = deadline - now;
            const std::chrono::steady_clock::duration slice =
                left < kPointerPoll ? left : kPointerPoll;
            {
                std::unique_lock<std::mutex> lock(d->mutex);
                if (slice.count() > 0) d->cv.wait_for(lock, slice, [this]() { return d->arrived; });
                arrived = d->arrived;
                d->arrived = false;
            }
            if (arrived || std::chrono::steady_clock::now() >= deadline) break;
            if (m_Cursor2.update(m_Cursor, m_DesktopRect, m_Width, m_Height))
                return AcquireStatus::PointerOnly;
        }

        if (FAILED(d->pool->TryGetNextFrame(next.ReleaseAndGetAddressOf())))
            return AcquireStatus::Lost;
        if (!next) {
            const bool movedLast = m_Cursor2.update(m_Cursor, m_DesktopRect, m_Width, m_Height);
            return movedLast ? AcquireStatus::PointerOnly : AcquireStatus::Timeout;
        }
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
    //
    // Asked of the frame's ContentSize, which is the item's new size: the
    // texture itself is one of the pool's buffers, still at the old size, so
    // its desc alone never saw the change. A display turned from 90° to 180°
    // in a session stayed streamed as a portrait (issue #54, 07/11/2026).
    ABI::Windows::Graphics::SizeInt32 content = {};
    if (SUCCEEDED(next->get_ContentSize(&content)) && content.Width > 0 && content.Height > 0 &&
        (content.Width != m_Width || content.Height != m_Height))
        return AcquireStatus::Lost;
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
    frame.presentRawUs = frame.presentUs;

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
    frame.retaken = m_Retaking;
    m_Retaking = false;
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
    m_Retaking = false;
    m_RetakeLost = false;
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
