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

#include "WasapiLoopback.h"

#include "AppAudioCapture.h"

#include "../../core/Log.h"
#include "../AudioPacer.h"
#include "../OpusEncoder.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace mw::native::audio {

namespace {

using Microsoft::WRL::ComPtr;

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

std::string hresultString(HRESULT hr)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}

/// One open loopback client on the default render endpoint. Rebuilt whole
/// when the device goes away — there is nothing worth keeping across it.
struct LoopbackDevice
{
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> capture;
    HANDLE event = nullptr;
    /// Samples per channel the endpoint's buffer holds; only for the log.
    UINT32 bufferFrames = 0;

    ~LoopbackDevice()
    {
        if (client) client->Stop();
        if (event) CloseHandle(event);
    }

    bool open(std::string& error)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                      IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            error = "MMDeviceEnumerator: " + hresultString(hr);
            return false;
        }
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            error = "no default playback device (" + hresultString(hr) + ")";
            return false;
        }
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
        if (FAILED(hr)) {
            error = "IAudioClient: " + hresultString(hr);
            return false;
        }

        // The stream's own format. AUTOCONVERTPCM makes the engine convert
        // from the endpoint's mix format — rate and channel count included —
        // so what comes out of GetBuffer is exactly what the pacer expects.
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

        // 50 ms of endpoint buffer: room for a late wake-up without a glitch;
        // it is not latency — the pacer drains it as soon as it is written.
        const REFERENCE_TIME bufferDuration = 50 * 10000;
        const DWORD flags = AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                            AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                            AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, bufferDuration, 0,
                                reinterpret_cast<const WAVEFORMATEX*>(&fmt), nullptr);
        if (FAILED(hr)) {
            error =
                "IAudioClient::Initialize (loopback, 48 kHz stereo float): " + hresultString(hr);
            return false;
        }
        client->GetBufferSize(&bufferFrames);

        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) {
            error = "CreateEvent failed";
            return false;
        }
        hr = client->SetEventHandle(event);
        if (FAILED(hr)) {
            error = "SetEventHandle: " + hresultString(hr);
            return false;
        }
        hr = client->GetService(IID_PPV_ARGS(&capture));
        if (FAILED(hr)) {
            error = "IAudioCaptureClient: " + hresultString(hr);
            return false;
        }
        hr = client->Start();
        if (FAILED(hr)) {
            error = "IAudioClient::Start: " + hresultString(hr);
            return false;
        }
        return true;
    }

    /// The errors that mean "this endpoint is gone", as opposed to a hiccup.
    static bool lost(HRESULT hr)
    {
        return hr == AUDCLNT_E_DEVICE_INVALIDATED || hr == AUDCLNT_E_SERVICE_NOT_RUNNING ||
               hr == AUDCLNT_E_RESOURCES_INVALIDATED;
    }

    /// Move everything the endpoint has into the pacer. Returns false when the
    /// device is gone and must be reopened.
    bool drain(AudioPacer& pacer)
    {
        for (;;) {
            UINT32 packetFrames = 0;
            HRESULT hr = capture->GetNextPacketSize(&packetFrames);
            if (FAILED(hr)) return !lost(hr);
            if (packetFrames == 0) return true;

            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) return !lost(hr);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                pacer.pushSilence(frames);
            else
                pacer.push(reinterpret_cast<const float*>(data), frames);
            capture->ReleaseBuffer(frames);
        }
    }
};

} // namespace

WasapiLoopback::WasapiLoopback(AudioCallback onPacket, int frameSamples, int graceUs,
                               AppAudioOptions appAudio)
    : m_OnPacket(std::move(onPacket))
    , m_FrameSamples(AudioPacer::isFrameSamples(frameSamples) ? frameSamples
                                                              : AudioPacer::kFrameSamples)
    , m_GraceUs(std::max(0, graceUs))
    , m_AppAudio(std::move(appAudio))
{}

WasapiLoopback::~WasapiLoopback()
{
    stop();
}

bool WasapiLoopback::start(std::string& error)
{
    if (m_Running.load()) return true;
    if (!m_OnPacket) {
        error = "no audio callback";
        return false;
    }
    // The device is opened ON the audio thread (its COM apartment, its MMCSS
    // registration), so start() waits for that first attempt's verdict rather
    // than reporting a success it cannot vouch for.
    m_Running.store(true);
    std::promise<std::string> firstOpen;
    std::future<std::string> verdict = firstOpen.get_future();
    m_Thread = std::thread([this, p = std::move(firstOpen)]() mutable {
        m_FirstOpen = &p;
        run();
        // run() has resolved the promise by now, in every path.
    });
    error = verdict.get();
    if (!error.empty()) {
        stop();
        return false;
    }
    return true;
}

void WasapiLoopback::stop()
{
    m_Running.store(false);
    if (m_Thread.joinable()) {
        if (std::this_thread::get_id() == m_Thread.get_id())
            m_Thread.detach();
        else
            m_Thread.join();
    }
}

void WasapiLoopback::run() noexcept
{
    try {
        runLoop();
    } catch (const std::exception& e) {
        log::error(std::string("[native] audio thread threw: ") + e.what());
    } catch (...) {
        log::error("[native] audio thread threw");
    }
    // A throw before the first verdict must still release start().
    if (m_FirstOpen) {
        try {
            m_FirstOpen->set_value("audio thread failed before opening the device");
        } catch (...) {}
        m_FirstOpen = nullptr;
    }
    m_Running.store(false);
}

void WasapiLoopback::runLoop()
{
    const HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool coOwned = SUCCEEDED(coInit);

    auto resolveFirst = [this](const std::string& verdict) {
        if (!m_FirstOpen) return;
        try {
            m_FirstOpen->set_value(verdict);
        } catch (...) {}
        m_FirstOpen = nullptr;
    };

    OpusEncoder encoder;
    std::string error;
    if (!encoder.open(error, m_FrameSamples)) {
        resolveFirst(error);
        if (coOwned) CoUninitialize();
        return;
    }

    auto device = std::make_unique<LoopbackDevice>();
    if (!device->open(error)) {
        resolveFirst(error);
        if (coOwned) CoUninitialize();
        return;
    }
    const int frameMs = m_FrameSamples / (AudioPacer::kSampleRate / 1000);
    log::info("[native] audio: WASAPI loopback on the default output, 48 kHz stereo -> Opus " +
              std::to_string(frameMs) + " ms (" + std::string(OpusEncoder::libraryVersion()) +
              "), endpoint buffer " + std::to_string(device->bufferFrames) + " frames");
    resolveFirst(std::string());

    // "Stream audio from" other than All apps (AudioSource.h): the whole-mix
    // client gives way to one app's own (AppAudioCapture). Chosen once, at
    // start, and never both at once: nothing else can reach the stream
    // through the whole mix. The device above was still opened, so a host
    // with no playback device at all fails the same way in either mode.
    std::unique_ptr<AppAudioCapture> focused;
    if (m_AppAudio.choice.source != AudioSource::AllApps) {
        device.reset();
        focused = std::make_unique<AppAudioCapture>(m_AppAudio);
        log::info(m_AppAudio.choice.source == AudioSource::OneApp
                      ? "[native] audio: streaming " + m_AppAudio.choice.app + " only"
                      : std::string("[native] audio: streaming the app in focus only"));
    }

    // Pro Audio: the class the OS reserves for exactly this loop. Refused in
    // a service session, in which case the thread keeps an ordinary priority.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    if (mmcss)
        log::info("[native] audio thread on MMCSS Pro Audio");
    else
        log::info("[native] audio thread: MMCSS Pro Audio refused (error " +
                  std::to_string(GetLastError()) + ") — ordinary priority");

    // The 5 ms tick that keeps the wire fed while the endpoint is silent and
    // signals nothing. High-resolution when the OS has it (1803+): the default
    // timer would round the period up to the 15.6 ms scheduler tick.
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                          TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    if (timer) {
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(AudioPacer::kFramePeriodUs) * 10;
        SetWaitableTimer(timer, &due, static_cast<LONG>(AudioPacer::kFramePeriodUs / 1000), nullptr,
                         nullptr, FALSE);
    }

    // Four 5 ms frames of slack, 20 ms; longer frames (`audioframe=`) keep the
    // 20 ms, never fewer than two frames. The tick above stays at 5 ms either
    // way: it is how often the endpoint is drained, not the frame.
    AudioPacer pacer(std::max(2, 20 / frameMs), m_FrameSamples);
    if (m_GraceUs > 0) pacer.setUnderrunGraceUs(m_GraceUs);
    pacer.start(steadyNowUs());
    // What the pacer holds behind each frame it sends, summed over the minute:
    // the sound's wait in the host, which the grace trades against silence.
    int64_t heldSamples = 0, heldFrames = 0;
    std::vector<float> frame(pacer.frameFloats());
    std::vector<uint8_t> packet;
    int64_t lastReopenAttemptUs = 0;
    int64_t lastReportUs = steadyNowUs();
    int64_t reportedDropped = 0, reportedUnderruns = 0;
    bool deviceLost = false;

    while (m_Running.load(std::memory_order_acquire)) {
        // Wake on the endpoint's data, or on the 5 ms tick, whichever first.
        HANDLE handles[2] = {nullptr, nullptr};
        DWORD count = 0;
        if (device && device->event) handles[count++] = device->event;
        if (timer) handles[count++] = timer;
        if (count > 0)
            WaitForMultipleObjects(count, handles, FALSE,
                                   static_cast<DWORD>(AudioPacer::kFramePeriodUs / 1000));
        else
            Sleep(static_cast<DWORD>(AudioPacer::kFramePeriodUs / 1000));
        if (!m_Running.load(std::memory_order_acquire)) break;

        const int64_t now = steadyNowUs();

        if (focused) focused->pump(pacer, now);

        if (!focused && device && !device->drain(pacer)) {
            // The endpoint went away — unplugged, or the user picked another
            // default output. Reopen on whatever is the default now; the
            // pacer keeps the wire fed with silence meanwhile.
            log::warning("[native] audio: playback device lost — reopening on the default output");
            device.reset();
            deviceLost = true;
            lastReopenAttemptUs = now;
        }
        if (!focused && !device && now - lastReopenAttemptUs >= 1'000'000) {
            lastReopenAttemptUs = now;
            auto fresh = std::make_unique<LoopbackDevice>();
            std::string reopenError;
            if (fresh->open(reopenError)) {
                device = std::move(fresh);
                if (deviceLost) log::info("[native] audio: playback device is back");
                deviceLost = false;
            }
        }

        // With a grace (`audiograce=<ms>`), a frame owed while the loopback's
        // packet is a little late waits for it, that long at most: the next
        // wake, on the endpoint's event or the 5 ms tick, asks again. Without
        // it, silence goes out at once and the late packet, arriving on top
        // of a queue the clock no longer drains, costs a dropped frame later.
        const int due = pacer.dueFrames(now);
        for (int i = 0; i < due; ++i) {
            bool captured = true;
            if (m_GraceUs > 0) {
                const AudioPacer::Take got = pacer.take(frame.data(), now);
                if (got == AudioPacer::Take::Deferred) break;
                captured = got == AudioPacer::Take::Frame;
            } else {
                captured = pacer.pop(frame.data());
            }
            heldSamples += static_cast<int64_t>(pacer.queuedSamples());
            heldFrames++;
            const size_t n = encoder.encode(frame.data(), packet);
            if (n == 0) continue;
            AudioPacket out;
            out.data = packet.data();
            out.size = n;
            out.samplesPerChannel = m_FrameSamples;
            out.capturedUs = now;
            out.queuedFrames = static_cast<int>(pacer.queuedFrames());
            out.peak = AudioPacer::peakOf(frame.data(), frame.size());
            out.silence = !captured;
            m_OnPacket(out);
            m_Packets.fetch_add(1, std::memory_order_relaxed);
        }

        // Once a minute, and only when there is something to say: the two
        // counters that tell a saturated thread from a quiet host. A bench's
        // own grace (`audiograce=`) reports every minute, to be compared.
        if (now - lastReportUs >= 60'000'000) {
            lastReportUs = now;
            const int64_t dropped = pacer.droppedFrames();
            const int64_t underruns = pacer.underruns();
            if (dropped != reportedDropped || pacer.reanchors() > 0 ||
                m_GraceUs != kDefaultGraceUs) {
                // Held behind each frame sent, in tenths of a millisecond.
                const int64_t heldTenthsMs =
                    heldFrames > 0 ? heldSamples * 10'000 / (heldFrames * AudioPacer::kSampleRate)
                                   : 0;
                log::info("[native] audio: " + std::to_string(dropped - reportedDropped) +
                          " frames dropped (queue full), " +
                          std::to_string(underruns - reportedUnderruns) +
                          " sent as silence, in the last minute; " +
                          std::to_string(heldTenthsMs / 10) + "." +
                          std::to_string(heldTenthsMs % 10) + " ms held on average" +
                          (m_GraceUs > 0 ? " (grace " + std::to_string(m_GraceUs) +
                                               " us: " + std::to_string(pacer.deferrals()) +
                                               " waits for a late capture so far)"
                                         : std::string()));
            }
            reportedDropped = dropped;
            reportedUnderruns = underruns;
            heldSamples = heldFrames = 0;
        }
    }

    log::info("[native] audio: " + std::to_string(m_Packets.load()) + " packets, " +
              std::to_string(pacer.droppedFrames()) + " frames dropped, " +
              std::to_string(pacer.underruns()) + " silence frames (quiet host or late capture)");

    if (timer) CloseHandle(timer);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    focused.reset();
    device.reset();
    if (coOwned) CoUninitialize();
}

} // namespace mw::native::audio
