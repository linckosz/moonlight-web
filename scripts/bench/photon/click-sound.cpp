// MoonlightWeb — browser-based Sunshine/GameStream client.
// Copyright (C) 2026 Bruno Martin <brunoocto@gmail.com>
// GPLv3 — see repository LICENSE.
//
// mw-click-sound: the client's half of the click → sound bench, without a
// camera or a microphone (plan « le son et la priorité des paquets », A1).
//
// The host plays a short beep when an injected click reaches it, the same
// instant its latency flag goes up (MW_LATENCY_FLAG_SOUND=click on the host;
// LatencyFlag.cpp). This tool, on a Windows client streaming that host:
//   - clicks (SendInput, at the cursor's position) and stamps the moment on
//     QueryPerformanceCounter;
//   - listens to what this machine plays, through WASAPI loopback on the
//   default
//     output: each packet carries the QPC time of its first sample, so the
//     beep's first loud sample has an exact time on the same clock;
//   - optionally watches one screen pixel (--flag X,Y, a point of the host's
//     flag's blue band in the stream) on the DWM's composed desktop.
// Per click: click → sound, click → flag, and sound − flag, the lip-sync
// offset of that click.
//
// --tick SECS: no clicks. The host beeps and raises its flag together every
// 500 ms (MW_LATENCY_FLAG_SOUND=tick); each beep is paired with the nearest
// flag, and only the offset is measured, on this machine's clock alone.
//
// What it counts: from the click's send to the client's system mixer (the
// loopback tap) — the way up, the host's input, its beep through its own mixer
// and loopback capture, the pacer, Opus, the relay, the network, the browser's
// jitter buffer and its audio output. What it does not: the DAC, the speakers,
// a Bluetooth headset (100-200 ms more with SBC/AAC).
//
// Build (Developer Command Prompt, x64), static so it runs on any client:
//   cl /nologo /O2 /EHsc /MT /std:c++17 click-sound.cpp /Fe:mw-click-sound.exe
//   ole32.lib user32.lib gdi32.lib winmm.lib
// (build-click-sound.bat does it)
// Usage:
//   mw-click-sound [--clicks 30] [--interval 1000] [--timeout 1500]
//                  [--threshold 0.05] [--flag X,Y] [--tick SECS] [--out
//                  file.json] [--window TITLE [--center]] [--warmup N]
// The cursor must rest over the streaming page (--center puts it in the
// middle of --window's client area), the page must play sound (not a muted
// kiosk), and this machine must not be the host itself. --warmup N clicks
// first, measured by nothing: a page that captures the pointer takes its first
// click for that. Each click's line has its QPC time (µs), to join it with the
// page's frame log or a trace (POC Ultra U3.7, the end of the chain).

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmsystem.h>
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

double g_QpcToUs = 0;

int64_t qpcUs()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return static_cast<int64_t>(static_cast<double>(t.QuadPart) * g_QpcToUs);
}

struct Onsets
{
    std::mutex mutex;
    std::vector<int64_t> times; // µs on the QPC clock
    void add(int64_t t)
    {
        std::lock_guard<std::mutex> lk(mutex);
        times.push_back(t);
    }
    // The first onset at or after @p since, or -1.
    int64_t firstAfter(int64_t since)
    {
        std::lock_guard<std::mutex> lk(mutex);
        for (int64_t t : times)
            if (t >= since) return t;
        return -1;
    }
    std::vector<int64_t> all()
    {
        std::lock_guard<std::mutex> lk(mutex);
        return times;
    }
};

std::atomic<bool> g_Run{true};
Onsets g_Sound, g_Flag;
std::atomic<int> g_SoundReady{0}; // 1 capturing, -1 failed
// The loudest sample heard, x1000: how far a missed beep stayed under the threshold.
std::atomic<int> g_MaxPeakMilli{0};
constexpr int64_t kRefractoryUs = 150'000;
// --wav FILE: everything the client played, mono 16-bit, gaps filled with
// silence on the QPC clock, for a look at what became of a missed beep.
std::string g_WavPath;
// --window: the stream's window, where the flag is looked for by its colours.
HWND g_FlagWindow = nullptr;

void writeWavHeader(std::FILE* f, int rate, uint32_t samples)
{
    const uint32_t bytes = samples * 2;
    const uint32_t riff = 36 + bytes;
    const uint16_t pcm = 1, mono = 1, align = 2, bitsPer = 16;
    const uint32_t r = static_cast<uint32_t>(rate), byteRate = r * 2, fmtLen = 16;
    std::fseek(f, 0, SEEK_SET);
    std::fwrite("RIFF", 1, 4, f);
    std::fwrite(&riff, 4, 1, f);
    std::fwrite("WAVEfmt ", 1, 8, f);
    std::fwrite(&fmtLen, 4, 1, f);
    std::fwrite(&pcm, 2, 1, f);
    std::fwrite(&mono, 2, 1, f);
    std::fwrite(&r, 4, 1, f);
    std::fwrite(&byteRate, 4, 1, f);
    std::fwrite(&align, 2, 1, f);
    std::fwrite(&bitsPer, 2, 1, f);
    std::fwrite("data", 1, 4, f);
    std::fwrite(&bytes, 4, 1, f);
}

// WASAPI loopback of the default output. Polls every millisecond: loopback
// streams have no event of their own on every Windows.
void soundThread(float threshold)
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioCaptureClient* capture = nullptr;
    WAVEFORMATEX* fmt = nullptr;
    HRESULT hr =
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                         __uuidof(IMMDeviceEnumerator), reinterpret_cast<void**>(&enumerator));
    if (SUCCEEDED(hr)) hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr))
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                              reinterpret_cast<void**>(&client));
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&fmt);
    if (SUCCEEDED(hr))
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                200'000 /* 20 ms */, 0, fmt, nullptr);
    if (SUCCEEDED(hr))
        hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(&capture));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        std::fprintf(stderr, "loopback capture failed: 0x%08lx\n", static_cast<unsigned long>(hr));
        g_SoundReady = -1;
        return;
    }
    bool isFloat = fmt->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    if (fmt->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(fmt);
        // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT: the format tag in the GUID's first
        // field.
        isFloat = ext->SubFormat.Data1 == WAVE_FORMAT_IEEE_FLOAT;
    }
    const int channels = fmt->nChannels;
    const double rate = fmt->nSamplesPerSec;
    const int bits = fmt->wBitsPerSample;
    std::fprintf(stderr, "loopback: %d Hz, %d channels, %s %d-bit\n", static_cast<int>(rate),
                 channels, isFloat ? "float" : "pcm", bits);
    g_SoundReady = 1;
    int64_t lastOnset = 0;
    std::FILE* wav = g_WavPath.empty() ? nullptr : std::fopen(g_WavPath.c_str(), "wb");
    uint32_t wavSamples = 0;
    int64_t wavNextUs = -1;
    if (wav) writeWavHeader(wav, static_cast<int>(rate), 0);
    while (g_Run) {
        UINT32 next = 0;
        if (FAILED(capture->GetNextPacketSize(&next))) break;
        if (next == 0) {
            Sleep(1);
            continue;
        }
        BYTE* data = nullptr;
        UINT32 frames = 0;
        DWORD flags = 0;
        UINT64 devPos = 0, qpcPos = 0;
        if (FAILED(capture->GetBuffer(&data, &frames, &flags, &devPos, &qpcPos))) break;
        const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const int64_t firstUs = static_cast<int64_t>(qpcPos / 10); // 100 ns units
        if (!silent && data) {
            for (UINT32 i = 0; i < frames; ++i) {
                float peak = 0;
                for (int c = 0; c < channels; ++c) {
                    float v = 0;
                    if (isFloat && bits == 32)
                        v = reinterpret_cast<const float*>(data)[i * channels + c];
                    else if (bits == 16)
                        v = reinterpret_cast<const int16_t*>(data)[i * channels + c] / 32768.0f;
                    peak = std::max(peak, std::fabs(v));
                }
                if (peak * 1000 > g_MaxPeakMilli) g_MaxPeakMilli = static_cast<int>(peak * 1000);
                if (peak >= threshold) {
                    const int64_t t = firstUs + static_cast<int64_t>(i * 1e6 / rate);
                    if (t - lastOnset >= kRefractoryUs) {
                        g_Sound.add(t);
                        lastOnset = t;
                    }
                }
            }
        }
        if (wav) {
            if (wavNextUs < 0) {
                std::printf("wav starts at %lld"
                            "\n",
                            static_cast<long long>(firstUs));
            } else if (firstUs > wavNextUs + 1000) {
                // Nothing played meanwhile: the loopback sends no packet.
                const int64_t gap =
                    std::min<int64_t>((firstUs - wavNextUs) * rate / 1e6, 10 * rate);
                const int16_t zero = 0;
                for (int64_t k = 0; k < gap; ++k)
                    std::fwrite(&zero, 2, 1, wav);
                wavSamples += static_cast<uint32_t>(gap);
            }
            for (UINT32 i = 0; i < frames; ++i) {
                float sum = 0;
                if (!silent && data)
                    for (int c = 0; c < channels; ++c)
                        sum += isFloat && bits == 32
                                   ? reinterpret_cast<const float*>(data)[i * channels + c]
                                   : reinterpret_cast<const int16_t*>(data)[i * channels + c] /
                                         32768.0f;
                const float v = std::max(-1.0f, std::min(1.0f, sum / channels));
                const int16_t sample = static_cast<int16_t>(v * 32767);
                std::fwrite(&sample, 2, 1, wav);
            }
            wavSamples += frames;
            wavNextUs = firstUs + static_cast<int64_t>(frames * 1e6 / rate);
        }
        capture->ReleaseBuffer(frames);
    }
    client->Stop();
    if (wav) {
        writeWavHeader(wav, static_cast<int>(rate), wavSamples);
        std::fclose(wav);
    }
    capture->Release();
    client->Release();
    device->Release();
    enumerator->Release();
    CoTaskMemFree(fmt);
}

// The flag, read on the composed desktop like click-photon.ps1, but by its
// colour rather than a change of brightness: --flag names a point of its LEFT,
// pure blue band, and an onset is that pixel turning clearly blue. A bench page
// that scrolls under the flag changes brightness all the time; it is never
// that blue.
// The flag in the window, by its colours: a run of pure blue, then white, then
// red (LatencyFlag's tricolour), in the top half of the client area. Its blue
// band's middle is where flagThread then reads.
bool findFlag(HWND hwnd, int& x, int& y)
{
    RECT r;
    if (!GetClientRect(hwnd, &r)) return false;
    POINT tl = {r.left, r.top};
    ClientToScreen(hwnd, &tl);
    const int w = r.right - r.left, h = (r.bottom - r.top) / 2;
    if (w <= 0 || h <= 0) return false;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h; // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    void* bits = nullptr;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, screen, tl.x, tl.y, SRCCOPY);
    bool found = false;
    const auto* px = static_cast<const uint8_t*>(bits); // B, G, R, X
    auto at = [&](int cx, int cy) { return px + (static_cast<size_t>(cy) * w + cx) * 4; };
    auto isBlue = [&](const uint8_t* p) { return p[0] > 180 && p[1] < 70 && p[2] < 70; };
    auto isWhite = [&](const uint8_t* p) { return p[0] > 200 && p[1] > 200 && p[2] > 200; };
    auto isRed = [&](const uint8_t* p) { return p[2] > 180 && p[1] < 70 && p[0] < 70; };
    for (int cy = 0; cy < h && !found; cy += 2) {
        int run = 0;
        for (int cx = 0; cx < w && !found; ++cx) {
            if (isBlue(at(cx, cy))) {
                ++run;
                continue;
            }
            if (run >= 8 && isWhite(at(cx + 1 < w ? cx + 1 : cx, cy))) {
                const int band = run;
                const int redX = cx + band + band / 2;
                if (redX < w && isRed(at(redX, cy))) {
                    x = tl.x + cx - band / 2;
                    y = tl.y + cy;
                    found = true;
                }
            }
            run = 0;
        }
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return found;
}

void flagThread(int x, int y)
{
    // --window: no point yet; look for the flag until a click raises it.
    while (x < 0 && g_Run && g_FlagWindow) {
        if (findFlag(g_FlagWindow, x, y)) {
            std::fprintf(stderr, "the flag found in the window, its blue band read at %d,%d\n", x,
                         y);
            break;
        }
        Sleep(20);
    }
    if (x < 0) return;
    HDC dc = GetDC(nullptr);
    int64_t lastOnset = 0;
    bool up = false;
    while (g_Run) {
        const COLORREF c = GetPixel(dc, x, y);
        const int64_t t = qpcUs();
        const bool blue = GetBValue(c) > 160 && GetRValue(c) < 90 && GetGValue(c) < 90;
        if (blue && !up && t - lastOnset >= kRefractoryUs) {
            g_Flag.add(t);
            lastOnset = t;
        }
        up = blue;
    }
    ReleaseDC(nullptr, dc);
}

// --window TITLE: the flag's blue band inside the first visible window whose
// title holds TITLE (the stream page): 46 % across, 2.5 % down, the middle of
// the host flag's left band when the stream fills the window (LatencyFlag.h:
// 0.44-0.56 across, 0-0.05 down).
struct FindWindow
{
    std::wstring title;
    HWND found = nullptr;
};

BOOL CALLBACK findWindow(HWND hwnd, LPARAM lParam)
{
    auto* f = reinterpret_cast<FindWindow*>(lParam);
    if (!IsWindowVisible(hwnd)) return TRUE;
    wchar_t text[512] = {};
    GetWindowTextW(hwnd, text, 512);
    if (std::wstring(text).find(f->title) != std::wstring::npos) {
        f->found = hwnd;
        return FALSE;
    }
    return TRUE;
}

bool flagPointInWindow(const std::string& title, int& x, int& y)
{
    FindWindow f;
    f.title.assign(title.begin(), title.end());
    EnumWindows(findWindow, reinterpret_cast<LPARAM>(&f));
    if (!f.found) return false;
    // The client area, on the screen: a maximised window's title bar and
    // borders are not the stream.
    RECT r;
    if (!GetClientRect(f.found, &r)) return false;
    POINT tl = {r.left, r.top}, br = {r.right, r.bottom};
    ClientToScreen(f.found, &tl);
    ClientToScreen(f.found, &br);
    // Its middle, for --center.
    x = (tl.x + br.x) / 2;
    y = (tl.y + br.y) / 2;
    g_FlagWindow = f.found;
    std::fprintf(stderr, "window client %ld,%ld-%ld,%ld: the flag is looked for in it\n", tl.x,
                 tl.y, br.x, br.y);
    return true;
}

void click()
{
    INPUT in[2] = {};
    in[0].type = INPUT_MOUSE;
    in[0].mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in[1].type = INPUT_MOUSE;
    in[1].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(2, in, sizeof(INPUT));
}

double quantile(std::vector<double> v, double q)
{
    if (v.empty()) return -1;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(q * (v.size() - 1) + 0.5))];
}

std::string stats(const char* name, const std::vector<double>& v)
{
    char line[256];
    std::snprintf(line, sizeof line,
                  "\"%s\": {\"n\": %zu, \"median\": %.2f, \"p90\": %.2f, "
                  "\"min\": %.2f, \"max\": %.2f}",
                  name, v.size(), quantile(v, 0.5), quantile(v, 0.9), quantile(v, 0.0),
                  quantile(v, 1.0));
    return line;
}

} // namespace

int main(int argc, char** argv)
{
    int clicks = 30, intervalMs = 1000, timeoutMs = 1500, warmup = 0;
    double tickSecs = 0;
    float threshold = 0.05f;
    int flagX = -1, flagY = -1;
    bool center = false;
    std::string out, window;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto val = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--clicks")
            clicks = std::atoi(val().c_str());
        else if (a == "--interval")
            intervalMs = std::atoi(val().c_str());
        else if (a == "--timeout")
            timeoutMs = std::atoi(val().c_str());
        else if (a == "--threshold")
            threshold = static_cast<float>(std::atof(val().c_str()));
        else if (a == "--tick")
            tickSecs = std::atof(val().c_str());
        else if (a == "--out")
            out = val();
        else if (a == "--window")
            window = val();
        else if (a == "--center")
            center = true;
        else if (a == "--warmup")
            warmup = std::atoi(val().c_str());
        else if (a == "--wav")
            g_WavPath = val();
        else if (a == "--flag") {
            const std::string v = val();
            if (std::sscanf(v.c_str(), "%d,%d", &flagX, &flagY) != 2) flagX = flagY = -1;
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return 2;
        }
    }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_QpcToUs = 1e6 / static_cast<double>(f.QuadPart);
    timeBeginPeriod(1);
    if (!window.empty()) {
        // The stream may open after this starts (a series pass): wait for its
        // window, then for it to settle (kiosk, full screen) and read it again.
        bool found = false;
        for (int i = 0; i < 180 && !found; ++i) {
            found = flagPointInWindow(window, flagX, flagY);
            if (!found) Sleep(1000);
        }
        if (!found) {
            std::fprintf(stderr, "no visible window titled *%s*\n", window.c_str());
            return 1;
        }
        if (center) {
            SetCursorPos(flagX, flagY);
            std::fprintf(stderr, "the cursor put at %d,%d\n", flagX, flagY);
        }
        flagX = flagY = -1; // found by its colours (flagThread)
    }

    std::thread sound(soundThread, threshold);
    std::thread flag;
    const bool watchFlag = flagX >= 0 || g_FlagWindow != nullptr;
    if (watchFlag) flag = std::thread(flagThread, flagX, flagY);
    while (g_SoundReady == 0)
        Sleep(5);
    if (g_SoundReady < 0) {
        g_Run = false;
        sound.join();
        if (flag.joinable()) flag.join();
        return 1;
    }
    Sleep(300);

    std::vector<double> soundMs, flagMs, avMs;
    int soundMiss = 0, flagMiss = 0;
    std::string rows;
    if (tickSecs > 0) {
        // Each onset as it comes, flushed: a run stopped early keeps them.
        const int64_t end = qpcUs() + static_cast<int64_t>(tickSecs * 1e6);
        size_t printedSound = 0, printedFlag = 0;
        while (qpcUs() < end) {
            Sleep(500);
            const auto ss = g_Sound.all();
            const auto ff = g_Flag.all();
            for (; printedSound < ss.size(); ++printedSound)
                std::printf("beep %lld\n", static_cast<long long>(ss[printedSound]));
            for (; printedFlag < ff.size(); ++printedFlag)
                std::printf("flag %lld\n", static_cast<long long>(ff[printedFlag]));
            std::fflush(stdout);
        }
        const auto s = g_Sound.all();
        const auto fl = g_Flag.all();
        for (int64_t t : s) {
            int64_t best = -1;
            for (int64_t u : fl)
                if (best < 0 || std::llabs(u - t) < std::llabs(best - t)) best = u;
            if (best >= 0 && std::llabs(best - t) <= 250'000) avMs.push_back((t - best) / 1000.0);
        }
        std::printf("tick: %zu beeps, %zu flags, %zu paired; loudest sample %.3f\n", s.size(),
                    fl.size(), avMs.size(), g_MaxPeakMilli / 1000.0);
    } else {
        for (int k = 0; k < warmup; ++k) {
            click();
            std::printf("warmup %d at %lld\n", k + 1, static_cast<long long>(qpcUs()));
            Sleep(intervalMs);
        }
        for (int k = 0; k < clicks; ++k) {
            const int64_t t0 = qpcUs();
            click();
            const int64_t deadline = t0 + timeoutMs * 1000LL;
            int64_t ts = -1, tf = -1;
            while (qpcUs() < deadline && (ts < 0 || (watchFlag && tf < 0))) {
                Sleep(1);
                if (ts < 0) ts = g_Sound.firstAfter(t0);
                if (watchFlag && tf < 0) tf = g_Flag.firstAfter(t0);
            }
            const double s = ts >= 0 ? (ts - t0) / 1000.0 : -1;
            const double fl = tf >= 0 ? (tf - t0) / 1000.0 : -1;
            if (s >= 0)
                soundMs.push_back(s);
            else
                ++soundMiss;
            if (watchFlag) {
                if (fl >= 0)
                    flagMs.push_back(fl);
                else
                    ++flagMiss;
            }
            if (s >= 0 && fl >= 0) avMs.push_back(s - fl);
            std::printf("click %2d at %lld  sound %7.1f ms  flag %8.2f ms  sound-flag %7.1f ms\n",
                        k + 1, static_cast<long long>(t0), s, fl, s >= 0 && fl >= 0 ? s - fl : 0.0);
            std::fflush(stdout);
            char row[96];
            std::snprintf(row, sizeof row, "%s[%.2f, %.2f]", rows.empty() ? "" : ", ", s, fl);
            rows += row;
            const int64_t next = t0 + intervalMs * 1000LL;
            while (qpcUs() < next)
                Sleep(1);
        }
    }
    g_Run = false;
    sound.join();
    if (flag.joinable()) flag.join();
    timeEndPeriod(1);

    std::printf("click -> sound  median %.1f  p90 %.1f ms  (%zu, %d missed)\n",
                quantile(soundMs, 0.5), quantile(soundMs, 0.9), soundMs.size(), soundMiss);
    if (watchFlag)
        std::printf("click -> flag   median %.1f  p90 %.1f ms  (%zu, %d missed)\n",
                    quantile(flagMs, 0.5), quantile(flagMs, 0.9), flagMs.size(), flagMiss);
    if (!avMs.empty())
        std::printf("sound - flag    median %.1f  p10 %.1f  p90 %.1f ms  (%zu)\n",
                    quantile(avMs, 0.5), quantile(avMs, 0.1), quantile(avMs, 0.9), avMs.size());
    if (!out.empty()) {
        FILE* fp = std::fopen(out.c_str(), "wb");
        if (fp) {
            std::fprintf(fp,
                         "{%s, %s, %s, \"soundMissed\": %d, \"flagMissed\": %d, "
                         "\"threshold\": %.3f, "
                         "\"clicks\": [%s]}\n",
                         stats("sound", soundMs).c_str(), stats("flag", flagMs).c_str(),
                         stats("soundMinusFlag", avMs).c_str(), soundMiss, flagMiss, threshold,
                         rows.c_str());
            std::fclose(fp);
        }
    }
    return 0;
}
