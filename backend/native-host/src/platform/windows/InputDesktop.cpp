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

#include "InputDesktop.h"

#include "../../core/Log.h"
#include "../SignInScreen.h"

#include <windows.h>
#include <wtsapi32.h>

#include <atomic>
#include <mutex>
#include <string>

namespace mw::native::platform {
namespace {

/// The desktop THIS translation unit put the current thread on. Kept so it can
/// be closed after the next switch — and only then, since closing the desktop a
/// thread is standing on is undefined.
thread_local HDESK t_Owned = nullptr;

/// setSecureAttentionSender's.
std::mutex g_SenderLock;
std::function<bool(std::string&)> g_Sender;

/// enterSignInScreenOnly's: set once, never cleared.
std::atomic<bool> g_SignInOnly{false};
/// The first failed sign-in check, latched (signInScreenStillShown).
std::mutex g_SignInLock;
bool g_SignInLost = false;
std::string g_SignInLostWhy;

std::string desktopName(HDESK desktop)
{
    if (!desktop) return {};
    wchar_t buffer[256] = {};
    DWORD needed = 0;
    if (!::GetUserObjectInformationW(desktop, UOI_NAME, buffer, sizeof(buffer), &needed)) return {};
    const int wide = ::WideCharToMultiByte(CP_UTF8, 0, buffer, -1, nullptr, 0, nullptr, nullptr);
    if (wide <= 1) return {};
    std::string name(static_cast<size_t>(wide - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, buffer, -1, name.data(), wide, nullptr, nullptr);
    return name;
}

/// Somebody is signed in on the session this process runs in. A session at
/// the sign-in screen has no user name. "Yes" whenever Windows will not say:
/// the sign-in screen mode reads a doubt as a reason to stop.
bool sessionHasUser()
{
    LPWSTR name = nullptr;
    DWORD bytes = 0;
    if (!::WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, WTS_CURRENT_SESSION, WTSUserName,
                                       &name, &bytes) ||
        !name)
        return true;
    const bool someone = name[0] != 0;
    ::WTSFreeMemory(name);
    return someone;
}

} // namespace

bool runningAsSystem()
{
    static const bool system = []() {
        HANDLE token = nullptr;
        if (!::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token)) return false;
        DWORD size = 0;
        ::GetTokenInformation(token, TokenUser, nullptr, 0, &size);
        std::string buffer(size, '\0');
        bool isSystem = false;
        if (size && ::GetTokenInformation(token, TokenUser, buffer.data(), size, &size)) {
            SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
            PSID localSystem = nullptr;
            if (::AllocateAndInitializeSid(&authority, 1, SECURITY_LOCAL_SYSTEM_RID, 0, 0, 0, 0, 0,
                                           0, 0, &localSystem)) {
                isSystem = ::EqualSid(reinterpret_cast<TOKEN_USER*>(buffer.data())->User.Sid,
                                      localSystem) != 0;
                ::FreeSid(localSystem);
            }
        }
        ::CloseHandle(token);
        return isSystem;
    }();
    return system;
}

std::string inputDesktopName()
{
    HDESK desktop = ::OpenInputDesktop(0, FALSE, GENERIC_READ);
    if (!desktop) return {};
    const std::string name = desktopName(desktop);
    ::CloseDesktop(desktop);
    return name;
}

bool attachThread(std::string* name)
{
    if (!runningAsSystem()) {
        if (name) name->clear();
        return false;
    }

    // DESKTOP_SWITCHDESKTOP is not asked for: nothing here ever switches the
    // desktop, it only follows Windows. What is needed is the right to read it
    // (duplication) and to post to it (SendInput), which GENERIC_ALL covers and
    // a SYSTEM token is granted.
    HDESK desktop = ::OpenInputDesktop(0, FALSE, GENERIC_ALL);
    if (!desktop) {
        // Between two desktops — a switch in flight — OpenInputDesktop simply
        // fails, and the caller retries on its own schedule. Not worth a line
        // each time; the capture loop already says "the display is away".
        if (name) name->clear();
        return false;
    }
    const std::string opened = desktopName(desktop);
    if (name) *name = opened;

    // Sign-in screen mode: Winlogon and nothing else. A thread is never moved
    // to the desktop of somebody who signed in on this session; it stays
    // where it was, and the per-picture and per-input checks end the stream.
    if (g_SignInOnly.load() && !isSignInDesktop(opened)) {
        ::CloseDesktop(desktop);
        return false;
    }

    // Already there: do not touch the thread. SetThreadDesktop on the desktop a
    // thread is already on succeeds, but the handle dance below would then be
    // closing and reopening for nothing, several times a second.
    HDESK current = ::GetThreadDesktop(::GetCurrentThreadId());
    if (current && desktopName(current) == opened) {
        ::CloseDesktop(desktop);
        return true;
    }

    if (!::SetThreadDesktop(desktop)) {
        // The documented refusal: the thread owns a window or a hook. Nothing
        // can be done about it from here — that thread will never follow — so
        // it is said once, loudly enough to explain input that stops at a UAC
        // prompt.
        static bool reported = false;
        if (!reported) {
            reported = true;
            log::warning("[native] this thread cannot follow the desktop switch (error " +
                         std::to_string(::GetLastError()) +
                         ") — it owns a window or a hook; the secure desktop will not take its "
                         "input");
        }
        ::CloseDesktop(desktop);
        return false;
    }

    // Only now: the thread has left the old desktop, so the handle we opened
    // for it last time is safe to release.
    if (t_Owned) ::CloseDesktop(t_Owned);
    t_Owned = desktop;
    log::info("[native] now on the \"" + opened + "\" desktop");
    return true;
}

void setSecureAttentionSender(std::function<bool(std::string& error)> sender)
{
    std::lock_guard<std::mutex> lock(g_SenderLock);
    g_Sender = std::move(sender);
}

bool sendSecureAttention(std::string& error)
{
    if (!runningAsSystem()) {
        error = "Ctrl+Alt+Suppr needs the worker to run as SYSTEM (the launcher service)";
        return false;
    }
    std::function<bool(std::string&)> sender;
    {
        std::lock_guard<std::mutex> lock(g_SenderLock);
        sender = g_Sender;
    }
    if (sender) return sender(error);
    // Loaded on demand and kept: sas.dll is present on every desktop Windows,
    // but this is the only thing in the engine that wants it, and a session
    // that never presses the combination should never map it. From System32
    // only: the caller is SYSTEM.
    using SendSasFn = VOID(WINAPI*)(BOOL);
    static HMODULE library = ::LoadLibraryExW(L"sas.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    static SendSasFn sendSas = library ? reinterpret_cast<SendSasFn>(reinterpret_cast<void*>(
                                             ::GetProcAddress(library, "SendSAS")))
                                       : nullptr;
    if (!sendSas) {
        error = "sas.dll has no SendSAS on this machine";
        return false;
    }
    // FALSE: as the SYSTEM service we are, not "as the current user" — the
    // latter needs the caller to BE the interactive user, which a worker
    // started by the launcher service is not.
    sendSas(FALSE);
    return true;
}

void enterSignInScreenOnly()
{
    if (!g_SignInOnly.exchange(true))
        log::info("[native] sign-in screen mode: pictures and input only while the sign-in "
                  "screen shows, and nobody is signed in on this session");
}

bool signInScreenOnly()
{
    return g_SignInOnly.load();
}

bool signInScreenStillShown(bool forPicture, std::string* why, SignInLook* look)
{
    if (!g_SignInOnly.load()) return true;
    {
        std::lock_guard<std::mutex> lock(g_SignInLock);
        if (g_SignInLost) {
            if (why) *why = g_SignInLostWhy;
            return false;
        }
    }
    SignInLook seen;
    seen.inputDesktop = inputDesktopName();
    if (forPicture) seen.threadDesktop = desktopName(::GetThreadDesktop(::GetCurrentThreadId()));
    seen.sessionHasUser = sessionHasUser();
    const std::string refusal = signInRefusal(seen, forPicture);
    if (look) *look = seen;
    if (refusal.empty()) return true;

    std::lock_guard<std::mutex> lock(g_SignInLock);
    if (!g_SignInLost) {
        g_SignInLost = true;
        g_SignInLostWhy = refusal;
        log::warning("[native] sign-in screen mode: " + refusal + " — the stream ends here (" +
                     (forPicture ? "a picture" : "an input") + " held back)");
    }
    if (why) *why = g_SignInLostWhy;
    return false;
}

} // namespace mw::native::platform
