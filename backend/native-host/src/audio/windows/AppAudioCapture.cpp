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
#include <tlhelp32.h>
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
#include <utility>
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

/// Every process on this machine, for finding a picked app by its executable.
std::vector<ProcessEntry> processSnapshot()
{
    std::vector<ProcessEntry> out;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more;
         more = Process32NextW(snapshot, &entry)) {
        std::string exe = narrow(entry.szExeFile);
        for (char& ch : exe)
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        out.push_back({entry.th32ProcessID, entry.th32ParentProcessID, std::move(exe)});
    }
    CloseHandle(snapshot);
    return out;
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

/// The window is on the display being streamed: the monitor that holds most
/// of it is that display. True when the streamed display is not known.
bool onStreamedScreen(HWND window, const std::string& streamed)
{
    if (streamed.empty()) return true;
    HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    if (!monitor) return true;
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return true;
    return _stricmp(narrow(info.szDevice).c_str(), streamed.c_str()) == 0;
}

/// False only when the process is known to have ended. Without a handle that
/// cannot be told (OpenProcess can be refused on a protected game): the target
/// stays, rather than being taken for closed the moment focus moves elsewhere.
bool processMayBeAlive(HANDLE process)
{
    return !process || WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
}

} // namespace

FocusSample AppAudioCapture::sampleForeground(const std::string& selfExe)
{
    constexpr char kUwpFrame[] = "applicationframehost.exe";
    FocusSample out;
    HWND window = GetForegroundWindow();
    if (!window) return out;
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == 0) return out;
    // Asked again only for another window, or a UWP frame whose app was not
    // there yet: these polls run ten times a second.
    const bool newWindow = window != static_cast<HWND>(m_FrontWindow) || owner != m_FrontOwner;
    if (newWindow) {
        m_FrontWindow = window;
        m_FrontOwner = owner;
        m_FrontPid = owner;
        m_FrontExe = exeName(owner);
    }
    if (m_FrontExe == kUwpFrame) {
        if (const DWORD app = uwpChildProcess(window, owner)) {
            m_FrontPid = app;
            m_FrontExe = exeName(app);
        }
    }
    const std::string streamed = m_Options.display ? m_Options.display->get() : std::string();
    out.pid = m_FrontPid;
    out.onStreamedScreen = onStreamedScreen(window, streamed);
    // This host itself (the stream worker, the server's own window) is never
    // the target either: it plays no sound worth streaming.
    out.ignored = m_FrontPid == GetCurrentProcessId() || isWindowsShellExe(m_FrontExe) ||
                  (!selfExe.empty() && m_FrontExe == selfExe);
    return out;
}

void AppAudioCapture::publishApp(const std::string& exe)
{
    if (exe == m_PublishedApp) return;
    m_PublishedApp = exe;
    if (m_Options.onAppChanged) m_Options.onAppChanged(exe);
}

AppAudioCapture::AppAudioCapture(AppAudioOptions options)
    : m_Options(std::move(options))
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
    publishApp(std::string());
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
    // The foreground window can change at any moment; a picked app only starts
    // or exits, and finding it walks every process.
    const auto pollEvery = m_Options.choice.source == AudioSource::OneApp
                               ? std::chrono::milliseconds(1000)
                               : std::chrono::milliseconds(100);
    try {
        for (;;) {
            scanOnce();
            std::unique_lock<std::mutex> lock(m_Mutex);
            m_Cv.wait_for(lock, pollEvery, [this] { return m_Stop; });
            if (m_Stop) break;
        }
    } catch (const std::exception& e) {
        log::error(std::string("[native] audio: per-app scanner threw: ") + e.what());
    } catch (...) {
        log::error("[native] audio: per-app scanner threw");
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
    const bool alive = processMayBeAlive(targetProcess);
    FocusTarget next = m_Target;
    if (m_Options.choice.source == AudioSource::OneApp) {
        // Without a handle, only the process list can tell it has gone.
        if (next.pid == 0 || !alive || !targetProcess)
            next.pid = pickAppRoot(processSnapshot(), m_Options.choice.app);
    } else {
        next = nextFocusTarget(m_Target, alive, sampleForeground(selfExe));
    }

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
        if (next.pid && !targetProcess)
            log::warning("[native] audio: no handle on pid " + std::to_string(next.pid) +
                         ": its exit cannot be seen");
        // Shown in Settings ("Automatic — huntgame.exe") whether or not its
        // capture opens: it is what the host considers in front.
        publishApp(next.pid ? exeName(next.pid) : std::string());
        // Silence while the new client comes up: the old app must not keep
        // playing into the stream once something else is in front.
        if (current) {
            std::lock_guard<std::mutex> lock(m_Mutex);
            m_Client.reset();
            current.reset();
        }
        if (next.pid == 0)
            log::info("[native] audio: the app closed or is not running — the stream is silent");
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
                       "Settings → Stream audio from → All applications to stream everything.");
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
    log::info("[native] audio: streaming one app, " + client->exe + " (pid " +
              std::to_string(next.pid) + ")");
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Client = std::move(client);
}

} // namespace mw::native::audio
