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
#include "../AppTreeSelect.h"
#include "../AudioPacer.h"
#include "mw/native/AudioAppFilter.h"

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
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <new>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

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

/// Frames of 48 kHz stereo — the units every FIFO below counts in.
constexpr size_t kChannels = static_cast<size_t>(AudioPacer::kChannels);
constexpr size_t framesFromMs(int ms)
{
    return static_cast<size_t>(AudioPacer::kSampleRate) * static_cast<size_t>(ms) / 1000;
}
/// A client that delivered nothing for this long is idle: it no longer holds
/// the mix back waiting for its next packet.
constexpr int64_t kIdleUs = 60'000;
/// One client this far ahead of the slowest active one means that one is
/// stuck, not late: mix what there is and pad the laggard with silence.
constexpr size_t kResyncFrames = framesFromMs(40);
/// No FIFO grows past this; the oldest audio goes first.
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

    // Audio thread only.
    std::vector<float> fifo;
    size_t readFrames = 0;
    int64_t lastDataUs = 0;
    /// Written by the audio thread, read by the scanner (which retires it).
    std::atomic<bool> lost{false};

    ~Client()
    {
        if (client) client->Stop();
        if (event) CloseHandle(event);
    }

    size_t queuedFrames() const { return fifo.size() / kChannels - readFrames; }

    void compact()
    {
        if (readFrames == 0) return;
        fifo.erase(fifo.begin(), fifo.begin() + static_cast<ptrdiff_t>(readFrames * kChannels));
        readFrames = 0;
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
        // the audio thread polls every client on its own 5 ms tick.
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
    void drain(int64_t nowUs)
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
            lastDataUs = nowUs;
        }
    }
};

AppAudioCapture::AppAudioCapture()
    : m_Clients(std::make_shared<const std::vector<std::shared_ptr<Client>>>())
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
}

void AppAudioCapture::wake()
{
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Wake = true;
    }
    m_Cv.notify_all();
}

int AppAudioCapture::clientCount() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return static_cast<int>(m_Clients->size());
}

void AppAudioCapture::pump(AudioPacer& pacer, int64_t nowUs)
{
    std::shared_ptr<const std::vector<std::shared_ptr<Client>>> clients;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        clients = m_Clients;
    }
    if (clients->empty()) return;

    for (const auto& c : *clients) {
        c->drain(nowUs);
        // A runaway FIFO (a client the mix could not keep up with) loses its
        // oldest audio rather than growing without bound.
        const size_t queued = c->queuedFrames();
        if (queued > kMaxFifoFrames) c->readFrames += queued - kMaxFifoFrames;
    }

    // How much to mix now. Every process loopback stream is cut from the same
    // engine pass, so active clients deliver in step: mix what the slowest
    // active one has, which keeps them aligned without a clock of our own. An
    // idle client (nothing for 60 ms — its app is quiet or gone) does not hold
    // the others back, and one stuck far behind is padded rather than waited for.
    size_t minActive = SIZE_MAX, maxAny = 0;
    bool anyActive = false;
    for (const auto& c : *clients) {
        const size_t q = c->queuedFrames();
        maxAny = std::max(maxAny, q);
        if (nowUs - c->lastDataUs < kIdleUs && !c->lost) {
            anyActive = true;
            minActive = std::min(minActive, q);
        }
    }
    size_t frames = 0;
    if (!anyActive)
        frames = maxAny; // only leftovers: flush them
    else if (maxAny > minActive + kResyncFrames)
        frames = maxAny;
    else
        frames = minActive;
    if (frames == 0) return;

    m_Mix.assign(frames * kChannels, 0.0f);
    for (const auto& c : *clients) {
        const size_t take = std::min(frames, c->queuedFrames());
        const float* src = c->fifo.data() + c->readFrames * kChannels;
        for (size_t i = 0; i < take * kChannels; ++i)
            m_Mix[i] += src[i];
        c->readFrames += take;
        if (c->readFrames * kChannels * 2 > c->fifo.size() || c->readFrames > framesFromMs(100))
            c->compact();
    }
    // The system mixer clips the sum the same way; nothing subtler is needed
    // for a handful of apps.
    for (float& s : m_Mix)
        s = std::clamp(s, -1.0f, 1.0f);
    pacer.push(m_Mix.data(), frames);

    if (nowUs - m_LastReportUs >= 60'000'000) {
        m_LastReportUs = nowUs;
        std::string names;
        for (const auto& c : *clients)
            names += (names.empty() ? "" : ", ") + c->exe;
        log::info("[native] audio: per-app capture of " + std::to_string(clients->size()) +
                  " app(s): " + names);
    }
}

void AppAudioCapture::scannerLoop() noexcept
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coOwned = SUCCEEDED(coInit);
    try {
        for (;;) {
            {
                std::unique_lock<std::mutex> lock(m_Mutex);
                m_Cv.wait_for(lock, std::chrono::milliseconds(750),
                              [this] { return m_Stop || m_Wake; });
                if (m_Stop) break;
                m_Wake = false;
            }
            scanOnce();
        }
    } catch (const std::exception& e) {
        log::error(std::string("[native] audio: per-app scanner threw: ") + e.what());
    } catch (...) {
        log::error("[native] audio: per-app scanner threw");
    }
    {
        // The clients are COM objects created on this thread's apartment (MTA,
        // shared with the audio thread): drop them before leaving it.
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Clients = std::make_shared<const std::vector<std::shared_ptr<Client>>>();
    }
    if (coOwned) CoUninitialize();
}

namespace {

struct ProcInfo
{
    DWORD parent = 0;
    std::string exe; // normalised (AudioAppFilter::normalise)
};

/// Every process, with its parent and normalised executable name.
std::unordered_map<DWORD, ProcInfo> snapshotProcesses()
{
    std::unordered_map<DWORD, ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            ProcInfo info;
            info.parent = pe.th32ParentProcessID;
            info.exe = AudioAppFilter::normalise(narrow(pe.szExeFile));
            out.emplace(pe.th32ProcessID, std::move(info));
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

/// When a process started, for telling a real parent from a recycled PID.
/// 0 when it cannot be read.
uint64_t creationTime(DWORD pid, std::unordered_map<DWORD, uint64_t>& cache)
{
    auto it = cache.find(pid);
    if (it != cache.end()) return it->second;
    uint64_t t = 0;
    if (HANDLE h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        FILETIME created, exited, kernel, user;
        if (GetProcessTimes(h, &created, &exited, &kernel, &user))
            t = (static_cast<uint64_t>(created.dwHighDateTime) << 32) | created.dwLowDateTime;
        CloseHandle(h);
    }
    cache.emplace(pid, t);
    return t;
}

/// The chain of real ancestors of `pid`, nearest first. A parent PID that has
/// been reused by a younger process is where the chain stops.
std::vector<DWORD> ancestors(DWORD pid, const std::unordered_map<DWORD, ProcInfo>& procs,
                             std::unordered_map<DWORD, uint64_t>& times)
{
    std::vector<DWORD> out;
    DWORD cur = pid;
    for (int depth = 0; depth < 64; ++depth) {
        auto it = procs.find(cur);
        if (it == procs.end()) break;
        const DWORD parent = it->second.parent;
        if (parent == 0 || parent == cur || procs.find(parent) == procs.end()) break;
        const uint64_t childT = creationTime(cur, times);
        const uint64_t parentT = creationTime(parent, times);
        if (childT && parentT && parentT > childT) break; // recycled PID
        if (std::find(out.begin(), out.end(), parent) != out.end()) break;
        out.push_back(parent);
        cur = parent;
    }
    return out;
}

/// The processes that own an audio session on any active render endpoint.
std::set<DWORD> sessionOwners()
{
    std::set<DWORD> pids;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&enumerator))))
        return pids;
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &devices))) return pids;
    UINT count = 0;
    devices->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        if (FAILED(devices->Item(i, &device))) continue;
        ComPtr<IAudioSessionManager2> manager;
        if (FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr,
                                    reinterpret_cast<void**>(manager.GetAddressOf()))))
            continue;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager->GetSessionEnumerator(&sessions))) continue;
        int n = 0;
        sessions->GetCount(&n);
        for (int s = 0; s < n; ++s) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(s, &control))) continue;
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control.As(&control2))) continue;
            if (control2->IsSystemSoundsSession() == S_OK) continue;
            AudioSessionState state = AudioSessionStateInactive;
            if (SUCCEEDED(control2->GetState(&state)) && state == AudioSessionStateExpired)
                continue;
            DWORD pid = 0;
            if (SUCCEEDED(control2->GetProcessId(&pid)) && pid != 0) pids.insert(pid);
        }
    }
    return pids;
}

} // namespace

void AppAudioCapture::scanOnce()
{
    const std::vector<std::string> excludedList = AudioAppFilter::get();
    const std::unordered_set<std::string> excluded(excludedList.begin(), excludedList.end());

    const auto procs = snapshotProcesses();
    std::unordered_map<DWORD, uint64_t> times;
    const DWORD self = GetCurrentProcessId();

    // Every running process of an excluded app, session or not: one launched
    // by an app we capture must not be swept in through that app's tree.
    std::vector<DWORD> excludedPids;
    for (const auto& [pid, info] : procs)
        if (excluded.count(info.exe)) excludedPids.push_back(pid);

    // Session owners that may be heard.
    std::set<DWORD> candidates;
    for (DWORD pid : sessionOwners()) {
        auto it = procs.find(pid);
        if (it == procs.end() || pid == self) continue;
        if (excluded.count(it->second.exe)) continue;
        candidates.insert(pid);
    }

    // Which trees to open: no owner with an excluded app under it, no owner
    // already carried by its ancestor's tree (AppTreeSelect.h).
    std::set<uint32_t> candidateIds(candidates.begin(), candidates.end());
    std::vector<uint32_t> excludedIds(excludedPids.begin(), excludedPids.end());
    const TreeChoice choice = chooseCaptureTrees(candidateIds, excludedIds, [&](uint32_t pid) {
        const std::vector<DWORD> chain = ancestors(static_cast<DWORD>(pid), procs, times);
        return std::vector<uint32_t>(chain.begin(), chain.end());
    });
    const std::set<uint32_t>& wanted = choice.wanted;

    // Report each owner we leave out on purpose, once per run.
    static std::set<std::string> s_reported;
    for (uint32_t u : choice.unsafe) {
        const std::string& exe = procs.at(u).exe;
        if (s_reported.insert(exe).second)
            log::info("[native] audio: not capturing " + exe +
                      " — an excluded app runs under it, and its tree would carry that app too");
    }

    std::shared_ptr<const std::vector<std::shared_ptr<Client>>> current;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        current = m_Clients;
    }

    auto next = std::make_shared<std::vector<std::shared_ptr<Client>>>();
    std::set<DWORD> have;
    bool changed = false;
    for (const auto& c : *current) {
        // Keep a client that is still wanted, still the same process, and alive.
        auto it = procs.find(c->pid);
        if (wanted.count(c->pid) && it != procs.end() && it->second.exe == c->exe && !c->lost) {
            next->push_back(c);
            have.insert(c->pid);
        } else {
            changed = true;
        }
    }

    // New owners. A PID that failed is not retried for 5 s; the first failure
    // that says the API itself is missing is reported loudly, once.
    static std::unordered_map<DWORD, int64_t> s_retryAfterUs;
    static bool s_reportedUnsupported = false;
    const int64_t now = steadyNowUs();
    for (uint32_t wantedPid : wanted) {
        const DWORD pid = static_cast<DWORD>(wantedPid);
        if (have.count(pid)) continue;
        auto retry = s_retryAfterUs.find(pid);
        if (retry != s_retryAfterUs.end() && now < retry->second) continue;
        auto client = std::make_shared<Client>();
        client->pid = pid;
        client->exe = procs.at(pid).exe;
        // Idle until its first packet: a new app must not hold the mix back
        // while its client warms up.
        client->lastDataUs = 0;
        std::string step;
        const HRESULT hr = client->open(step);
        if (FAILED(hr)) {
            s_retryAfterUs[pid] = now + 5'000'000;
            const bool unsupported = hr == E_NOTIMPL || hr == E_INVALIDARG ||
                                     hr == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED) ||
                                     hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
            if (unsupported && !s_reportedUnsupported) {
                s_reportedUnsupported = true;
                log::error("[native] audio: per-app capture is not available on this Windows (" +
                           step + " " + hresultString(hr) +
                           ") — Windows 11 is required. The stream stays SILENT while apps are "
                           "excluded; clear the exclusions to stream the whole mix.");
            } else {
                log::warning("[native] audio: could not capture " + client->exe + " (pid " +
                             std::to_string(pid) + "): " + step + " " + hresultString(hr));
            }
            continue;
        }
        s_retryAfterUs.erase(pid);
        log::info("[native] audio: capturing " + client->exe + " (pid " + std::to_string(pid) +
                  ")");
        next->push_back(std::move(client));
        changed = true;
    }

    if (changed) {
        std::lock_guard<std::mutex> lock(m_Mutex);
        m_Clients = std::move(next);
    }
}

} // namespace mw::native::audio
