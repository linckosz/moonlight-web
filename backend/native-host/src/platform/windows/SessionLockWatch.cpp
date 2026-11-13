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

#include "SessionLockWatch.h"

#include "../../core/Log.h"
#include "../SignInScreen.h"
#include "InputDesktop.h"

#include <windows.h>
#include <VersionHelpers.h>
#include <wtsapi32.h>

#include <chrono>

namespace mw::native::platform {

namespace {

constexpr UINT kMsgKick = WM_APP + 1;
constexpr UINT kMsgQuit = WM_APP + 2;
constexpr UINT_PTR kReadTimer = 1;
constexpr wchar_t kWindowClass[] = L"MoonlightWeb.SessionLockWatch";

// WTS_SESSIONSTATE_LOCK and WTS_SESSIONSTATE_UNLOCK, spelled out: Windows 7
// and Server 2008 R2 return the two the other way round (WTSINFOEX_LEVEL1's
// documentation), so those systems are not read at all (readSession).
constexpr LONG kSessionStateLock = 0;
constexpr LONG kSessionStateUnlock = 1;

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// This session's state, read now. Never the user's name, which the same
/// answer carries: nothing of it is kept or said.
LockRead readSession(DWORD session)
{
    if (!::IsWindows8OrGreater()) return LockRead::Unknown;
    // Not the session the console shows (another user switched to, a remote
    // session, Remote Desktop holding this one): as good as locked (D1).
    if (::WTSGetActiveConsoleSessionId() != session) return LockRead::Locked;
    LPWSTR buffer = nullptr;
    DWORD bytes = 0;
    if (!::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx,
                                       &buffer, &bytes) ||
        !buffer)
        return LockRead::Unknown;
    LockRead read = LockRead::Unknown;
    const auto* info = reinterpret_cast<const WTSINFOEXW*>(buffer);
    if (bytes >= sizeof(WTSINFOEXW) && info->Level == 1) {
        const WTSINFOEX_LEVEL1_W& level1 = info->Data.WTSInfoExLevel1;
        if (level1.SessionId != session) read = LockRead::Unknown;
        // Another user switched to, the console disconnected, a remote
        // session: nobody on this one's screen, as good as locked (D1).
        else if (level1.SessionState != WTSActive)
            read = LockRead::Locked;
        else if (level1.SessionFlags == kSessionStateUnlock)
            read = LockRead::Unlocked;
        else if (level1.SessionFlags == kSessionStateLock)
            read = LockRead::Locked;
    }
    ::WTSFreeMemory(buffer);
    return read;
}

const char* readName(LockRead read)
{
    switch (read) {
    case LockRead::Unlocked: return "unlocked";
    case LockRead::Locked: return "locked";
    case LockRead::Unknown: return "unknown";
    }
    return "unknown";
}

const char* noticeName(DWORD code)
{
    switch (code) {
    case WTS_SESSION_LOCK: return "lock";
    case WTS_SESSION_UNLOCK: return "unlock";
    default: return "other";
    }
}

/// The line said when the gate is found closed, by its reason.
std::string heldLine(DesktopGate::Why why)
{
    switch (why) {
    case DesktopGate::Why::Desktop:
        return "[native] plain HTTP: the input desktop is not the user's own, pictures held back, "
               "audio held, input dropped";
    case DesktopGate::Why::Unknown:
        return "[native] plain HTTP: the session's state cannot be read, pictures held back, "
               "audio held, input dropped";
    default:
        return "[native] plain HTTP: the session is locked, pictures held back, audio held, input "
               "dropped";
    }
}

} // namespace

/// The window procedure (SessionLockWatch's friend), on the watcher's thread.
struct LockWatchWindow
{
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
    {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
            ::SetWindowLongPtrW(window, GWLP_USERDATA,
                                reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self =
            reinterpret_cast<SessionLockWatch*>(::GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self) {
            switch (message) {
            case WM_WTSSESSION_CHANGE:
                self->onNotice(static_cast<unsigned long>(wParam),
                               static_cast<unsigned long>(lParam));
                return 0;
            case WM_TIMER:
                if (wParam != kReadTimer) break;
                self->evaluate();
                return 0;
            case kMsgKick: self->evaluate(); return 0;
            case kMsgQuit: ::PostQuitMessage(0); return 0;
            default: break;
            }
        }
        return ::DefWindowProcW(window, message, wParam, lParam);
    }
};

SessionLockWatch::SessionLockWatch(std::shared_ptr<DesktopGate> gate, Callbacks callbacks)
    : m_Gate(std::move(gate))
    , m_Callbacks(std::move(callbacks))
{}

SessionLockWatch::~SessionLockWatch()
{
    stop();
}

bool SessionLockWatch::start(std::string& error)
{
    if (!m_Gate) {
        error = "no gate to watch for";
        return false;
    }
    m_Thread = std::thread([this] { run(); });
    std::unique_lock<std::mutex> lock(m_StartMutex);
    m_StartWake.wait(lock, [this] { return m_StartDone; });
    if (m_StartOk) return true;
    error = m_StartError;
    lock.unlock();
    if (m_Thread.joinable()) m_Thread.join();
    return false;
}

void SessionLockWatch::kick()
{
    std::lock_guard<std::mutex> lock(m_WindowMutex);
    if (m_Window) ::PostMessageW(static_cast<HWND>(m_Window), kMsgKick, 0, 0);
}

void SessionLockWatch::stop()
{
    {
        std::lock_guard<std::mutex> lock(m_WindowMutex);
        if (m_Window) ::PostMessageW(static_cast<HWND>(m_Window), kMsgQuit, 0, 0);
    }
    if (m_Thread.joinable()) m_Thread.join();
}

void SessionLockWatch::run()
{
    const auto finishStart = [this](bool ok, const std::string& error) {
        std::lock_guard<std::mutex> lock(m_StartMutex);
        m_StartOk = ok;
        m_StartError = error;
        m_StartDone = true;
        m_StartWake.notify_all();
    };

    // This process's session, never the console's (D2): the stream is this
    // session's screen, and a notice is taken only when it names it.
    DWORD session = 0;
    if (::ProcessIdToSessionId(::GetCurrentProcessId(), &session) == FALSE) {
        finishStart(false, "this process's session is unknown (error " +
                               std::to_string(::GetLastError()) + ")");
        return;
    }
    m_Session = session;

    // An invisible top-level window: a message-only one does not always get
    // the session's notifications (D3). It is never shown, and it owns
    // nothing but this thread's queue.
    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = &LockWatchWindow::proc;
    windowClass.hInstance = instance;
    windowClass.lpszClassName = kWindowClass;
    if (::RegisterClassExW(&windowClass) == 0 && ::GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        finishStart(false, "no window class for the session's notifications (error " +
                               std::to_string(::GetLastError()) + ")");
        return;
    }
    HWND window = ::CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kWindowClass, L"",
                                    WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
    if (!window) {
        finishStart(false, "no window for the session's notifications (error " +
                               std::to_string(::GetLastError()) + ")");
        return;
    }

    // Registered first, read second (C1): what is announced in between waits
    // in this thread's queue, and is read again when it is taken.
    const bool registered =
        ::WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION) != FALSE;
    const DWORD registerError = registered ? 0 : ::GetLastError();
    const LockRead first = readSession(m_Session);
    m_Gate->read(first, isUserDesktop(inputDesktopName()), steadyNowUs());
    m_Seen = m_Gate->generation();
    ::SetTimer(window, kReadTimer, kReadEveryMs, nullptr);
    {
        std::lock_guard<std::mutex> lock(m_WindowMutex);
        m_Window = window;
    }
    log::info(
        std::string("[native] plain HTTP: the session's lock state is watched (") +
        (registered ? "notices, " : "no notices (error " + std::to_string(registerError) + "), ") +
        "a read every " + std::to_string(kReadEveryMs) + " ms); first read: " + readName(first) +
        ", pictures and audio " + (m_Gate->open() ? "go" : "held back"));
    finishStart(true, std::string());

    MSG message;
    while (::GetMessageW(&message, nullptr, 0, 0) > 0) {
        ::TranslateMessage(&message);
        ::DispatchMessageW(&message);
    }

    {
        std::lock_guard<std::mutex> lock(m_WindowMutex);
        m_Window = nullptr;
    }
    ::KillTimer(window, kReadTimer);
    if (registered) ::WTSUnRegisterSessionNotification(window);
    ::DestroyWindow(window);
    log::info("[native] plain HTTP: the session's lock state is no longer watched (" +
              std::to_string(m_Notices) + " notice(s) received)");
}

void SessionLockWatch::onNotice(unsigned long code, unsigned long session)
{
    ++m_Notices;
    // Its own session's only, by number (D2); another's word is never taken,
    // and only has this one read again (Q1).
    const bool own = session == m_Session;
    log::debug(std::string("[native] plain HTTP: lock notice received (") + noticeName(code) +
               (own ? "" : ", not this session's") + ")");
    const LockNotice notice = code == WTS_SESSION_LOCK     ? LockNotice::Lock
                              : code == WTS_SESSION_UNLOCK ? LockNotice::Unlock
                                                           : LockNotice::Other;
    const DesktopGate::Change change = m_Gate->notice(notice, own);
    if (change.readNow)
        evaluate();
    else
        settle();
}

void SessionLockWatch::evaluate()
{
    m_Gate->read(readSession(m_Session), isUserDesktop(inputDesktopName()), steadyNowUs());
    settle();
}

void SessionLockWatch::settle()
{
    const uint64_t now = m_Gate->generation();
    if (now == m_Seen) return;
    const bool closedSince = DesktopGate::closes(now) != DesktopGate::closes(m_Seen);
    m_Seen = now;
    if (closedSince && m_Callbacks.onClosed) m_Callbacks.onClosed();
    if (DesktopGate::isOpen(now)) {
        log::info(m_SaidFirst ? "[native] plain HTTP: the session is unlocked on the user's "
                                "desktop, pictures and audio resume"
                              : "[native] plain HTTP: the session is unlocked on the user's "
                                "desktop, pictures and audio go");
        m_SaidFirst = true;
        if (m_Callbacks.onOpened) m_Callbacks.onOpened();
    } else {
        log::info(heldLine(m_Gate->why()));
        m_SaidFirst = true;
    }
}

} // namespace mw::native::platform
