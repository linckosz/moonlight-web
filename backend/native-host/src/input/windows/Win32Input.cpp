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

#include "Win32Input.h"

#include "../../platform/windows/InputDesktop.h"

#include "../../core/Log.h"
#include "UsScanCode.h"

#include <windows.h>
// IShellDispatch::MinimizeAll — the one lever an unelevated process has over an
// elevated window. See releaseBlock().
#include <shldisp.h>
// ImmGetDefaultIMEWnd: whether an input method is open where text goes.
#include <imm.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cwctype>
#include <iterator>
#include <string>
#include <chrono>
#include <thread>
#include <vector>

namespace mw::native::input {
namespace {

/// The browser sets this when the key had no US-layout equivalent and its code
/// must be taken as-is. Mirrors SS_KBE_FLAG_NON_NORMALIZED, redefined here
/// rather than included: Limelight.h is GPL and this module must not touch it.
/// The value is protocol, not implementation — it cannot drift without the
/// protocol itself changing.
constexpr uint8_t kFlagNonNormalized = 0x01;

/// Send one batch, reporting the first refusal and then staying quiet.
///
/// The failure that matters — UIPI, see foregroundBlocksInput — is NOT one
/// SendInput reports: Windows drops the events and returns success, as its
/// documentation says outright. What is caught here is the rest: a desktop
/// that went away, a malformed batch.
void sendBatch(INPUT* inputs, int count)
{
    if (count <= 0) return;
    const UINT sent = ::SendInput(static_cast<UINT>(count), inputs, sizeof(INPUT));
    if (sent == static_cast<UINT>(count)) return;

    static std::atomic<bool> reported{false};
    bool expected = false;
    if (!reported.compare_exchange_strong(expected, true)) return;

    log::warning("[native] SendInput refused an event (error " + std::to_string(::GetLastError()) +
                 ")");
}

/// Integrity level of a process — the RID of its mandatory label (0x1000 low,
/// 0x2000 medium, 0x3000 high, 0x4000 system) — or -1 when Windows will not
/// say, which for a protected process it will not.
int integrityLevel(HANDLE process)
{
    HANDLE token = nullptr;
    if (!::OpenProcessToken(process, TOKEN_QUERY, &token)) return -1;

    int level = -1;
    DWORD size = 0;
    ::GetTokenInformation(token, TokenIntegrityLevel, nullptr, 0, &size);
    if (size > 0) {
        std::vector<uint8_t> buffer(size);
        if (::GetTokenInformation(token, TokenIntegrityLevel, buffer.data(), size, &size)) {
            auto* label = reinterpret_cast<TOKEN_MANDATORY_LABEL*>(buffer.data());
            PSID sid = label->Label.Sid;
            const DWORD count = *::GetSidSubAuthorityCount(sid);
            if (count > 0) level = static_cast<int>(*::GetSidSubAuthority(sid, count - 1));
        }
    }
    ::CloseHandle(token);
    return level;
}

/// Our own level, computed once: it cannot change for the life of the process.
int ownIntegrityLevel()
{
    static const int level = integrityLevel(::GetCurrentProcess());
    return level;
}

std::string narrow(const wchar_t* wide)
{
    if (!wide || !*wide) return {};
    const int len = ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (len <= 1) return {};
    std::string out(static_cast<size_t>(len - 1), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, wide, -1, out.data(), len, nullptr, nullptr);
    return out;
}

/// What a window's process runs as: its integrity level, and a name for the
/// viewer when it turns out to matter.
///
/// Two things are read off the level, by the caller:
///
///  - UIPI: an unelevated process cannot inject into a window of a higher
///    level, so input stops working the moment such a window takes focus —
///    Task Manager, an installer, a game launched as administrator, a Hyper-V
///    console. SendInput says nothing, the events simply vanish, and from the
///    browser it looks exactly like a frozen session. So it is looked for
///    rather than waited for.
///  - Policy: a window at high integrity or above runs as administrator, and
///    a viewer without that standing on this machine is not let into it even
///    when the OS would allow it (SessionConfig::allowElevatedInput).
///
/// A window that cannot be read is UNKNOWN, level -1, and the policy holds it
/// back like an administrator's (gateReason): no window at all is what a worker
/// that does not follow the input desktop sees while the secure desktop has it
/// (a UAC prompt), and a token it may not open is a process of SYSTEM's. Our
/// own windows are the one exception, and say so.
Win32Input::WindowStanding standingOf(HWND window)
{
    Win32Input::WindowStanding standing;
    standing.name = "(no window MoonlightWeb can read: the secure desktop, or a process "
                    "of SYSTEM's)";
    if (!window) return standing;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid == 0) return standing;
    if (pid == ::GetCurrentProcessId()) {
        standing.ours = true;
        standing.name.clear();
        return standing;
    }

    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return standing;
    standing.level = integrityLevel(process);
    if (standing.level >= 0) standing.name.clear();

    if (standing.level >= SECURITY_MANDATORY_HIGH_RID) {
        wchar_t title[256] = {};
        ::GetWindowTextW(window, title, 256);
        wchar_t image[MAX_PATH] = {};
        DWORD imageLen = MAX_PATH;
        ::QueryFullProcessImageNameW(process, 0, image, &imageLen);
        const std::string path = narrow(image);
        const size_t slash = path.find_last_of('\\');
        const std::string exe = slash == std::string::npos ? path : path.substr(slash + 1);
        standing.name = "\"" + narrow(title) + "\" (" + exe + ", pid " + std::to_string(pid) + ")";
    }
    ::CloseHandle(process);
    return standing;
}

/// The top-level window at @p p: what a click there would land on.
HWND windowAt(POINT p)
{
    HWND hit = ::WindowFromPoint(p);
    return hit ? ::GetAncestor(hit, GA_ROOT) : nullptr;
}

/// The top-level window under the pointer.
HWND windowUnderCursor()
{
    POINT p = {};
    if (!::GetCursorPos(&p)) return nullptr;
    return windowAt(p);
}

/// The pointer placed at @p absX, @p absY, in SendInput's absolute coordinates
/// over the whole virtual desktop.
INPUT absoluteMove(LONG absX, LONG absY)
{
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = absX;
    input.mi.dy = absY;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return input;
}

/// One button's edge as SendInput takes it; false for a button there is none of.
bool buttonInput(int button, bool down, INPUT& input)
{
    input = {};
    input.type = INPUT_MOUSE;
    switch (button) {
    case 1: input.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP; break;
    case 2: input.mi.dwFlags = down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP; break;
    case 3: input.mi.dwFlags = down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP; break;
    case 4:
        input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON1;
        break;
    case 5:
        input.mi.dwFlags = down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        input.mi.mouseData = XBUTTON2;
        break;
    default: return false;
    }
    return true;
}

bool isModifierKey(int vk)
{
    switch (vk) {
    case VK_SHIFT:
    case VK_LSHIFT:
    case VK_RSHIFT:
    case VK_CONTROL:
    case VK_LCONTROL:
    case VK_RCONTROL:
    case VK_MENU:
    case VK_LMENU:
    case VK_RMENU:
    case VK_LWIN:
    case VK_RWIN: return true;
    default: return false;
    }
}

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Presses change the host; releases only ever undo one, and dropping a
/// release is how a key gets stuck. Motion changes nothing by itself. So the
/// gate is on presses, text and scrolling, and on nothing else.
bool isPress(InputEvent::Type type)
{
    switch (type) {
    case InputEvent::Type::KeyDown:
    case InputEvent::Type::CharDown:
    case InputEvent::Type::Utf8Text:
    case InputEvent::Type::MouseButtonDown:
    case InputEvent::Type::MouseScrollVertical:
    case InputEvent::Type::MouseScrollHorizontal:
    case InputEvent::Type::LockKeySync: return true;
    default: return false;
    }
}

bool isMouse(InputEvent::Type type)
{
    switch (type) {
    case InputEvent::Type::MouseButtonDown:
    case InputEvent::Type::MouseScrollVertical:
    case InputEvent::Type::MouseScrollHorizontal: return true;
    default: return false;
    }
}

void sendOne(INPUT& input)
{
    sendBatch(&input, 1);
}

/// UTF-8 to code points. What is not valid UTF-8 — a lone surrogate, a stray
/// byte — is dropped: the payload comes from a browser and is valid in every
/// case that matters.
std::u32string decodeUtf8Text(const std::string& utf8)
{
    std::u32string out;
    if (utf8.empty()) return out;
    const int size = static_cast<int>(utf8.size());
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, nullptr, 0);
    if (n <= 0) return out;
    std::wstring wide(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, utf8.data(), size, wide.data(), n);
    out.reserve(wide.size());
    for (size_t i = 0; i < wide.size(); ++i) {
        const wchar_t c = wide[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < wide.size() && wide[i + 1] >= 0xDC00 &&
            wide[i + 1] < 0xE000) {
            out += static_cast<char32_t>(0x10000 + ((c - 0xD800) << 10) + (wide[i + 1] - 0xDC00));
            ++i;
        } else if (c < 0xD800 || c >= 0xE000) {
            out += static_cast<char32_t>(c);
        }
    }
    return out;
}

std::string utf8Of(char32_t ch)
{
    std::string out;
    if (ch < 0x80) {
        out += static_cast<char>(ch);
    } else if (ch < 0x800) {
        out += static_cast<char>(0xC0 | (ch >> 6));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    } else if (ch < 0x10000) {
        out += static_cast<char>(0xE0 | (ch >> 12));
        out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    } else {
        out += static_cast<char>(0xF0 | (ch >> 18));
        out += static_cast<char>(0x80 | ((ch >> 12) & 0x3F));
        out += static_cast<char>(0x80 | ((ch >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (ch & 0x3F));
    }
    return out;
}

/// A plan's step as SendInput takes it: the scancode of the host's layout
/// when there is one, so Raw Input and DirectInput see the key; the virtual
/// key as is when the layout has no position for it.
INPUT toInput(const WinKeyEvent& e)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    if (e.kind == WinKeyEvent::Kind::Unicode) {
        input.ki.wScan = e.unit;
        input.ki.dwFlags = KEYEVENTF_UNICODE | (e.up ? KEYEVENTF_KEYUP : 0);
        return input;
    }
    input.ki.dwFlags = e.up ? KEYEVENTF_KEYUP : 0;
    if (e.scan == 0) {
        input.ki.wVk = e.vk;
        return input;
    }
    input.ki.wScan = e.scan;
    input.ki.dwFlags |= KEYEVENTF_SCANCODE;
    if (e.extended || isExtendedKey(e.vk)) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return input;
}

/// One plan, one SendInput: nothing the person at the host types gets in
/// between its keys.
void sendPlan(const std::vector<WinKeyEvent>& plan)
{
    if (plan.empty()) return;
    std::vector<INPUT> inputs;
    inputs.reserve(plan.size());
    for (const WinKeyEvent& e : plan)
        inputs.push_back(toInput(e));
    sendBatch(inputs.data(), static_cast<int>(inputs.size()));
}

/// The layout the foreground window types with, and that window. 0 with no
/// window to ask: a window losing activation, a desktop this thread does not
/// see — and then nothing about the host's layout is to be trusted.
uintptr_t foregroundLayout(HWND& window)
{
    window = ::GetForegroundWindow();
    if (!window) return 0;
    const DWORD thread = ::GetWindowThreadProcessId(window, nullptr);
    if (!thread) return 0;
    return reinterpret_cast<uintptr_t>(::GetKeyboardLayout(thread));
}

/// Whether an input method is open on @p window: there, keys are composed
/// into something else (a, then あ), so text is better left as text. Asked
/// only for the languages whose layouts come with one — Japanese, Chinese,
/// Korean — through the window every thread has for it. A window that does
/// not answer counts as open: Unicode is what always went.
bool imeOpen(HWND window, uintptr_t hkl)
{
    const WORD primary = PRIMARYLANGID(static_cast<LANGID>(hkl & 0xFFFF));
    if (primary != LANG_JAPANESE && primary != LANG_CHINESE && primary != LANG_KOREAN) return false;
    HWND ime = ::ImmGetDefaultIMEWnd(window);
    if (!ime) return false;
    // IMC_GETOPENSTATUS, which today's SDK headers no longer name: the
    // question every IME window still answers through WM_IME_CONTROL.
    constexpr WPARAM kGetOpenStatus = 0x0005;
    DWORD_PTR open = 0;
    if (!::SendMessageTimeoutW(ime, WM_IME_CONTROL, kGetOpenStatus, 0, SMTO_ABORTIFHUNG, 50, &open))
        return true;
    return open != 0;
}

std::string windowClass(HWND window)
{
    if (!window) return "(none)";
    wchar_t name[128] = {};
    if (::GetClassNameW(window, name, 128) <= 0) return "(unknown)";
    return narrow(name);
}

std::string hkl8(uintptr_t hkl)
{
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "0x%08X", static_cast<unsigned>(hkl & 0xFFFFFFFFu));
    return buffer;
}

/// Short name for the log line that says which kinds of input ever arrived.
const char* describe(InputEvent::Type type)
{
    switch (type) {
    case InputEvent::Type::KeyDown: return "key press";
    case InputEvent::Type::KeyUp: return "key release";
    case InputEvent::Type::CharDown: return "character press";
    case InputEvent::Type::CharUp: return "character release";
    case InputEvent::Type::Utf8Text: return "text";
    case InputEvent::Type::MouseMoveRelative: return "relative mouse move";
    case InputEvent::Type::MouseMoveAbsolute: return "absolute mouse move";
    case InputEvent::Type::MouseButtonDown: return "mouse button press";
    case InputEvent::Type::MouseButtonUp: return "mouse button release";
    case InputEvent::Type::MouseScrollVertical: return "scroll";
    case InputEvent::Type::MouseScrollHorizontal: return "horizontal scroll";
    case InputEvent::Type::ControllerArrival: return "controller arrival";
    case InputEvent::Type::ControllerState: return "controller state";
    case InputEvent::Type::ControllerRemoval: return "controller removal";
    case InputEvent::Type::LockKeySync: return "lock-key sync";
    case InputEvent::Type::SecureAttention: return "Ctrl+Alt+Suppr";
    }
    return "event";
}

/// Build a keyboard event for one virtual key.
///
/// Scancode by default so Raw Input and DirectInput see it (see the class
/// comment); virtual key when the browser said the code is not normalized, or
/// when the key has no scancode at all — a dead mapping injected as scancode 0
/// would land as a keypress nobody asked for.
///
/// The scancode comes from the US layout (usScanCode), never from the host's:
/// the browser normalized the key's POSITION into a US virtual key, so only the
/// US layout leads back to that position. Asking Windows instead — its
/// MapVirtualKeyW answers with the calling thread's layout — made a French host
/// type every AZERTY key at its QWERTY position. Windows is still asked for the
/// keys no layout names (media, browser, Pause), which are layout-independent.
INPUT makeKeyInput(int vk, bool down, bool nonNormalized)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;

    UINT mapped = 0;
    if (!nonNormalized) {
        mapped = usScanCode(vk);
        if (mapped == 0) mapped = ::MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
    }
    const UINT prefix = (mapped >> 8) & 0xFF;
    const UINT scan = mapped & 0xFF;

    // 0xE1 is Pause's two-scancode sequence, which a single INPUT cannot
    // express. Let the virtual key carry it instead of emitting half of it.
    if (scan == 0 || prefix == 0xE1) {
        input.ki.wVk = static_cast<WORD>(vk);
        return input;
    }

    input.ki.wScan = static_cast<WORD>(scan);
    input.ki.dwFlags |= KEYEVENTF_SCANCODE;
    if (prefix == 0xE0 || isExtendedKey(vk)) input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    return input;
}

// ── Keyboard diagnostics ────────────────────────────────────────────────────
//
// Everything below runs only when NativeHost::setKeyboardDiagnostics(true) was
// called, and only on a key DOWN. It answers the one question no client can:
// how did THIS host's layout read the key we just injected?
//
// Two verdicts, because a keystroke has two jobs and they fail separately:
//   Notepad — the character a text field will show. Checked for real, by
//             reading the key we are about to press back through the host's
//             layout: if it does not come back as the character the client
//             asked for, the resolution is wrong and the line says what the
//             layout types instead.
//   Game    — the physical key a title reading raw scancodes sees, named by
//             its US label because that is how bindings are written. A real
//             key press scores OK; Unicode text scores KO, and always will —
//             it types the character perfectly and presses nothing.
//
// Never on the secure desktop, switch or no switch: what is typed there is the
// lock screen's PIN or a UAC prompt's password (NativeHost::
// secureDesktopHasInput). A desktop this process cannot read counts as that one.

bool diagnosing()
{
    return keyboardDiagnostics() && platform::inputDesktopName() == "Default";
}

std::string toUtf8(const std::wstring& text)
{
    if (text.empty()) return std::string();
    const int size = static_cast<int>(text.size());
    const int needed =
        ::WideCharToMultiByte(CP_UTF8, 0, text.data(), size, nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return std::string();
    std::string out(static_cast<size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.data(), size, out.data(), needed, nullptr, nullptr);
    return out;
}

std::string hexByte(unsigned value)
{
    static const char kDigits[] = "0123456789ABCDEF";
    std::string out = "0x";
    out += kDigits[(value >> 4) & 0xF];
    out += kDigits[value & 0xF];
    return out;
}

/// VkKeyScanEx's modifier byte, spelled out. 1 Shift, 2 Ctrl, 4 Alt — and
/// Ctrl+Alt together is how Windows reports AltGr.
std::string modifierText(int needed)
{
    std::string out;
    if (needed & 1) out += "+Shift";
    if ((needed & 6) == 6)
        out += "+AltGr";
    else {
        if (needed & 2) out += "+Ctrl";
        if (needed & 4) out += "+Alt";
    }
    return out;
}

/// The US label of the key at @p mapped (a MAPVK_VK_TO_VSC_EX value, prefix
/// included). Found by searching the US table rather than storing a second one
/// the two could drift apart on — this runs once per keystroke, in a debug mode.
std::string usKeyLabel(UINT mapped)
{
    if ((mapped & 0xFF) == 0) return "no US key";
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (usScanCode(vk) != mapped) continue;
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
            return std::string(1, static_cast<char>(vk));
        switch (vk) {
        case VK_SPACE: return "Space";
        case VK_RETURN: return "Enter";
        case VK_TAB: return "Tab";
        case VK_BACK: return "Backspace";
        case 0xBA: return ";";
        case 0xBB: return "=";
        case 0xBC: return ",";
        case 0xBD: return "-";
        case 0xBE: return ".";
        case 0xBF: return "/";
        case 0xC0: return "`";
        case 0xDB: return "[";
        case 0xDC: return "\\";
        case 0xDD: return "]";
        case 0xDE: return "'";
        default: return "VK " + hexByte(static_cast<unsigned>(vk));
        }
    }
    return "sc " + hexByte(mapped & 0xFF);
}

/// What @p layout types for @p vk held with @p neededMods. Empty when the key
/// produces nothing (a modifier, an arrow); the bare accent for a dead key.
std::wstring hostCharacter(int vk, UINT scanCode, int neededMods, HKL layout)
{
    BYTE state[256] = {};
    if (neededMods & 1) state[VK_SHIFT] = 0x80;
    if (neededMods & 2) state[VK_CONTROL] = 0x80;
    if (neededMods & 4) state[VK_MENU] = 0x80;

    wchar_t buffer[8] = {};
    // Bit 2 of wFlags: answer the question WITHOUT touching the layout's own
    // dead-key state. Asking what a key types must never change what the next
    // real keystroke does — leave it out and typing "^" then "e" stops giving
    // "ê" the moment diagnostics are on. Windows 10 1607 and later; older
    // builds ignore the bit, which is the pre-existing behaviour.
    const int written =
        ::ToUnicodeEx(static_cast<UINT>(vk), scanCode, state, buffer, 8, 0x4, layout);
    if (written > 0) return std::wstring(buffer, static_cast<size_t>(written));
    if (written < 0) return std::wstring(buffer, 1); // dead key: the accent alone
    return std::wstring();
}

/// A character injected as a real key: did the host's layout give it back?
///
/// The check walks the chain the HOST will walk, not the one we walked to get
/// here. We resolved character → virtual key → scancode; Windows will resolve
/// the injected scancode back into a virtual key through the active layout and
/// only then produce a character. Verifying the virtual key we started from
/// would leave the scancode step unchecked — which is exactly where the bug
/// fixed in 986bf929 lived: the virtual key came from the host's layout and the
/// scancode from the US table, so on a French host 'a' typed 'q' while every
/// intermediate value looked right. Starting from the scancode catches it, and
/// says nothing on a US host, which is why nobody saw it.
void reportChar(wchar_t wanted, int vk, UINT mapped, int neededMods, HKL layout)
{
    const UINT scanByte = mapped & 0xFF;
    std::string sent;
    std::wstring got;
    if (scanByte != 0) {
        const UINT hostVk = ::MapVirtualKeyExW(scanByte, MAPVK_VSC_TO_VK_EX, layout);
        got = hostCharacter(static_cast<int>(hostVk), scanByte, neededMods, layout);
        sent = ", scancode " + hexByte(scanByte);
    } else {
        // No position for it on this layout: the virtual key was sent as-is and
        // Windows works the scancode out on its own, so that is what to check.
        got = hostCharacter(vk, 0, neededMods, layout);
        sent = ", no scancode (virtual key sent as-is)";
    }
    const bool ok = got.size() == 1 && got[0] == wanted;

    const std::string line =
        "[KBD] host '" + toUtf8(std::wstring(1, wanted)) + "' -> VK " +
        hexByte(static_cast<unsigned>(vk)) + modifierText(neededMods) + sent +
        " | Notepad: " + (ok ? "OK" : "KO this layout types '" + toUtf8(got) + "'") +
        " | Game: OK real key, US '" + usKeyLabel(mapped) + "'";
    if (ok)
        log::info(line);
    else
        log::warning(line);
}

/// A character no key of this layout can reach, so it went out as Unicode.
void reportCharAsText(const std::string& utf8)
{
    log::warning("[KBD] host '" + utf8 +
                 "' -> no key on this layout | Notepad: OK as Unicode text | Game: KO nothing was "
                 "pressed");
}

/// A key sent by POSITION — layout fidelity off, or a key that has no character
/// to correct. There is no client character to check against here, so no
/// verdict: what this reports is the interpretation itself, which read next to
/// the transport's own line for the same keystroke is the whole answer.
void reportPosition(int vk, UINT mapped, HKL layout)
{
    const UINT scan = mapped & 0xFF;
    const UINT hostVk = ::MapVirtualKeyExW(scan, MAPVK_VSC_TO_VK_EX, layout);
    const std::wstring got = hostCharacter(static_cast<int>(hostVk), scan, 0, layout);
    if (got.empty()) return; // a modifier, an arrow, an F-key: no layout involved

    log::info("[KBD] host position VK " + hexByte(static_cast<unsigned>(vk)) + " (US '" +
              usKeyLabel(mapped) + "') -> this layout types '" + toUtf8(got) +
              "' | Notepad: no client character to check against | Game: OK real key, US '" +
              usKeyLabel(mapped) + "'");
}

/// A character from the fidelity path, as the layout's table resolved it.
void reportCharPlan(char32_t ch, const WinRecipe& recipe, uintptr_t hkl)
{
    const HKL layout = reinterpret_cast<HKL>(hkl);
    switch (recipe.kind) {
    case WinRecipe::Kind::Key:
        reportChar(static_cast<wchar_t>(ch), recipe.key.vk,
                   recipe.key.scan | (recipe.key.extended ? 0xE000u : 0u), recipe.key.mods, layout);
        return;
    case WinRecipe::Kind::DeadKey:
        log::info("[KBD] host '" + utf8Of(ch) + "' -> dead key VK " + hexByte(recipe.key.vk) +
                  modifierText(recipe.key.mods) + ", scancode " + hexByte(recipe.key.scan) +
                  ", then VK " + hexByte(recipe.base.vk) + modifierText(recipe.base.mods) +
                  ", scancode " + hexByte(recipe.base.scan) +
                  " | Notepad: OK composed by the layout | Game: OK real keys");
        return;
    case WinRecipe::Kind::Unicode: reportCharAsText(utf8Of(ch)); return;
    }
}

/// A chord of the touch keyboard's bar: the key it pressed. A shortcut is read
/// by virtual key, so that is the verdict; the US label says where the key is.
void reportChord(char32_t ch, const WinStroke& key)
{
    log::info("[KBD] host chord '" + utf8Of(ch) + "' -> VK " + hexByte(key.vk) + ", scancode " +
              hexByte(key.scan) + " (US '" + usKeyLabel(key.scan) +
              "'), modifiers as the viewer holds them | Shortcut: OK read on VK " +
              hexByte(key.vk));
}

/// A soft keyboard's text: what went as keys, through dead keys, as Unicode.
void reportText(const std::string& utf8, const WinTextCounts& counts, uintptr_t hkl,
                const std::string& whole)
{
    const std::string line = "[KBD] host text '" + utf8 + "' -> " + std::to_string(counts.keys) +
                             " key(s), " + std::to_string(counts.deadKeys) +
                             " dead-key sequence(s), " + std::to_string(counts.unicode) +
                             " as Unicode on layout " + hkl8(hkl) +
                             (whole.empty() ? std::string() : " (" + whole + ")") +
                             " | Console: " + (counts.unicode ? "KO for the Unicode ones" : "OK");
    if (counts.unicode)
        log::warning(line);
    else
        log::info(line);
}

/// Map a desktop point to SendInput's absolute space: 0..65535 across the
/// WHOLE virtual desktop, not across one screen.
bool desktopToAbsolute(int64_t desktopX, int64_t desktopY, LONG& outX, LONG& outY)
{
    const int virtualLeft = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth <= 1 || virtualHeight <= 1) return false;

    outX = static_cast<LONG>(((desktopX - virtualLeft) * 65535) / (virtualWidth - 1));
    outY = static_cast<LONG>(((desktopY - virtualTop) * 65535) / (virtualHeight - 1));
    return true;
}

/// Map a point on the captured display to SendInput's absolute space.
///
/// The display's origin has to be added before normalising — otherwise every
/// secondary monitor would be aimed at as if it were the primary. Both
/// rectangles come from the same DPI-virtualized coordinate system (see
/// DesktopRect), so no scaling correction belongs here.
/// A client position, in the client's own picture, as a desktop pixel of the
/// captured display.
///
/// Clamped to the display: a client whose aspect ratio differs slightly can
/// report a point a pixel or two outside, and letting that through would walk
/// the cursor onto the neighbouring screen.
bool clampToDisplay(const capture::DesktopRect& rect, int x, int y, int refW, int refH,
                    int64_t& outX, int64_t& outY)
{
    if (refW <= 0 || refH <= 0 || !rect.valid()) return false;

    const int64_t onDisplayX = rect.left + (static_cast<int64_t>(x) * rect.width()) / refW;
    const int64_t onDisplayY = rect.top + (static_cast<int64_t>(y) * rect.height()) / refH;
    outX = std::min<int64_t>(std::max<int64_t>(onDisplayX, rect.left), rect.right - 1);
    outY = std::min<int64_t>(std::max<int64_t>(onDisplayY, rect.top), rect.bottom - 1);
    return true;
}

bool toAbsolute(const capture::DesktopRect& rect, int x, int y, int refW, int refH, LONG& outX,
                LONG& outY)
{
    int64_t onDisplayX = 0;
    int64_t onDisplayY = 0;
    if (!clampToDisplay(rect, x, y, refW, refH, onDisplayX, onDisplayY)) return false;
    return desktopToAbsolute(onDisplayX, onDisplayY, outX, outY);
}

/// Where the pointer is on the desktop, if Windows will say (it will not from
/// a desktop we cannot see, in which case injection is pointless anyway).
bool cursorPosition(POINT& out)
{
    return ::GetCursorPos(&out) != FALSE;
}

bool contains(const capture::DesktopRect& rect, const POINT& p)
{
    return p.x >= rect.left && p.x < rect.right && p.y >= rect.top && p.y < rect.bottom;
}

/// The devices of one kind Windows lists for Raw Input, one made through VHF
/// told apart: made with no hardware ids, as ours are (VhidDevice.cpp), it
/// enumerates under HID_DEVICE_SYSTEM_VHF, which nothing plugged into the host
/// does. Mouse Keys are no device and are not counted.
struct DevicesListed
{
    bool known = false; ///< false: Windows would not say, and the counts mean nothing
    int physical = 0;
    int virtualHid = 0;
};

/// @p skipRemote leaves out what Remote Desktop keeps listed on any machine.
DevicesListed listDevices(DWORD type, bool skipRemote = false)
{
    DevicesListed devices;
    std::vector<RAWINPUTDEVICELIST> list;
    UINT count = 0;
    // A device that comes or goes between the two calls leaves the second one
    // short of room: asked again.
    for (int attempt = 0; attempt < 3 && !devices.known; ++attempt) {
        if (::GetRawInputDeviceList(nullptr, &count, sizeof(RAWINPUTDEVICELIST)) != 0)
            return devices;
        if (count == 0) {
            devices.known = true;
            return devices;
        }
        list.resize(count);
        const UINT got = ::GetRawInputDeviceList(list.data(), &count, sizeof(RAWINPUTDEVICELIST));
        if (got == static_cast<UINT>(-1)) {
            list.clear();
            continue;
        }
        list.resize(got);
        devices.known = true;
    }
    for (const RAWINPUTDEVICELIST& device : list) {
        if (device.dwType != type) continue;
        wchar_t name[512] = {};
        UINT chars = static_cast<UINT>(std::size(name) - 1);
        const bool named = ::GetRawInputDeviceInfoW(device.hDevice, RIDI_DEVICENAME, name,
                                                    &chars) != static_cast<UINT>(-1);
        std::wstring upper(name);
        for (wchar_t& c : upper)
            c = static_cast<wchar_t>(std::towupper(c));
        if (named && upper.find(L"HID_DEVICE_SYSTEM_VHF") != std::wstring::npos)
            ++devices.virtualHid;
        else if (!(named && skipRemote &&
                   (upper.find(L"RDP_") != std::wstring::npos ||
                    upper.find(L"TERMINPUT") != std::wstring::npos)))
            ++devices.physical;
    }
    return devices;
}

DevicesListed listMice()
{
    return listDevices(RIM_TYPEMOUSE);
}

/// The input desktop's name when nothing secure is up: where our keyboard may
/// speak (plan « souris HID virtuelle », Bruno, 05/11/2026).
constexpr const char* kNormalDesktop = "Default";

/// Whether the console session is this process's: a device's input goes to
/// whoever has the console, SendInput's to the session it was sent from. The
/// same question holds for our mouse, which does not ask it.
bool consoleIsOurs()
{
    static const DWORD ours = []() {
        DWORD session = 0xFFFFFFFF;
        if (!::ProcessIdToSessionId(::GetCurrentProcessId(), &session)) session = 0xFFFFFFFF;
        return session;
    }();
    return ours != 0xFFFFFFFF && ::WTSGetActiveConsoleSessionId() == ours;
}

} // namespace

Win32Input::Win32Input(const capture::DesktopRect& displayRect, VigemGamepad::RumbleSink onRumble,
                       VigemGamepad::NumberSink onPadNumber)
    : m_DisplayRect(displayRect)
    , m_OnRumble(std::move(onRumble))
    , m_OnPadNumber(std::move(onPadNumber))
{}

Win32Input::~Win32Input()
{
    stop();
}

void Win32Input::setDisplayRect(int left, int top, int right, int bottom)
{
    const capture::DesktopRect rect{left, top, right, bottom};
    if (rect.left == m_DisplayRect.left && rect.top == m_DisplayRect.top &&
        rect.right == m_DisplayRect.right && rect.bottom == m_DisplayRect.bottom)
        return;
    m_DisplayRect = rect;
    // Positions are about to mean somewhere else; what was learnt about
    // where the pointer keeps returning to is not worth carrying over — and
    // only a spot on this display can be learnt from now on.
    m_Recentre.setDisplay(rect.left, rect.top, rect.right, rect.bottom);
    log::info("[native] input: display now at " + std::to_string(m_DisplayRect.left) + "," +
              std::to_string(m_DisplayRect.top) + " " + std::to_string(m_DisplayRect.width()) +
              "x" + std::to_string(m_DisplayRect.height()));
}

bool Win32Input::start(std::string& error)
{
    if (m_Started) return true;
    m_Recentre.reset();
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        m_KeyboardQuit = false;
    }

    // Nothing to open, nothing to allocate for keyboard and mouse: SendInput
    // needs no handle and no setup. Whether there IS a desktop to inject into
    // was already settled by the probe (hasInteractiveSession), which refuses
    // the whole engine in session 0 rather than letting a session start and
    // feel dead.
    //
    // The gamepad is the one part that CAN be missing, because it needs a
    // kernel driver. Its absence is reported once and then forgotten: a desktop
    // stream does not need a controller, and refusing the session over it would
    // be wildly out of proportion.
    // Not on the sign-in screen: keyboard and mouse only there.
    if (platform::signInScreenOnly()) {
        log::info("[native] no virtual gamepad on the sign-in screen");
    } else {
        auto pads = std::make_unique<VigemGamepad>(m_OnRumble, m_OnPadNumber);
        std::string gamepadError;
        if (pads->start(gamepadError))
            m_Gamepad = std::move(pads);
        else
            log::info("[native] no virtual gamepad: " + gamepadError);
    }

    // Only a SYSTEM worker can stand on the secure desktop; for everybody else
    // this stays exactly the path that was measured — no queue, no thread.
    //
    // Settled BEFORE m_Started, which is what opens inject() to callers: the
    // other order lets the first events through on the caller's thread while
    // this is still false, and those are the ones that would miss the desktop.
    // Before m_Started as well: the first relative move may come at once.
    startVirtualMouse();

    // The table of the layout the keys will be typed in, read now rather than
    // on the first character: 1 to 8 ms once per layout (WinTextKeys), which a
    // first keystroke should not wait for. Another layout later pays it once.
    {
        HWND window = nullptr;
        WinKeyLayout::of(foregroundLayout(window));
    }

    m_Follow = platform::runningAsSystem();
    if (m_Follow) {
        m_FollowerQuit = false;
        m_Follower = std::thread([this] { runFollower(); });
        log::info("[native] input: running as SYSTEM — following the desktop switch, so the "
                  "UAC prompt and the lock screen take input too");
    }
    m_Started = true;
    log::info("[native] input: SendInput on the display at " + std::to_string(m_DisplayRect.left) +
              "," + std::to_string(m_DisplayRect.top) + " " +
              std::to_string(m_DisplayRect.width()) + "x" + std::to_string(m_DisplayRect.height()));
    (void)error;
    return true;
}

void Win32Input::stop()
{
    if (!m_Started) return;
    // No key starts through our keyboard from here on, and its thread goes:
    // the release below is what still speaks to it.
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        m_KeyboardQuit = true;
    }
    m_KeyboardWake.notify_all();
    // Before anything else it might report onto: the unblock thread ends by
    // telling the listener what the gate looks like now.
    {
        std::lock_guard<std::mutex> lock(m_UnblockMutex);
        if (m_UnblockThread.joinable()) m_UnblockThread.join();
    }
    if (m_Follow) {
        // The follower releases what is held on its way out — it is the only
        // thread that can still reach the desktop the keys were pressed on.
        {
            std::lock_guard<std::mutex> lock(m_QueueMutex);
            m_FollowerQuit = true;
        }
        m_QueueWake.notify_one();
        if (m_Follower.joinable()) m_Follower.join();
        if (m_Dropped)
            log::warning("[native] input: " + std::to_string(m_Dropped) +
                         " event(s) dropped — the host stopped accepting them");
        m_Follow = false;
    } else {
        releaseAll();
    }
    // Unplugged before anything else: a virtual pad that outlived its session
    // would sit in the Windows game controller list forever, and the next game
    // to start would see a controller nobody is holding.
    m_Gamepad.reset();
    // After the release above, which went through it for what it held: the
    // host has no pointing device again, as before the stream.
    if (m_Mouse) {
        m_Mouse.reset();
        log::info("[native] input: our mouse let go of");
    }
    if (m_KeyboardThread.joinable()) m_KeyboardThread.join();
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        if (m_Keyboard) {
            m_Keyboard.reset();
            log::info("[native] input: our keyboard let go of");
        }
        m_HidKeys.clear();
        m_HidForgotten.clear();
        m_HidSent = {};
        // m_KeyboardQuit stays up until the next start(): a press still on its
        // way must not start a thread nobody will join.
        m_KeyboardAsked = false;
        m_KeyboardListed = false;
    }
    const uint64_t gated = m_Gated.load(std::memory_order_relaxed);
    log::info("[native] input: " + std::to_string(m_Injected.load(std::memory_order_relaxed)) +
              " event(s) injected this session" +
              (gated ? ", " + std::to_string(gated) + " press(es) dropped at the gate" : ""));
    {
        // What the soft keyboard's text became, in one line: counts and why
        // Unicode was needed, never a character (a PIN goes through here).
        std::lock_guard<std::mutex> lock(m_TextMutex);
        const WinTextCounts& t = m_Text;
        if (t.keys || t.deadKeys || t.unicode || t.dropped) {
            std::string why;
            for (const std::string& reason : m_TextUnicodeWhy)
                why += (why.empty() ? " (" : "; ") + reason;
            if (!why.empty()) why += ")";
            log::info("[native] input: text: " + std::to_string(t.keys) + " char(s) as keys, " +
                      std::to_string(t.deadKeys) + " dead-key sequence(s), " +
                      std::to_string(t.unicode) + " as Unicode" + why +
                      (t.dropped
                           ? ", " + std::to_string(t.dropped) + " control character(s) dropped"
                           : std::string()));
        }
        m_Text = WinTextCounts{};
        m_TextUnicodeWhy.clear();
        m_TextWindowSaid = false;
    }
    m_Started = false;
}

/// Forget what is held without releasing it — the desktop it was pressed on is
/// gone, and Windows dropped the state with it. Releasing into the NEW desktop
/// would be a key-up nobody pressed, which some applications act on.
void Win32Input::forgetHeld()
{
    size_t keys = 0, buttons = 0;
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        keys = m_HeldKeys.size();
        buttons = m_HeldButtons.size();
        letGoKeyboardLocked();
        m_HeldKeys.clear();
        m_HeldButtons.clear();
        m_HeldScan.clear();
        m_CharKeys.clear();
        // Our mouse is told, unlike SendInput: a device keeps its buttons
        // across the switch, and its next report would release them anyway.
        if (m_MouseButtons) {
            m_MouseButtons = 0;
            sendThroughMouseLocked(0, 0, 0, 0);
        }
    }
    if (keys || buttons)
        log::info("[native] input: the desktop switched while " + std::to_string(keys) +
                  " key(s) and " + std::to_string(buttons) + " button(s) were down — forgotten");
}

void Win32Input::releaseAll()
{
    std::set<int> keys;
    std::set<int> buttons;
    std::map<int, uint16_t> scans;
    uint8_t mouseButtons = 0;
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        // What our keyboard holds goes up through it, in one report.
        letGoKeyboardLocked();
        keys.swap(m_HeldKeys);
        buttons.swap(m_HeldButtons);
        scans.swap(m_HeldScan);
        // A character's key is in m_HeldKeys too; one that is not any more
        // still goes up, at its own scancode.
        for (const auto& [ch, key] : m_CharKeys) {
            (void)ch;
            if (keys.insert(key.vk).second)
                scans[key.vk] = static_cast<uint16_t>(key.scan | (key.extended ? 0xE000 : 0));
        }
        m_CharKeys.clear();
        // What our mouse holds goes up through it, in one report.
        mouseButtons = m_MouseButtons;
        if (mouseButtons) {
            m_MouseButtons = 0;
            sendThroughMouseLocked(0, 0, 0, 0);
        }
    }

    if (keys.empty() && buttons.empty()) return;

    // Non-normalized is irrelevant on release: what matters is that the same
    // key goes up, and makeKeyInput derives that from the VK either way —
    // except for a character's key, which went down at the host layout's own
    // scancode for it, and goes up at that one.
    for (int vk : keys) {
        auto scan = scans.find(vk);
        if (scan != scans.end()) {
            WinKeyEvent up;
            up.up = true;
            up.vk = static_cast<uint8_t>(vk);
            up.scan = static_cast<uint8_t>(scan->second & 0xFF);
            up.extended = (scan->second & 0xFF00) != 0;
            sendPlan({up});
            continue;
        }
        INPUT input = makeKeyInput(vk, false, false);
        sendOne(input);
    }
    // Straight to the raw sender: the tracking sets were just emptied, so the
    // deduplicating path would look at them, decide these buttons are already
    // up, and release nothing at all.
    for (int button : buttons)
        if (!(mouseButtons & systemmouse::buttonBit(button))) sendMouseButton(button, false);

    log::info("[native] input: released " + std::to_string(keys.size()) + " key(s) and " +
              std::to_string(buttons.size()) + " button(s) at session end");
}

/// Straight to the OS, or through the thread that follows the desktop switch.
void Win32Input::inject(const InputEvent& event)
{
    if (!m_Started) return;
    if (!m_Follow) {
        injectNow(event);
        return;
    }
    // A SYSTEM worker: hand it to the one thread that may stand on the secure
    // desktop. The bound is generous — a second of the fastest mouse there is —
    // and reaching it means SendInput itself has stopped returning, in which
    // case the newest events are the ones worth keeping.
    constexpr size_t kMaxQueued = 4096;
    if (m_ClickTrace && event.type == InputEvent::Type::MouseButtonDown)
        m_PressQueuedUs.store(steadyNowUs(), std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(m_QueueMutex);
        if (m_Queue.size() >= kMaxQueued) {
            m_Queue.pop_front();
            ++m_Dropped;
        }
        m_Queue.push_back(event);
    }
    m_QueueWake.notify_one();
}

void Win32Input::runFollower()
{
    // The attach has to happen HERE, on this thread: a desktop is a property of
    // the thread, not of the process.
    std::string desktop;
    platform::attachThread(&desktop);
    // How often the desktop is re-read while events are flowing. A switch that
    // is missed costs at most this much input sent to the desktop that just
    // left; reading it on every single event would cost an OpenInputDesktop per
    // mouse move, which at 1000 Hz is not free.
    constexpr auto kRecheck = std::chrono::milliseconds(100);
    auto nextCheck = std::chrono::steady_clock::now();

    for (;;) {
        std::deque<InputEvent> batch;
        {
            std::unique_lock<std::mutex> lock(m_QueueMutex);
            m_QueueWake.wait(lock, [this] { return m_FollowerQuit || !m_Queue.empty(); });
            if (m_FollowerQuit && m_Queue.empty()) break;
            batch.swap(m_Queue);
        }
        // Before injecting, not after: the events in hand were produced for
        // whatever is on screen NOW.
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextCheck) {
            nextCheck = now + kRecheck;
            std::string current = platform::inputDesktopName();
            if (!current.empty() && current != desktop) {
                if (platform::attachThread(&desktop)) {
                    // Whatever was held belongs to the desktop that just left;
                    // on the new one nothing is pressed, and a key still marked
                    // down would never be released.
                    forgetHeld();
                }
            }
        }
        for (const InputEvent& event : batch)
            injectNow(event);
    }

    // The release has to happen on this thread too, and before it ends: after
    // the join there is nobody left who can reach the desktop.
    releaseAll();
}

/// One line the first time anything is injected, and a count at the end.
///
/// Worth its keep: "the stream works but nothing responds" has too many
/// plausible causes — the browser not forwarding, the relay dropping on policy,
/// the sink never constructed, Windows refusing — and they look identical from
/// the outside. This separates "nothing arrived" from "arrived and was
/// refused", which is the first question every time.
void Win32Input::injectNow(const InputEvent& event)
{
    // Sign-in screen mode (platform/SignInScreen.h), before anything else:
    // keyboard and mouse only, each event only while the sign-in screen shows
    // and nobody is signed in on this session. The first time not, nothing is
    // injected again, and the capture loop ends the session at its next look.
    if (platform::signInScreenOnly()) {
        if (event.type == InputEvent::Type::ControllerArrival ||
            event.type == InputEvent::Type::ControllerState ||
            event.type == InputEvent::Type::ControllerRemoval) {
            m_Gated.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        std::string why;
        if (!platform::signInScreenStillShown(false, &why)) {
            m_Gated.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    // The gate — see InputGate in NativeHost.h. Keyboard goes to the focused
    // window; a click or a scroll goes to whatever is under the pointer, which
    // is not always the same window, and is exactly how a viewer shut out of
    // an administrator window still clicks on any other. Both are looked up
    // through a one-second cache, so the token queries cost nothing next to
    // the injection. Reported once per change, not once per drop.
    if (isPress(event.type)) {
        const bool mouse = isMouse(event.type);
        if (!m_AllowElevated && !event.resync) settleAfterSwitch(!mouse);
        HWND target = nullptr;
        if (mouse && anchored()) {
            POINT placed = {static_cast<LONG>(m_PlacedX), static_cast<LONG>(m_PlacedY)};
            target = windowAt(placed);
        } else if (mouse) {
            target = windowUnderCursor();
        }
        if (!target) target = ::GetForegroundWindow();
        const WindowStanding& standing = cachedStanding(target, mouse ? m_UnderCursor : m_Focused);
        if (*gateReason(standing)) {
            m_Gated.fetch_add(1, std::memory_order_relaxed);
            reportGate(standing, target);
            return;
        }
    }
    // Ctrl+Alt+Suppr opens the secure desktop, which locks the host, signs
    // its user out or starts Task Manager: an administrator's act, held back
    // from a viewer the policy holds back (share_guests_admin_windows lets
    // them through with the rest).
    if (event.type == InputEvent::Type::SecureAttention && !m_AllowElevated) {
        m_Gated.fetch_add(1, std::memory_order_relaxed);
        log::warning("[native] input: Ctrl+Alt+Suppr not sent — this viewer is not let into "
                     "administrator windows, and the secure desktop is one");
        return;
    }
    // A foreground that cannot be read is said only by a press it held back,
    // just above: one that comes and goes as a window loses activation would
    // otherwise flash the viewer's strip at every motion.
    HWND foreground = ::GetForegroundWindow();
    const WindowStanding& focused = cachedStanding(foreground, m_Focused);
    if (focused.level >= 0 || focused.ours) reportGate(focused, foreground);
    // First of each KIND, not first overall. A stream where the pointer moves
    // but nothing clicks, or where the mouse works and the keyboard does not,
    // is the failure that actually happens, and one line per kind separates
    // those cases without a per-event log nobody can read. Past the gate: a
    // press it held back was not injected, and its kind's line waits for one
    // that is.
    const uint32_t bit = 1u << static_cast<int>(event.type);
    if ((m_SeenTypes.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
        log::info(std::string("[native] input: first ") + describe(event.type) + " injected");
    m_Injected.fetch_add(1, std::memory_order_relaxed);

    switch (event.type) {
    case InputEvent::Type::KeyDown: injectKey(event, true); break;
    case InputEvent::Type::KeyUp: injectKey(event, false); break;
    case InputEvent::Type::CharDown: injectChar(event, true); break;
    case InputEvent::Type::CharUp: injectChar(event, false); break;
    case InputEvent::Type::Utf8Text: injectText(event.text); break;
    case InputEvent::Type::MouseMoveRelative: injectMouseMove(event.deltaX, event.deltaY); break;
    case InputEvent::Type::MouseMoveAbsolute: injectMousePosition(event); break;
    case InputEvent::Type::MouseButtonDown: injectMouseButton(event.button, true); break;
    case InputEvent::Type::MouseButtonUp: injectMouseButton(event.button, false); break;
    case InputEvent::Type::MouseScrollVertical: injectScroll(event.scrollAmount, false); break;
    case InputEvent::Type::MouseScrollHorizontal: injectScroll(event.scrollAmount, true); break;
    case InputEvent::Type::LockKeySync: syncLockKeys(event); break;
    case InputEvent::Type::SecureAttention: {
        // On the follower thread, so the switch to the secure desktop that
        // SendSAS causes is one this very thread follows on its next round.
        std::string error;
        if (platform::sendSecureAttention(error))
            log::info("[native] input: Ctrl+Alt+Suppr sent");
        else
            log::warning("[native] input: Ctrl+Alt+Suppr not sent — " + error);
        break;
    }

    // Ignored when ViGEmBus is absent, never approximated: mapping a stick onto
    // the mouse would be a surprise, not a feature.
    case InputEvent::Type::ControllerArrival:
        if (m_Gamepad) m_Gamepad->arrive(event);
        break;
    case InputEvent::Type::ControllerState:
        if (m_Gamepad) m_Gamepad->update(event);
        break;
    case InputEvent::Type::ControllerRemoval:
        if (m_Gamepad) m_Gamepad->remove(event);
        break;
    }
    if (!m_AllowElevated && !event.resync) noteSwitch(event);
}

/// What may switch windows under a viewer the gate holds back. Windows does it
/// a little after the keys that ask for it — Alt+Tab when Alt goes up — so a
/// press sent right behind them would be checked against the window that is
/// leaving, and land in the one that comes: an administrator's, perhaps
/// (security review M2, 26/10/2026). Noted here, waited for by the next press
/// (settleAfterSwitch).
void Win32Input::noteSwitch(const InputEvent& event)
{
    switch (event.type) {
    case InputEvent::Type::KeyUp:
        // Alt+Tab, Alt+Esc, the Start menu: the switch lands as Alt or the
        // Windows key goes up.
        if (event.keyCode == VK_MENU || event.keyCode == VK_LMENU || event.keyCode == VK_RMENU ||
            event.keyCode == VK_LWIN || event.keyCode == VK_RWIN)
            m_SwitchUs = steadyNowUs();
        break;
    case InputEvent::Type::KeyDown:
    case InputEvent::Type::CharDown: {
        // Any key with Alt or the Windows key held (Alt+F4, Win+1), and
        // Ctrl+Esc or Ctrl+Shift+Esc, which opens Task Manager — an
        // administrator's window wherever its user is one.
        if (event.type == InputEvent::Type::KeyDown && isModifierKey(event.keyCode)) break;
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        const WinHeldMods held = heldModsLocked();
        if (held.alt || held.win || (held.ctrl && event.keyCode == VK_ESCAPE))
            m_SwitchUs = steadyNowUs();
        break;
    }
    case InputEvent::Type::MouseButtonUp:
        // A click activates the window it lands on, the taskbar's included —
        // a switch for the keyboard. Not while the viewer moves relatively,
        // as a game does: keys pressed with a click there would all wait.
        if (!m_Relative) m_ClickUs = steadyNowUs();
        break;
    default: break;
    }
}

void Win32Input::settleAfterSwitch(bool keyboard)
{
    // Long enough for the shell's switch, measured by nobody but generous;
    // short enough that a key typed right after Alt+Tab is barely late.
    constexpr int64_t kSettleUs = 100000;
    const int64_t since = std::max<int64_t>(m_SwitchUs, keyboard ? m_ClickUs : 0);
    m_SwitchUs = 0;
    if (keyboard) m_ClickUs = 0;
    if (since == 0) return;
    const int64_t waitUs = since + kSettleUs - steadyNowUs();
    if (waitUs > 0) std::this_thread::sleep_for(std::chrono::microseconds(waitUs));
}

bool Win32Input::anchored() const
{
    return !m_AllowElevated && !m_Relative && m_Placed;
}

void Win32Input::injectKey(const InputEvent& event, bool down, bool throughKeyboard)
{
    const int vk = event.keyCode;
    if (vk <= 0 || vk > 0xFF) return;

    bool charKey = false;
    uint16_t charScan = 0;
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        // Let go of on the viewer's behalf: its repeats would land where they
        // never aimed them, until the release says the hand has come off.
        if (m_HidForgotten.count(vk)) {
            if (!down) m_HidForgotten.erase(vk);
            return;
        }
        // Up the way it went down. A repeat from the browser changes nothing:
        // Windows repeats a key a device holds, on its own.
        auto hid = m_HidKeys.find(vk);
        if (hid != m_HidKeys.end()) {
            if (down) return;
            m_HidKeys.erase(hid);
            m_HeldKeys.erase(vk);
            sendKeyboardReportLocked();
            return;
        }
        // A heartbeat re-press of a key we never let go of would be an extra
        // character. A real repeat from the browser is NOT filtered here: the
        // user holding a key genuinely wants typematic, and only the resync
        // path is suppressed.
        if (down && event.resync && m_HeldKeys.count(vk)) return;

        if (down && throughKeyboard && !event.resync && !m_HeldKeys.count(vk) &&
            !(event.keyFlags & kFlagNonNormalized) && keyboardReadyLocked(event.gaming)) {
            const systemkeyboard::Key key = systemkeyboard::keyOfScan(usScanCode(vk));
            size_t usages = 0;
            for (const auto& held : m_HidKeys)
                usages += held.second.usage != 0 ? 1 : 0;
            if (key.valid() && (key.usage == 0 || usages < systemkeyboard::kMaxKeys)) {
                m_HidKeys[vk] = key;
                m_HeldKeys.insert(vk);
                if (sendKeyboardReportLocked()) {
                    m_KeyboardWake.notify_all();
                    return;
                }
                // Refused and let go of: this one goes the old way.
                m_HidForgotten.erase(vk);
            }
        }

        if (down) {
            m_HeldKeys.insert(vk);
        } else {
            m_HeldKeys.erase(vk);
            // A character's key, released by its position: it went down at
            // the host layout's scancode, and goes up at that one, as at the
            // session's end (releaseAll). The US one would leave it down for
            // whatever reads scancodes, and nothing would let go of it later.
            auto scan = m_HeldScan.find(vk);
            if (scan != m_HeldScan.end()) {
                charKey = true;
                charScan = scan->second;
                m_HeldScan.erase(scan);
            }
            for (auto it = m_CharKeys.begin(); it != m_CharKeys.end();)
                it = it->second.vk == vk ? m_CharKeys.erase(it) : std::next(it);
        }
        // A modifier SendInput holds counts in the report's rules: before it
        // goes out, for a combination never to be seen whole.
        if (!m_HidKeys.empty()) sendKeyboardReportLocked();
    }
    if (charKey) {
        WinKeyEvent up;
        up.up = true;
        up.vk = static_cast<uint8_t>(vk);
        up.scan = static_cast<uint8_t>(charScan & 0xFF);
        up.extended = (charScan & 0xFF00) != 0;
        sendPlan({up});
        return;
    }

    // The modifier mask that rides along is NOT applied. The browser sends a
    // real keydown/keyup for Shift, Ctrl, Alt and Meta like any other key, so
    // pressing them again from the mask would double them — and the session's
    // input watchdog already owns reconciling what is still held.
    const bool nonNormalized = (event.keyFlags & kFlagNonNormalized) != 0;
    INPUT input = makeKeyInput(vk, down, nonNormalized);
    sendOne(input);

    if (down && !nonNormalized && diagnosing()) {
        // Positional path: the key's US position went out untouched and the
        // host's layout decides. What it decided is exactly what the viewer
        // wants to know when the wrong character appears.
        reportPosition(
            vk, usScanCode(vk),
            ::GetKeyboardLayout(::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr)));
    }
}

WinHeldMods Win32Input::heldModsLocked() const
{
    const auto held = [this](int a, int b, int c) {
        return m_HeldKeys.count(a) || m_HeldKeys.count(b) || m_HeldKeys.count(c);
    };
    WinHeldMods mods;
    mods.shift = held(VK_SHIFT, VK_LSHIFT, VK_RSHIFT);
    mods.ctrl = held(VK_CONTROL, VK_LCONTROL, VK_RCONTROL);
    mods.alt = held(VK_MENU, VK_LMENU, VK_RMENU);
    mods.win = m_HeldKeys.count(VK_LWIN) || m_HeldKeys.count(VK_RWIN);
    return mods;
}

void Win32Input::injectChar(const InputEvent& event, bool down)
{
    // One character the client's layout produced. It is resolved in the layout
    // of the FOREGROUND window's thread — the layout the character will actually
    // be interpreted in, which is the whole point — read on every keystroke:
    // the user can switch layouts mid-stream. WinKeyLayout keeps what each
    // layout types, so that read costs a lookup.
    const std::u32string text = decodeUtf8Text(event.text);
    if (text.empty()) return;
    if (text.size() != 1) {
        // More than one character for one key: something the client composed
        // and could not split. Typed whole on the press; nothing is held.
        if (down) injectText(event.text);
        return;
    }
    const char32_t ch = text[0];
    HWND window = nullptr;
    const uintptr_t hkl = foregroundLayout(window);
    const std::shared_ptr<const WinKeyLayout> layout = WinKeyLayout::of(hkl);
    if (event.chord) {
        injectChord(event, ch, layout.get(), down);
        return;
    }

    std::vector<WinKeyEvent> plan;
    if (!down) {
        // The key its press went down on. None: it was typed whole on the
        // press (an AltGr symbol, a dead key and its base, or Unicode), or a
        // desktop switch forgot it — either way there is nothing to let go of.
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        auto it = m_CharKeys.find(ch);
        if (it == m_CharKeys.end()) return;
        const WinStroke key = it->second;
        m_CharKeys.erase(it);
        m_HeldKeys.erase(key.vk);
        m_HeldScan.erase(key.vk);
        // The key alone: its levels went back on the press.
        planKeyEdge(key, false, WinHeldMods{}, plan);
    } else {
        const bool caps = (::GetKeyState(VK_CAPITAL) & 0x0001) != 0;
        const WinRecipe recipe = layout && !layout->ime() ? layout->recipe(ch, caps) : WinRecipe{};
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        WinHeldMods held = heldModsLocked();
        // AltGr, or a Mac's Option, composed it: their Ctrl and Alt are a
        // level here. Otherwise a Ctrl, an Alt or the Windows key held makes
        // the key a shortcut.
        held.altLevel = event.altGraph;
        const bool shortcut = (held.ctrl || held.alt || held.win) && !held.altLevel;
        WinStroke letter;
        const bool byLetter = shortcut && layout && !layout->ime() && layout->letterKey(ch, letter);
        if (byLetter) {
            // A shortcut names a key, and a program reads it by virtual key:
            // Ctrl+A is the key this layout calls A, under exactly what the
            // viewer holds, Caps Lock or not, where Ctrl+a typed as a
            // character would add or lift a Shift, or be Unicode on a layout
            // with no "a" at all (Russian). The client sends a shortcut's
            // other keys by their position.
            m_CharKeys[ch] = letter;
            m_HeldKeys.insert(letter.vk);
            m_HeldScan[letter.vk] =
                static_cast<uint16_t>(letter.scan | (letter.extended ? 0xE000 : 0));
            planChordEdge(letter, true, plan);
            if (diagnosing()) reportChord(ch, letter);
        } else if (recipe.kind == WinRecipe::Kind::Key) {
            // Held like the key it is. Nothing pressed here is ever recorded
            // as the viewer's own modifier: m_HeldKeys gets the character's
            // key, never the levels around it, which go back at once. An
            // AltGr symbol is typed whole, and nothing of it is held.
            if (planKeyEdge(recipe.key, true, held, plan)) {
                m_CharKeys[ch] = recipe.key;
                m_HeldKeys.insert(recipe.key.vk);
                m_HeldScan[recipe.key.vk] =
                    static_cast<uint16_t>(recipe.key.scan | (recipe.key.extended ? 0xE000 : 0));
            }
        } else if (layout && !layout->ime()) {
            // A dead key and its base, or no key at all: typed whole now.
            planText(*layout, text, caps, held, plan);
        } else {
            planUnicode(text, plan);
        }
        if (diagnosing() && !byLetter) reportCharPlan(ch, recipe, hkl);
    }
    sendPlan(plan);
}

void Win32Input::injectChord(const InputEvent& event, char32_t ch, const WinKeyLayout* layout,
                             bool down)
{
    // The key that carries the character on the host's layout, at whatever
    // level: Ctrl+z is the Z key. Sent as a US position it was the key where
    // QWERTY has Z, which an AZERTY host reads as W — Ctrl+W closed the tab
    // (22/10/2026). The release lets go of the key its press went down on.
    WinStroke key;
    bool have = false;
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        if (!down) {
            auto it = m_CharKeys.find(ch);
            if (it != m_CharKeys.end()) {
                key = it->second;
                have = true;
                m_CharKeys.erase(it);
            }
        }
    }
    if (!have && layout) have = layout->chordKey(ch, key);
    if (!have) {
        // No key for it on this layout (a Cyrillic one, say): its US
        // position, as before. Shortcuts there are read by virtual key, and
        // those layouts keep the Latin ones at the US places.
        if (event.keyCode <= 0) return;
        InputEvent positional = event;
        positional.type = down ? InputEvent::Type::KeyDown : InputEvent::Type::KeyUp;
        positional.keyFlags = 0;
        injectKey(positional, down, false);
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        if (down) {
            m_CharKeys[ch] = key;
            m_HeldKeys.insert(key.vk);
            m_HeldScan[key.vk] = static_cast<uint16_t>(key.scan | (key.extended ? 0xE000 : 0));
        } else {
            m_HeldKeys.erase(key.vk);
            m_HeldScan.erase(key.vk);
        }
    }
    std::vector<WinKeyEvent> plan;
    planChordEdge(key, down, plan);
    sendPlan(plan);
    if (down && diagnosing()) reportChord(ch, key);
}

void Win32Input::injectText(const std::string& utf8)
{
    // Text from a soft keyboard, typed as the keys of the host's layout — the
    // classic console drops a character injected as itself (KEYEVENTF_UNICODE),
    // administrator or not, and games never see one (WinTextKeys.h). Unicode
    // stays for what no key types, and for the whole text where keys would be
    // turned into something else: an input method open (Japanese, Chinese,
    // Korean), or no foreground window to read the layout of.
    const std::u32string text = decodeUtf8Text(utf8);
    if (text.empty()) return;

    HWND window = nullptr;
    const uintptr_t hkl = foregroundLayout(window);
    const std::shared_ptr<const WinKeyLayout> layout = WinKeyLayout::of(hkl);
    std::string whole; // why all of it goes as Unicode
    if (!layout)
        whole = "no foreground window to read the layout of";
    else if (layout->ime() || imeOpen(window, hkl))
        whole = "an input method was open";
    const bool caps = (::GetKeyState(VK_CAPITAL) & 0x0001) != 0;
    WinHeldMods held;
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        held = heldModsLocked();
    }

    // One SendInput per piece: a paste stays in order and in one block up to
    // the piece's size, and a very long one does not become one call of tens
    // of thousands of events. A \r\n is never cut in two Enters.
    constexpr size_t kPiece = 256;
    WinTextCounts counts;
    for (size_t at = 0; at < text.size();) {
        size_t n = std::min<size_t>(kPiece, text.size() - at);
        if (at + n < text.size() && text[at + n - 1] == U'\r' && text[at + n] == U'\n') ++n;
        const std::u32string piece = text.substr(at, n);
        std::vector<WinKeyEvent> plan;
        counts +=
            whole.empty() ? planText(*layout, piece, caps, held, plan) : planUnicode(piece, plan);
        sendPlan(plan);
        at += n;
    }
    noteText(counts, whole.empty() ? "no key on layout " + hkl8(hkl) : whole, window, hkl);
    if (diagnosing()) reportText(utf8, counts, hkl, whole);
}

void Win32Input::noteText(const WinTextCounts& counts, const std::string& unicodeWhy, void* window,
                          uintptr_t hkl)
{
    std::lock_guard<std::mutex> lock(m_TextMutex);
    m_Text += counts;
    if (counts.unicode) m_TextUnicodeWhy.insert(unicodeWhy);
    if (m_TextWindowSaid) return;
    m_TextWindowSaid = true;
    // The kind of window, never its title: a class names the program family
    // (ConsoleWindowClass is the classic console, CASCADIA_HOSTING_WINDOW_CLASS
    // Windows Terminal), which is what a "nothing typed" report needs.
    log::info("[native] input: first text, into a window of class \"" +
              windowClass(static_cast<HWND>(window)) + "\" on layout " + hkl8(hkl));
}

void Win32Input::injectMouseMove(int deltaX, int deltaY)
{
    if (deltaX == 0 && deltaY == 0) return;

    // A delta moves the pointer from wherever it IS — and that may be another
    // screen, invisible to a viewer who is looking at this one. Left there,
    // a trackpad client can push forever without ever seeing the cursor:
    // with a display sitting higher or lower than its neighbour, part of the
    // shared edge has no screen behind it and Windows simply stops the
    // pointer at the border. So the pointer is first brought home, to the
    // nearest point of the captured display, and the delta applied from there.
    //
    // Only when a delta arrives, never at session start: a stream that opens
    // should not touch the host's mouse until the viewer does.
    bringCursorOntoDisplay();

    // Through our mouse when there is one: a game reading Raw Input by device
    // sees it move. A placement above went through SendInput all the same,
    // and lands first.
    m_Relative = true;
    if (m_Mouse) {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        if (sendThroughMouseLocked(deltaX, deltaY, 0, 0)) return;
    }

    // Relative motion passes through the pointer speed and acceleration the
    // host has configured, exactly as a local mouse would. That is the right
    // default — a game reading raw input bypasses it anyway, and a desktop user
    // expects their own settings — but it does mean two hosts with different
    // pointer settings feel different for the same client movement. Our
    // mouse's reports go through the same settings.
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = deltaX;
    input.mi.dy = deltaY;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    sendOne(input);
}

void Win32Input::setAllowElevated(bool allow)
{
    m_AllowElevated = allow;
    if (allow) return;
    // A viewer held back from here on holds nothing on a device.
    std::lock_guard<std::mutex> lock(m_HeldMutex);
    letGoKeyboardLocked();
}

void Win32Input::setGateCallback(InputGateCallback callback)
{
    m_OnGate = std::move(callback);
}

const Win32Input::WindowStanding& Win32Input::cachedStanding(void* window, StandingCache& cache)
{
    // Same window within the second: what we already know. A process does not
    // change integrity level while it runs, so the only thing that can go
    // stale is the HWND itself being reused, which the second bounds.
    const int64_t nowUs = steadyNowUs();
    if (window != cache.window || nowUs >= cache.expiresUs) {
        cache.window = window;
        cache.standing = standingOf(static_cast<HWND>(window));
        cache.expiresUs = nowUs + 1000000;
    }
    return cache.standing;
}

const char* Win32Input::gateReason(const WindowStanding& standing) const
{
    if (standing.ours) return "";
    // Unknown is closed to a viewer the policy holds back, never open: that
    // is where a UAC prompt is, for a worker that cannot see it (standingOf).
    if (standing.level < 0) return m_AllowElevated ? "" : "policy";
    const int ours = ownIntegrityLevel();
    // The OS's refusal first: it applies whatever the policy says, and naming
    // it tells the operator what to change (run elevated), where "policy"
    // would tell them to grant a right that would change nothing.
    if (ours > 0 && standing.level > ours) return "uipi";
    if (!m_AllowElevated && standing.level >= SECURITY_MANDATORY_HIGH_RID) return "policy";
    return "";
}

void Win32Input::reportGate(const WindowStanding& standing, void* window, bool force)
{
    const char* reason = gateReason(standing);
    const bool blocked = *reason != '\0';
    // Locked because releaseBlock's thread reports too, and it is the whole
    // point of that thread: after it has minimised the window in the way,
    // nobody is pressing anything, so no injection would come along to notice.
    // Uncontended on the input path, where the GetForegroundWindow above
    // already costs more than this does.
    std::lock_guard<std::mutex> lock(m_GateMutex);
    // Same state, same window: nothing new. The name is compared too, so a
    // gate that moved from one elevated window to another is reported — the
    // viewer is told what is in the way now, not what was.
    if (!force && blocked == m_GateBlocked && (!blocked || standing.name == m_GateWindow)) return;
    m_GateBlocked = blocked;
    m_GateWindow = blocked ? standing.name : std::string();
    m_GateHwnd = blocked ? window : nullptr;

    if (blocked) {
        const std::string why =
            std::string(reason) == "uipi"
                ? "runs elevated and MoonlightWeb does not (UIPI) — nothing reaches it until "
                  "another window takes focus, or MoonlightWeb runs elevated"
            : standing.level < 0
                ? "cannot be told apart from an administrator's, and this viewer is not one "
                  "here — presses are dropped until a window that can be read takes focus"
                : "runs as administrator and this viewer is not one here — presses are dropped "
                  "until another window takes focus";
        log::warning("[native] input gate closed: the window " + standing.name + " " + why);
    } else {
        log::info("[native] input gate open: the focused window is reachable again");
    }

    if (!m_OnGate) return;
    InputGate gate;
    gate.blocked = blocked;
    gate.reason = reason;
    gate.window = m_GateWindow;
    m_OnGate(gate);
}

namespace {

/// Every top-level window a viewer would call "a window", in z-order, front
/// first. Already-minimised ones are left out: they were away before we
/// touched anything, and putting them back would be a change nobody asked for.
BOOL CALLBACK collectRestorable(HWND window, LPARAM param)
{
    auto* out = reinterpret_cast<std::vector<HWND>*>(param);
    if (!::IsWindowVisible(window) || ::IsIconic(window)) return TRUE;
    if (::GetWindow(window, GW_OWNER)) return TRUE; // a dialog follows its owner
    if (::GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return TRUE;
    if (::GetWindowTextLengthW(window) == 0) return TRUE;
    out->push_back(window);
    return TRUE;
}

/// Win+D, asked for by name. The shell may do to an elevated window what we may
/// not; this is the whole reason the way out goes through it.
bool minimiseEverything()
{
    const HRESULT init = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    // RPC_E_CHANGED_MODE: COM is already up on this thread under another model,
    // which is fine — we just must not uninitialise it.
    const bool owned = SUCCEEDED(init);
    if (init != RPC_E_CHANGED_MODE && !owned) {
        log::warning("[native] input unblock: COM refused to start (hr " + std::to_string(init) +
                     ")");
        return false;
    }

    IShellDispatch* shell = nullptr;
    const HRESULT hr = ::CoCreateInstance(CLSID_Shell, nullptr, CLSCTX_ALL, IID_IShellDispatch,
                                          reinterpret_cast<void**>(&shell));
    const bool ok = SUCCEEDED(hr) && shell;
    if (ok) {
        shell->MinimizeAll();
        shell->Release();
    } else {
        log::warning("[native] input unblock: no shell to ask (hr " + std::to_string(hr) +
                     ") — is Explorer running?");
    }
    if (owned) ::CoUninitialize();
    return ok;
}

} // namespace

bool Win32Input::releaseBlock()
{
    // It asks the shell for something; the sign-in screen mode asks Windows for
    // nothing but pictures and keys.
    if (platform::signInScreenOnly()) return false;
    HWND blocker = nullptr;
    {
        std::lock_guard<std::mutex> lock(m_GateMutex);
        if (!m_GateBlocked) return false;
        blocker = static_cast<HWND>(m_GateHwnd);
    }
    std::lock_guard<std::mutex> lock(m_UnblockMutex);
    // One at a time. The button that asks for this disables itself on the first
    // press, so a queue here would only ever hold a viewer pressing twice.
    if (m_UnblockThread.joinable()) m_UnblockThread.join();
    m_UnblockThread = std::thread([this, blocker] { runRelease(blocker); });
    return true;
}

/// Minimise the window in the way, and as little else as can be managed.
///
/// Minimising just that one window is what a viewer asks for and what Windows
/// refuses: ShowWindow across integrity levels is dropped exactly like
/// SendInput is — measured on this machine on 07/09/2026, where a SW_RESTORE
/// on an elevated console from an unelevated process changed nothing and
/// reported nothing. The only lever that reaches such a window is the shell's
/// own minimise-all.
///
/// So the blast radius is narrowed afterwards instead of before: note which
/// windows were up, minimise everything, then put back the ones we are allowed
/// to put back. The window in the way is skipped on purpose. Any OTHER elevated
/// window stays down too — not by choice, but because restoring it is refused
/// on the same grounds — and that is the honest limit of this.
void Win32Input::runRelease(void* blockerWindow)
{
    HWND blocker = static_cast<HWND>(blockerWindow);
    std::vector<HWND> restore;
    ::EnumWindows(collectRestorable, reinterpret_cast<LPARAM>(&restore));

    if (!minimiseEverything()) return;

    // MinimizeAll is a REQUEST to the shell, not a state change of our own: it
    // animates, and the windows go down over the frames that follow. Restoring
    // into that race is what left Bruno's desktop bare on 07/09 — every restore
    // landed first and the shell's minimise landed on top of it. So wait for it
    // to finish before putting anything back. A second is generous; the loop
    // gives up rather than hang if some window never goes down.
    const auto allDown = [&restore] {
        for (HWND w : restore)
            if (!::IsIconic(w)) return false;
        return true;
    };
    for (int waited = 0; waited < 40 && !allDown(); ++waited)
        std::this_thread::sleep_for(std::chrono::milliseconds(25));

    // Back to front, so whatever was in front before ends in front again.
    for (auto it = restore.rbegin(); it != restore.rend(); ++it) {
        if (*it == blocker) continue;
        ::ShowWindow(*it, SW_RESTORE);
    }

    // Then let the shell settle again before reading the result — and read it
    // from the windows themselves, never from ShowWindow's return value, which
    // reports whether the window WAS visible. A minimised window still is, so
    // that value would have called every refused restore a success.
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    size_t back = 0;
    size_t eligible = 0;
    for (HWND w : restore) {
        if (w == blocker) continue;
        ++eligible;
        if (!::IsIconic(w)) ++back;
    }
    log::info("[native] input unblock: desktop minimised at the viewer's request, " +
              std::to_string(back) + " of " + std::to_string(eligible) +
              " other window(s) put back");

    // Forced, so the report goes out even when the answer is "no change":
    // the viewer's button disabled itself on the press, and if this achieved
    // nothing they have to be told, or it stays greyed out over a session that
    // never came back.
    HWND foreground = ::GetForegroundWindow();
    reportGate(standingOf(foreground), foreground, true);
}

void Win32Input::bringCursorOntoDisplay()
{
    POINT here = {};
    if (!m_DisplayRect.valid() || !cursorPosition(here) || contains(m_DisplayRect, here)) return;

    // The nearest point inside, not the centre: a pointer sitting just past
    // the edge lands where the viewer would expect it, right at that edge.
    const int64_t x =
        std::min<int64_t>(std::max<int64_t>(here.x, m_DisplayRect.left), m_DisplayRect.right - 1);
    const int64_t y =
        std::min<int64_t>(std::max<int64_t>(here.y, m_DisplayRect.top), m_DisplayRect.bottom - 1);

    LONG absX = 0;
    LONG absY = 0;
    if (!desktopToAbsolute(x, y, absX, absY)) return;

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = absX;
    input.mi.dy = absY;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    sendOne(input);

    log::info("[native] input: pointer was on another screen (" + std::to_string(here.x) + "," +
              std::to_string(here.y) + "), brought onto the captured display at " +
              std::to_string(x) + "," + std::to_string(y));
}

void Win32Input::injectMousePosition(const InputEvent& event)
{
    int64_t x = 0;
    int64_t y = 0;
    if (!clampToDisplay(m_DisplayRect, event.positionX, event.positionY, event.referenceWidth,
                        event.referenceHeight, x, y))
        return;

    // A game that keeps warping the pointer to the middle of its window reads
    // the mouse as the distance from there — placing the client's position
    // would hand it half a screen per frame. Found by looking where the
    // pointer is before each placement; see RecentreDetector for the whole
    // argument. From then on the client's motion goes in as deltas, exactly
    // as gaming mode would send them, until the game lets the pointer be.
    POINT here = {};
    const bool haveHere = cursorPosition(here);
    const RecentreDetector::Verdict verdict =
        m_Recentre.observe(haveHere, here.x, here.y, x, y, steadyNowUs());
    if (verdict.changed) {
        if (verdict.relative)
            log::info(RecentreDetector::enteredMessage(m_Recentre.anchorX(), m_Recentre.anchorY()));
        else
            log::info(RecentreDetector::leftMessage());
    }
    if (verdict.relative) {
        // A pointer that re-enters the picture far from where it left is a
        // jump the viewer did not make: no game should turn on it.
        const int64_t width = m_DisplayRect.width();
        if (RecentreDetector::isReentryJump(verdict.deltaX, verdict.deltaY, width)) return;
        if (verdict.deltaX == 0 && verdict.deltaY == 0) return;
        // Nothing is placed any more: the buttons may go through our mouse.
        m_Relative = true;
        if (m_Mouse) {
            std::lock_guard<std::mutex> lock(m_HeldMutex);
            if (sendThroughMouseLocked(static_cast<int>(verdict.deltaX),
                                       static_cast<int>(verdict.deltaY), 0, 0))
                return;
        }
        INPUT input = {};
        input.type = INPUT_MOUSE;
        input.mi.dx = static_cast<LONG>(verdict.deltaX);
        input.mi.dy = static_cast<LONG>(verdict.deltaY);
        input.mi.dwFlags = MOUSEEVENTF_MOVE;
        sendOne(input);
        return;
    }

    LONG absX = 0;
    LONG absY = 0;
    if (!desktopToAbsolute(x, y, absX, absY)) return;

    // A placement: the clicks that follow go through SendInput behind it.
    if (m_Relative) {
        m_Relative = false;
        m_WheelNotches.reset();
        m_PanNotches.reset();
    }
    // Kept for the press that follows: see anchored().
    m_Placed = true;
    m_PlacedX = x;
    m_PlacedY = y;
    m_PlacedAbsX = absX;
    m_PlacedAbsY = absY;
    INPUT input = absoluteMove(absX, absY);
    sendOne(input);
}

void Win32Input::injectMouseButton(int button, bool down)
{
    if (button < 1 || button > 5) return;

    std::lock_guard<std::mutex> lock(m_HeldMutex);

    // A mouse button cannot be pressed twice without being released — no real
    // mouse can do it, and no application expects it. Windows reads a second
    // press at the same spot as a DOUBLE CLICK, which is exactly how a single
    // tap on a trackpad arrived here as one: the client's held-state heartbeat
    // beats every 100 ms, a tap outlives that, and the re-assertion landed as a
    // second press.
    //
    // Filtered unconditionally, not just on the resync path: whatever the
    // source — a duplicated event, two overlapping sessions — pressing an
    // already-pressed button is wrong.
    if (down == (m_HeldButtons.count(button) != 0)) return;

    if (down)
        m_HeldButtons.insert(button);
    else
        m_HeldButtons.erase(button);

    // Through our mouse while the viewer moves relatively; released the way it
    // was pressed, whatever the viewer does in between.
    //
    // Never for a viewer the policy holds back from administrator windows: a
    // device's report is hardware input, which reaches the secure desktop and
    // its UAC prompt, and lands wherever the pointer is when Windows reads it,
    // not where the gate looked. SendInput reaches neither. Their motion still
    // goes through our mouse, which shows the pointer and clicks nothing.
    const uint8_t bit = systemmouse::buttonBit(button);
    const bool throughMouse =
        m_Mouse && (down ? m_Relative && m_AllowElevated : (m_MouseButtons & bit) != 0);
    const int64_t startUs = m_ClickTrace && down ? steadyNowUs() : 0;
    if (throughMouse) {
        m_MouseButtons = static_cast<uint8_t>(down ? m_MouseButtons | bit : m_MouseButtons & ~bit);
        // Refused: the mouse is let go of with what it held, and a press goes
        // the old way. A release has nothing left to do — Windows let go of
        // the device's buttons with it.
        if (!sendThroughMouseLocked(0, 0, 0, 0) && down) {
            m_HeldButtons.insert(button);
            sendMouseButton(button, true);
        }
    } else if (down && anchored()) {
        // With the position the gate looked at, in the same SendInput: a
        // placement still on its way cannot move the press anywhere else.
        INPUT batch[2] = {absoluteMove(m_PlacedAbsX, m_PlacedAbsY), {}};
        if (buttonInput(button, true, batch[1])) sendBatch(batch, 2);
    } else {
        sendMouseButton(button, down);
    }
    if (startUs)
        m_ClickTrace->press(m_Follow ? m_PressQueuedUs.load(std::memory_order_relaxed) : 0, startUs,
                            steadyNowUs());
}

void Win32Input::sendMouseButton(int button, bool down)
{
    INPUT input = {};
    if (buttonInput(button, down, input)) sendOne(input);
}

void Win32Input::injectScroll(int amount, bool horizontal)
{
    if (amount == 0) return;

    // Through our mouse while the viewer moves relatively, in whole notches:
    // a report carries no less. Not for a viewer held back from administrator
    // windows, as a button is not (injectMouseButton).
    if (m_Mouse && m_Relative && m_AllowElevated) {
        const int notches = (horizontal ? m_PanNotches : m_WheelNotches).take(amount);
        if (notches == 0) return;
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        if (sendThroughMouseLocked(0, 0, horizontal ? 0 : notches, horizontal ? notches : 0))
            return;
        amount = notches * systemmouse::kNotch;
    }

    // Already in 120-unit notches: the relay quantizes high-resolution wheel
    // deltas upstream, for every transport, so re-scaling here would apply the
    // conversion twice.
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(amount);
    if (anchored()) {
        // Where the gate looked, as a click is (injectMouseButton).
        INPUT batch[2] = {absoluteMove(m_PlacedAbsX, m_PlacedAbsY), input};
        sendBatch(batch, 2);
        return;
    }
    sendOne(input);
}

void Win32Input::syncLockKeys(const InputEvent& event)
{
    // Lock keys are host STATE, not a keystroke: the client says what its own
    // NumLock/CapsLock/ScrollLock are, and the host taps the key only when the
    // two disagree. Replaying a press unconditionally would toggle a host that
    // already matched, and the two would take turns being wrong.
    const struct
    {
        int vk;
        bool wanted;
    } locks[] = {
        {VK_NUMLOCK, event.numLock},
        {VK_CAPITAL, event.capsLock},
        {VK_SCROLL, event.scrollLock},
    };

    for (const auto& lock : locks) {
        const bool current = (::GetKeyState(lock.vk) & 0x0001) != 0;
        if (current == lock.wanted) continue;

        INPUT down = makeKeyInput(lock.vk, true, false);
        INPUT up = makeKeyInput(lock.vk, false, false);
        INPUT tap[2] = {down, up};
        sendBatch(tap, 2);
    }
}

void Win32Input::startVirtualMouse()
{
    if (m_MouseMode == 0) return;
    if (m_MouseMode < 0 && listMice().physical > 0) return; // the host has one of its own

    auto device = makeVirtualHid(false);
    // No name: DirectInput's OEMName (VhidDevice.cpp) is for game controllers.
    VirtualHidIdentity identity;
    identity.descriptor = systemmouse::descriptor();
    std::string why;
    if (!device || !device->create(identity, why)) {
        log::info("[native] input: no mouse of our own (" + why +
                  ": a driver older than 0.1.2.0, none, or a worker that is not elevated) — "
                  "SendInput as before, and Mouse Keys show the pointer");
        return;
    }
    m_Mouse = std::move(device);
    m_MouseMadeUs = steadyNowUs();
    m_MouseLookedUs = 0;
    m_MouseListed = false;
    m_MouseUnlistedSaid = false;
    log::info(std::string("[native] input: ") +
              (m_MouseMode > 0 ? "a mouse of our own made beside the host's (vmouse=1)"
                               : "no pointing device on this host — a mouse of our own made") +
              ": Windows shows the pointer, games read a device; absolute positions stay "
              "SendInput");
}

bool Win32Input::sendThroughMouseLocked(int dx, int dy, int notches, int panNotches)
{
    if (!m_Mouse) return false;
    for (const systemmouse::Report& report :
         systemmouse::reports(m_MouseButtons, dx, dy, notches, panNotches)) {
        if (!m_Mouse->input(report.data(), report.size())) {
            dropVirtualMouseLocked();
            return false;
        }
    }
    return true;
}

void Win32Input::dropVirtualMouseLocked()
{
    log::warning("[native] input: our mouse refused a report (error " +
                 std::to_string(::GetLastError()) + ") — let go of, SendInput from here on");
    // Windows releases a device's buttons when the device goes.
    for (int button = 1; button <= 5; ++button)
        if (m_MouseButtons & systemmouse::buttonBit(button)) m_HeldButtons.erase(button);
    m_MouseButtons = 0;
    m_Mouse.reset();
}

bool Win32Input::keyboardReadyLocked(bool gaming)
{
    if (m_KeyboardQuit) return false;
    systemkeyboard::Standing standing;
    standing.mode = m_KeyboardMode;
    standing.allowElevated = m_AllowElevated;
    standing.gamingMode = gaming;
    standing.onConsole = consoleIsOurs();
    // Last, and only when it can still matter: it opens a desktop handle. One
    // this worker may not read counts as not the normal one.
    standing.onDefault = standing.mode != 0 && standing.allowElevated && standing.gamingMode &&
                         standing.onConsole && platform::inputDesktopName() == kNormalDesktop;
    if (!systemkeyboard::usable(standing)) return false;
    if (!m_KeyboardAsked) {
        m_KeyboardAsked = true;
        m_KeyboardThread = std::thread([this, standing] { runKeyboardThread(standing); });
        return false;
    }
    if (!m_Keyboard) return false;
    if (m_KeyboardListed) return true;
    // Windows drops a report until it has set the device up; listed, its
    // keyboard stack is there. Any keyboard made through VHF counts, another
    // stream's as well, as for the mouse.
    constexpr int64_t kLookEveryUs = 50 * 1000;
    const int64_t nowUs = steadyNowUs();
    if (nowUs - m_KeyboardLookedUs < kLookEveryUs) return false;
    m_KeyboardLookedUs = nowUs;
    m_KeyboardListed = listDevices(RIM_TYPEKEYBOARD, true).virtualHid > 0;
    if (m_KeyboardListed)
        log::info("[native] input: Windows lists our keyboard, " +
                  std::to_string((nowUs - m_KeyboardMadeUs) / 1000) + " ms after it was made");
    return m_KeyboardListed;
}

void Win32Input::runKeyboardThread(systemkeyboard::Standing standing)
{
    const DevicesListed listed = listDevices(RIM_TYPEKEYBOARD, true);
    if (!systemkeyboard::mayMake(standing, listed.known ? listed.physical : -1)) {
        log::info(std::string("[native] input: no keyboard of our own: ") +
                  (listed.known ? "the host has " + std::to_string(listed.physical) + " of its own"
                                : std::string("Windows would not list the host's")));
        return;
    }
    auto device = makeVirtualHid(false);
    VirtualHidIdentity identity;
    identity.descriptor = systemkeyboard::descriptor();
    std::string why;
    if (!device || !device->create(identity, why)) {
        log::info("[native] input: no keyboard of our own (" + why +
                  ": a driver older than 0.1.3.0, none, or a worker that is not elevated) — "
                  "SendInput as before");
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        if (m_KeyboardQuit) return; // the device goes with this scope
        m_Keyboard = std::move(device);
        m_KeyboardMadeUs = steadyNowUs();
        m_KeyboardLookedUs = 0;
        m_KeyboardListed = false;
    }
    log::info(std::string("[native] input: ") +
              (m_KeyboardMode > 0 ? "a keyboard of our own made beside the host's (vkeyboard=1)"
                                  : "no keyboard on this host — a keyboard of our own made") +
              ": games read a device; keys out of gaming mode, characters and the secure "
              "desktop stay SendInput");
    guardKeyboard();
}

void Win32Input::guardKeyboard()
{
    // How often a held key looks up. A desktop named otherwise is enough at once;
    // one that cannot be read takes a second look, since OpenInputDesktop fails
    // for a moment in the middle of a switch, and is what a worker that is not
    // SYSTEM sees of the secure desktop.
    constexpr auto kLookEvery = std::chrono::milliseconds(50);
    constexpr int kUnreadable = 2;
    int unreadable = 0;
    std::unique_lock<std::mutex> lock(m_HeldMutex);
    while (!m_KeyboardQuit && m_Keyboard) {
        if (m_HidKeys.empty()) {
            unreadable = 0;
            m_KeyboardWake.wait(
                lock, [this] { return m_KeyboardQuit || !m_Keyboard || !m_HidKeys.empty(); });
            continue;
        }
        lock.unlock();
        const bool console = consoleIsOurs();
        const std::string current = platform::inputDesktopName();
        lock.lock();
        if (m_HidKeys.empty()) continue;
        unreadable = current.empty() ? unreadable + 1 : 0;
        const bool away = current.empty() ? unreadable >= kUnreadable : current != kNormalDesktop;
        if (!console || away) {
            // Counted, never named: what is held may be a password's key.
            log::info("[native] input: our keyboard let go of " + std::to_string(m_HidKeys.size()) +
                      " key(s): " + (console ? "the desktop switched" : "the console changed"));
            letGoKeyboardLocked();
            continue;
        }
        m_KeyboardWake.wait_for(lock, kLookEvery, [this] { return m_KeyboardQuit || !m_Keyboard; });
    }
}

bool Win32Input::sendKeyboardReportLocked()
{
    if (!m_Keyboard) return false;
    uint8_t modifiers = 0;
    uint8_t others = 0;
    std::vector<uint8_t> usages;
    for (const auto& [vk, key] : m_HidKeys) {
        (void)vk;
        modifiers = static_cast<uint8_t>(modifiers | key.modifier);
        if (key.usage) usages.push_back(key.usage);
    }
    for (int vk : m_HeldKeys)
        if (!m_HidKeys.count(vk))
            others =
                static_cast<uint8_t>(others | systemkeyboard::keyOfScan(usScanCode(vk)).modifier);
    const systemkeyboard::Report report = systemkeyboard::reportOf(modifiers, usages, others);
    if (report == m_HidSent) return true;
    if (!m_Keyboard->input(report.data(), report.size())) {
        dropVirtualKeyboardLocked();
        return false;
    }
    m_HidSent = report;
    return true;
}

void Win32Input::letGoKeyboardLocked()
{
    for (const auto& [vk, key] : m_HidKeys) {
        (void)key;
        m_HeldKeys.erase(vk);
        m_HidForgotten.insert(vk);
    }
    m_HidKeys.clear();
    sendKeyboardReportLocked();
}

void Win32Input::dropVirtualKeyboardLocked()
{
    log::warning("[native] input: our keyboard refused a report (error " +
                 std::to_string(::GetLastError()) + ") — let go of, SendInput from here on");
    // Windows releases a device's keys when the device goes.
    for (const auto& [vk, key] : m_HidKeys) {
        (void)key;
        m_HeldKeys.erase(vk);
        m_HidForgotten.insert(vk);
    }
    m_HidKeys.clear();
    m_HidSent = {};
    m_Keyboard.reset();
    m_KeyboardListed = false;
    m_KeyboardWake.notify_all();
}

/// Whether our mouse is what shows the pointer: made, and listed by Windows —
/// or made a moment ago, the time Windows takes to list a device, so the
/// stream's first second does not turn Mouse Keys on for nothing. Listed
/// means any mouse made through VHF, another stream's as well: either shows
/// the pointer. Looked at four times a second at most.
bool Win32Input::showsPointer()
{
    // Read under the lock: the input thread lets go of our mouse when it
    // refuses a report (dropVirtualMouseLocked), and this runs on the capture's.
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        if (!m_Mouse) return false;
    }
    constexpr int64_t kLookEveryUs = 250 * 1000;
    constexpr int64_t kGraceUs = 3 * 1000 * 1000;
    const int64_t nowUs = steadyNowUs();
    if (m_MouseLookedUs == 0 || nowUs - m_MouseLookedUs >= kLookEveryUs) {
        m_MouseLookedUs = nowUs;
        const bool listed = listMice().virtualHid > 0;
        if (listed && !m_MouseListed && !m_MouseUnlistedSaid)
            log::info("[native] input: Windows lists our mouse, " +
                      std::to_string((nowUs - m_MouseMadeUs) / 1000) + " ms after it was made");
        m_MouseListed = listed;
    }
    if (m_MouseListed) return true;
    if (nowUs - m_MouseMadeUs < kGraceUs) return true;
    if (!m_MouseUnlistedSaid) {
        m_MouseUnlistedSaid = true;
        log::warning("[native] input: Windows does not list our mouse 3 s after it was made — "
                     "Mouse Keys show the pointer");
    }
    return false;
}

} // namespace mw::native::input
