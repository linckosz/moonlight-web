/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
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

#include "LatencyFlag.h"

#include <QtGlobal>
#include <QDebug>

#if defined(Q_OS_WIN) && defined(QT_DEBUG)
#define MW_LATENCY_FLAG_SUPPORTED 1
#else
#define MW_LATENCY_FLAG_SUPPORTED 0
#endif

#if MW_LATENCY_FLAG_SUPPORTED

#include <windows.h>

#include <atomic>
#include <mutex>
#include <thread>

namespace {

constexpr UINT kMsgClick = WM_APP + 0x4C; // an injected left button went down
constexpr UINT_PTR kHideTimer = 1;
constexpr const wchar_t* kClassName = L"MoonlightWebLatencyFlag";

// All of this belongs to the overlay thread once it runs; g_Mutex only guards
// start/stop against each other (a settings POST racing the startup path).
std::mutex g_Mutex;
std::thread g_Thread;
std::atomic<bool> g_Running{false};
std::atomic<DWORD> g_ThreadId{0};
HWND g_Hwnd = nullptr;
HHOOK g_Hook = nullptr;

// Runs on the overlay thread, synchronously inside Windows' input delivery: do
// the least possible here and hand the work to the message loop. Windows
// drops a hook that stalls (LowLevelHooksTimeout), which would make the probe
// silently blind — another reason to keep it to one PostMessage.
LRESULT CALLBACK mouseHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && wParam == WM_LBUTTONDOWN) {
        const auto* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
        const bool injected = info && (info->flags & LLMHF_INJECTED);
        if (injected && g_Hwnd) PostMessageW(g_Hwnd, kMsgClick, 0, 0);
        // One line per injected click, so a run can be matched against the
        // browser's table (and a hook that never fires shows as silence).
        if (injected)
            qInfo() << "[LatencyFlag] injected click at" << info->pt.x << "," << info->pt.y;
    }
    return CallNextHookEx(nullptr, code, wParam, lParam);
}

void paintFlag(HWND hwnd)
{
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(hwnd, &ps);
    RECT rc;
    GetClientRect(hwnd, &rc);
    const int bandW = (rc.right - rc.left) / 3;
    // Pure primaries, not the official tricolour: what has to survive is a
    // chroma-subsampled encode and a downscale, and the browser classifies by
    // "clearly blue / clearly white / clearly red".
    const COLORREF colors[3] = {RGB(0, 0, 255), RGB(255, 255, 255), RGB(255, 0, 0)};
    for (int i = 0; i < 3; ++i) {
        RECT band = rc;
        band.left = rc.left + i * bandW;
        band.right = (i == 2) ? rc.right : band.left + bandW;
        HBRUSH brush = CreateSolidBrush(colors[i]);
        FillRect(dc, &band, brush);
        DeleteObject(brush);
    }
    EndPaint(hwnd, &ps);
}

LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case kMsgClick:
        // Show without stealing focus from whatever the click landed on, and
        // re-assert topmost in case a game raised itself above us since.
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
        // Paint now rather than at the next idle: the capture may run before
        // this loop gets back to WM_PAINT otherwise.
        UpdateWindow(hwnd);
        // A second click inside the window restarts the countdown.
        SetTimer(hwnd, kHideTimer, LatencyFlag::kShowMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kHideTimer) {
            KillTimer(hwnd, kHideTimer);
            ShowWindow(hwnd, SW_HIDE);
        }
        return 0;
    case WM_PAINT: paintFlag(hwnd); return 0;
    case WM_ERASEBKGND: return 1; // the bands cover everything
    case WM_DESTROY: return 0;
    default: return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
}

void overlayThread()
{
    g_ThreadId = GetCurrentThreadId();
    const HINSTANCE inst = GetModuleHandleW(nullptr);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.lpszClassName = kClassName;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    // A second registration (stop then start) fails harmlessly: the class is
    // still there from the first one.
    RegisterClassW(&wc);

    // Primary screen geometry. Under DPI virtualisation these are the
    // virtualised sizes, and so is the window — still the same fraction of the
    // screen, which is all the browser relies on.
    const int screenW = GetSystemMetrics(SM_CXSCREEN);
    const int screenH = GetSystemMetrics(SM_CYSCREEN);
    const int x = static_cast<int>(screenW * LatencyFlag::kLeft);
    const int y = static_cast<int>(screenH * LatencyFlag::kTop);
    const int w = static_cast<int>(screenW * (LatencyFlag::kRight - LatencyFlag::kLeft));
    const int h = static_cast<int>(screenH * (LatencyFlag::kBottom - LatencyFlag::kTop));

    // Layered + transparent: the flag never takes a click meant for the app
    // under it. Tool window: no taskbar button, no Alt-Tab entry. NoActivate:
    // showing it must not move keyboard focus.
    g_Hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED |
                                 WS_EX_TRANSPARENT,
                             kClassName, L"MoonlightWeb latency flag", WS_POPUP, x, y, w, h,
                             nullptr, nullptr, inst, nullptr);
    if (!g_Hwnd) {
        qWarning() << "[LatencyFlag] CreateWindowEx failed:" << GetLastError();
        g_Running = false;
        return;
    }
    SetLayeredWindowAttributes(g_Hwnd, 0, 255, LWA_ALPHA);

    g_Hook = SetWindowsHookExW(WH_MOUSE_LL, mouseHookProc, inst, 0);
    if (!g_Hook) {
        qWarning() << "[LatencyFlag] SetWindowsHookEx(WH_MOUSE_LL) failed:" << GetLastError();
        DestroyWindow(g_Hwnd);
        g_Hwnd = nullptr;
        g_Running = false;
        return;
    }

    qInfo() << "[LatencyFlag] armed:" << w << "x" << h << "at" << x << "," << y << "on a" << screenW
            << "x" << screenH << "screen, shown" << LatencyFlag::kShowMs << "ms per injected click";

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    UnhookWindowsHookEx(g_Hook);
    g_Hook = nullptr;
    DestroyWindow(g_Hwnd);
    g_Hwnd = nullptr;
    g_ThreadId = 0;
    qInfo() << "[LatencyFlag] stopped";
}

} // namespace

namespace LatencyFlag {

bool isSupported()
{
    return true;
}

bool isEnabled()
{
    return g_Running.load();
}

void setEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    if (enabled == g_Running.load()) return;
    if (enabled) {
        // A thread that bailed out early (no window, no hook) is still joinable.
        if (g_Thread.joinable()) g_Thread.join();
        g_Running = true;
        g_Thread = std::thread(overlayThread);
        return;
    }
    // WM_QUIT breaks the loop; the thread tears down its own window and hook.
    // The id can still be 0 if the thread has not reached its loop yet, so
    // retry briefly rather than post into the void.
    for (int i = 0; i < 100 && g_ThreadId.load() == 0 && g_Thread.joinable(); ++i)
        Sleep(10);
    if (g_ThreadId.load() != 0) PostThreadMessageW(g_ThreadId.load(), WM_QUIT, 0, 0);
    if (g_Thread.joinable()) g_Thread.join();
    g_Running = false;
}

} // namespace LatencyFlag

#else // !MW_LATENCY_FLAG_SUPPORTED

namespace LatencyFlag {

bool isSupported()
{
    return false;
}

bool isEnabled()
{
    return false;
}

void setEnabled(bool enabled)
{
    if (enabled) qInfo() << "[LatencyFlag] not available: needs a debug build on Windows";
}

} // namespace LatencyFlag

#endif
