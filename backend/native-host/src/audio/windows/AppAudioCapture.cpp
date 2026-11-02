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

#include "AppAudioCapture.h"

#include "../../core/Log.h"
#include "../AudioPacer.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <audioclient.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <propidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <new>
#include <string>
#include <vector>

namespace mw::native::audio {

namespace {

using Microsoft::WRL::ComPtr;

// ── Process loopback activation, declared here ────────────────────────────────
// audioclientactivationparams.h exists only in recent SDKs, and behind an
// NTDDI guard in some of them. The activation blob is plain data whose layout
// Microsoft documents and cannot change, so it is spelled out here instead of
// depending on which SDK the runner happens to carry.
enum MwActivationType : int
{
    kActivationDefault = 0,
    kActivationProcessLoopback = 1,
};
enum MwLoopbackMode : int
{
    kLoopbackIncludeTree = 0,
    kLoopbackExcludeTree = 1,
};
struct MwProcessLoopbackParams
{
    DWORD targetProcessId;
    MwLoopbackMode mode;
};
struct MwActivationParams
{
    MwActivationType type;
    MwProcessLoopbackParams loopback;
};
constexpr wchar_t kProcessLoopbackDevice[] = L"VAD\\Process_Loopback";

std::string hresultString(HRESULT hr)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

std::string narrow(const wchar_t* w)
{
    if (!w || !*w) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return {};
    std::string out(static_cast<size_t>(n - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr);
    return out;
}

/// ActivateAudioInterfaceAsync's callback. It must be agile (IAgileObject):
/// the call refuses any other handler, since the audio service invokes it from
/// a thread of its own choosing. Hand-written rather than WRL so that nothing
/// beyond the plain COM headers is needed. The event is owned here, so a wait
/// that times out leaves nothing dangling when the late callback finally lands.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject
{
public:
    static ComPtr<ActivationHandler> create()
    {
        ComPtr<ActivationHandler> h;
        h.Attach(new (std::nothrow) ActivationHandler());
        return h;
    }

    // IUnknown — both bases derive from it; the handler's is the canonical one.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override
    {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown) ||
            riid == __uuidof(IActivateAudioInterfaceCompletionHandler))
            *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject))
            *out = static_cast<IAgileObject*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_Refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG left = --m_Refs;
        if (left == 0) delete this;
        return left;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override
    {
        HRESULT activateHr = E_FAIL;
        ComPtr<IUnknown> unk;
        const HRESULT hr = op->GetActivateResult(&activateHr, &unk);
        if (FAILED(hr))
            m_Result = hr;
        else if (FAILED(activateHr))
            m_Result = activateHr;
        else
            m_Result = unk.As(&m_Client);
        if (m_Done) SetEvent(m_Done);
        return S_OK;
    }

    HANDLE done() const { return m_Done; }
    HRESULT result() const { return m_Result; }
    ComPtr<IAudioClient> client() const { return m_Client; }

private:
    ActivationHandler() { m_Done = CreateEventW(nullptr, TRUE, FALSE, nullptr); }
    ~ActivationHandler()
    {
        if (m_Done) CloseHandle(m_Done);
    }

    std::atomic<ULONG> m_Refs{1};
    HANDLE m_Done = nullptr;
    // Written by the callback before SetEvent, read after the wait: the event
    // orders them.
    HRESULT m_Result = E_PENDING;
    ComPtr<IAudioClient> m_Client;
};

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Frames of 48 kHz stereo — the units the FIFO counts in.
constexpr size_t kChannels = static_cast<size_t>(AudioPacer::kChannels);
constexpr size_t framesFromMs(int ms)
{
    return static_cast<size_t>(AudioPacer::kSampleRate) * static_cast<size_t>(ms) / 1000;
}
/// The FIFO never grows past this; the oldest audio goes first.
constexpr size_t kMaxFifoFrames = framesFromMs(200);

} // namespace

/// One per-process loopback client and the FIFO the audio thread fills from it.
struct AppAudioCapture::Client
{
    DWORD pid = 0;
    std::string exe;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE event = nullptr;

    // Audio thread only: what drain() collected and pump() has not pushed yet.
    std::vector<float> fifo;
    /// Written by the audio thread, read by the scanner (which reopens it).
    std::atomic<bool> lost{false};

    ~Client()
    {
        if (client) client->Stop();
        if (event) CloseHandle(event);
    }

    /// Activate and start. Returns the failing HRESULT, S_OK on success.
    HRESULT open(std::string& step)
    {
        MwActivationParams params{};
        params.type = kActivationProcessLoopback;
        params.loopback.targetProcessId = pid;
        params.loopback.mode = kLoopbackIncludeTree;
        PROPVARIANT pv;
        PropVariantInit(&pv);
        pv.vt = VT_BLOB;
        pv.blob.cbSize = sizeof(params);
        pv.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

        ComPtr<ActivationHandler> handler = ActivationHandler::create();
        if (!handler || !handler->done()) {
            step = "activation handler";
            return E_OUTOFMEMORY;
        }
        ComPtr<IActivateAudioInterfaceAsyncOperation> op;
        step = "ActivateAudioInterfaceAsync";
        HRESULT hr = ActivateAudioInterfaceAsync(kProcessLoopbackDevice, __uuidof(IAudioClient),
                                                 &pv, handler.Get(), &op);
        if (FAILED(hr)) return hr;
        if (WaitForSingleObject(handler->done(), 2000) != WAIT_OBJECT_0) {
            step = "activation (timed out)";
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        step = "activation";
        if (FAILED(handler->result())) return handler->result();
        client = handler->client();
        if (!client) return E_NOINTERFACE;

        // The stream's own format, as the whole-mix loopback asks it. Process
        // loopback has no mix format of its own (GetMixFormat is not
        // implemented on it) and converts to whatever is asked.
        WAVEFORMATEXTENSIBLE fmt{};
        fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
        fmt.Format.nChannels = static_cast<WORD>(AudioPacer::kChannels);
        fmt.Format.nSamplesPerSec = static_cast<DWORD>(AudioPacer::kSampleRate);
        fmt.Format.wBitsPerSample = 32;
        fmt.Format.nBlockAlign =
            static_cast<WORD>(fmt.Format.nChannels * fmt.Format.wBitsPerSample / 8);
        fmt.Format.nAvgBytesPerSec = fmt.Format.nSamplesPerSec * fmt.Format.nBlockAlign;
        fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
        fmt.Samples.wValidBitsPerSample = 32;
        fmt.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
        fmt.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;

        const REFERENCE_TIME bufferDuration = 50 * 10000;
        step = "IAudioClient::Initialize";
        hr = client->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
            bufferDuration, 0, reinterpret_cast<const WAVEFORMATEX*>(&fmt), nullptr);
        if (FAILED(hr)) {
            // Microsoft's own sample passes neither conversion flag; a client
            // built where they are refused gets a second chance without them.
            client.Reset();
            ComPtr<ActivationHandler> again = ActivationHandler::create();
            ComPtr<IActivateAudioInterfaceAsyncOperation> op2;
            if (!again || !again->done() ||
                FAILED(ActivateAudioInterfaceAsync(kProcessLoopbackDevice, __uuidof(IAudioClient),
                                                   &pv, again.Get(), &op2)) ||
                WaitForSingleObject(again->done(), 2000) != WAIT_OBJECT_0 ||
                FAILED(again->result()) || !again->client())
                return hr;
            client = again->client();
            const HRESULT hr2 = client->Initialize(
                AUDCLNT_SHAREMODE_SHARED,
                AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK, bufferDuration, 0,
                reinterpret_cast<const WAVEFORMATEX*>(&fmt), nullptr);
            if (FAILED(hr2)) return hr2;
        }

        // Event-driven as Microsoft's sample is, though nobody waits on it:
        // the audio thread polls the client on its own 5 ms tick.
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            step = "CreateEvent";
            return HRESULT_FROM_WIN32(GetLastError());
        }
        step = "SetEventHandle";
        hr = client->SetEventHandle(event);
        if (FAILED(hr)) return hr;
        step = "IAudioCaptureClient";
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) return hr;
        step = "IAudioClient::Start";
        return client->Start();
    }

    /// Audio thread: move what the client has into the FIFO.
    void drain()
    {
        if (lost || !capture) return;
        for (;;) {
            UINT32 packetFrames = 0;
            HRESULT hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) {
                lost = true;
                return;
            }
            if (packetFrames == 0) return;
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) {
                lost = true;
                return;
            }
            const size_t floats = static_cast<size_t>(frames) * kChannels;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                fifo.insert(fifo.end(), floats, 0.0f);
            else {
                const float* f = reinterpret_cast<const float*>(data);
                fifo.insert(fifo.end(), f, f + floats);
            }
            capture->ReleaseBuffer(frames);
        }
    }
};

namespace {

/// The executable's file name, lower case ("discord.exe"); empty if unreadable.
std::string exeName(HANDLE process)
{
    std::array<wchar_t, MAX_PATH> path{};
    DWORD size = static_cast<DWORD>(path.size());
    if (!process || !QueryFullProcessImageNameW(process, 0, path.data(), &size)) return {};
    std::string full = narrow(path.data());
    const size_t slash = full.find_last_of("\\/");
    std::string name = slash == std::string::npos ? full : full.substr(slash + 1);
    for (char& ch : name)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return name;
}

std::string exeName(DWORD pid)
{
    HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return {};
    std::string name = exeName(h);
    CloseHandle(h);
    return name;
}

/// Windows' own surfaces: in front often (the taskbar, Start, the lock screen,
/// alt-tab), never what anyone means by "the game". Focus moving to one of
/// them keeps the current target.
bool isShell(const std::string& exe)
{
    static const char* const kShell[] = {
        "explorer.exe",
        "shellexperiencehost.exe",
        "startmenuexperiencehost.exe",
        "searchhost.exe",
        "searchapp.exe",
        "lockapp.exe",
        "textinputhost.exe",
        "applicationframehost.exe",
        "logonui.exe",
        "shellhost.exe",
    };
    for (const char* s : kShell)
        if (exe == s) return true;
    return false;
}

/// A UWP app's window belongs to ApplicationFrameHost; the app itself owns a
/// child window. Its PID, or 0 when there is none.
DWORD uwpChildProcess(HWND frame, DWORD framePid)
{
    struct Search
    {
        DWORD framePid;
        DWORD found;
    } search{framePid, 0};
    EnumChildWindows(
        frame,
        [](HWND child, LPARAM param) -> BOOL {
            auto* s = reinterpret_cast<Search*>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(child, &pid);
            if (pid != 0 && pid != s->framePid) {
                s->found = pid;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

/// The window covers its whole monitor with no title bar: exclusive or
/// borderless full screen. A maximized window keeps its caption, so it is not.
bool isFullscreen(HWND window)
{
    if (IsIconic(window)) return false;
    const LONG_PTR style = GetWindowLongPtrW(window, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION) return false;
    RECT rect{};
    if (!GetWindowRect(window, &rect)) return false;
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONULL);
    if (!monitor) return false;
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return false;
    return rect.left <= info.rcMonitor.left && rect.top <= info.rcMonitor.top &&
           rect.right >= info.rcMonitor.right && rect.bottom >= info.rcMonitor.bottom;
}

/// What is in front now.
FocusSample sampleForeground(const std::string& selfExe)
{
    FocusSample out;
    HWND window = GetForegroundWindow();
    if (!window) return out;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0) return out;
    std::string exe = exeName(pid);
    if (exe == "applicationframehost.exe") {
        if (const DWORD app = uwpChildProcess(window, pid)) {
            pid = app;
            exe = exeName(pid);
        }
    }
    out.pid = pid;
    out.fullscreen = isFullscreen(window);
    // This host itself (the stream worker, the server's own window) is never
    // the target either: it plays no sound worth streaming.
    out.ignored =
        pid == GetCurrentProcessId() || isShell(exe) || (!selfExe.empty() && exe == selfExe);
    return out;
}

bool processAlive(HANDLE process)
{
    return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

} // namespace

AppAudioCapture::AppAudioCapture()
{
    m_Scanner = std::thread([this] { scannerLoop(); });
}

AppAudioCapture::~AppAudioCapture()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Stop = true;
    }
    m_Cv.notify_all();
    if (m_Scanner.joinable()) m_Scanner.join();
    if (m_TargetProcess) CloseHandle(static_cast<HANDLE>(m_TargetProcess));
}

void AppAudioCapture::pump(AudioPacer& pacer, int64_t /*nowUs*/)
{
    std::shared_ptr<Client> client;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        client = m_Client;
    }
    // No target: nothing is pushed, and the pacer keeps the wire fed with
    // silence — never the whole mix.
    if (!client) return;

    client->drain();
    size_t frames = client->fifo.size() / kChannels;
    if (frames == 0) return;
    // A backlog (the audio thread stalled) loses its oldest audio.
    const float* src = client->fifo.data();
    if (frames > kMaxFifoFrames) {
        src += (frames - kMaxFifoFrames) * kChannels;
        frames = kMaxFifoFrames;
    }
    pacer.push(src, frames);
    client->fifo.clear();
}

void AppAudioCapture::scannerLoop() noexcept
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coOwned = SUCCEEDED(coInit);
    try {
        for (;;) {
            scanOnce();
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Cv.wait_for(lock, std::chrono::milliseconds(500), [this] { return m_Stop; });
            if (m_Stop) break;
        }
    } catch (const std::exception& e) {
        log::error(std::string("[native] audio: focused-app scanner threw: ") + e.what());
    } catch (...) {
        log::error("[native] audio: focused-app scanner threw");
    }
    {
        // The client is a COM object created on this thread's apartment (MTA,
        // shared with the audio thread): drop it before leaving it.
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Client.reset();
    }
    if (coOwned) CoUninitialize();
}

void AppAudioCapture::scanOnce()
{
    static const std::string selfExe = exeName(GetCurrentProcess());

    HANDLE targetProcess = static_cast<HANDLE>(m_TargetProcess);
    const FocusTarget next =
        nextFocusTarget(m_Target, processAlive(targetProcess), sampleForeground(selfExe));

    std::shared_ptr<Client> current;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        current = m_Client;
    }

    const bool newTarget = next.pid != m_Target.pid;
    m_Target = next;
    if (newTarget) {
        if (targetProcess) CloseHandle(targetProcess);
        targetProcess =
            next.pid ? OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, next.pid)
                     : nullptr;
        m_TargetProcess = targetProcess;
        m_RetryPid = 0;
        // Silence while the new client comes up: the old app must not keep
        // playing into the stream once something else is in front.
        if (current) {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Client.reset();
            current.reset();
        }
        if (next.pid == 0) log::info("[native] audio: focused app closed — the stream is silent");
    }

    if (next.pid == 0) return;
    if (current && !current->lost) return; // capturing it already

    // A client to (re)open. A PID that failed is not retried for 5 s.
    const int64_t now = steadyNowUs();
    if (m_RetryPid == next.pid && now < m_RetryAfterUs) return;

    auto client = std::make_shared<Client>();
    client->pid = next.pid;
    client->exe = exeName(targetProcess);
    if (client->exe.empty()) client->exe = "pid " + std::to_string(next.pid);
    std::string step;
    const HRESULT hr = client->open(step);
    if (FAILED(hr)) {
        m_RetryPid = next.pid;
        m_RetryAfterUs = now + 5'000'000;
        const bool unsupported = hr == E_NOTIMPL || hr == E_INVALIDARG ||
                                 hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ||
                                 hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
        if (unsupported && !m_ReportedUnsupported) {
            m_ReportedUnsupported = true;
            log::error("[native] audio: per-app capture is not available on this Windows (" + step +
                       " " + hresultString(hr) +
                       ") — Windows 11 is required. The stream stays SILENT; set "
                       "Settings → Stream audio from → All apps to stream the whole mix.");
        } else if (!unsupported) {
            log::warning("[native] audio: could not capture " + client->exe + ": " + step + " " +
                         hresultString(hr));
        }
        if (current) {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Client.reset();
        }
        return;
    }
    m_RetryPid = 0;
    log::info("[native] audio: streaming the focused app, " + client->exe + " (pid " +
              std::to_string(next.pid) + (next.fullscreen ? ", full screen" : "") + ")");
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Client = std::move(client);
}

} // namespace mw::native::audio
