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

#include "../../core/Log.h"
#include "UsScanCode.h"

#include <windows.h>
// IShellDispatch::MinimizeAll — the one lever an unelevated process has over an
// elevated window. See releaseBlock().
#include <shldisp.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
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

/// Keys whose scancode needs the 0xE0 prefix. MAPVK_VK_TO_VSC_EX reports the
/// prefix for most of them, but not consistently across layouts and Windows
/// versions — NumLock in particular comes back as a bare 0x45, which without
/// the extended flag is Pause instead. Naming them is cheap insurance.
bool isExtendedKey(int vk)
{
    switch (vk) {
    case VK_RCONTROL:
    case VK_RMENU:
    case VK_INSERT:
    case VK_DELETE:
    case VK_HOME:
    case VK_END:
    case VK_PRIOR:
    case VK_NEXT:
    case VK_LEFT:
    case VK_RIGHT:
    case VK_UP:
    case VK_DOWN:
    case VK_NUMLOCK:
    case VK_DIVIDE:
    case VK_LWIN:
    case VK_RWIN:
    case VK_APPS:
    case VK_SNAPSHOT: return true;
    default: return false;
    }
}

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
Win32Input::WindowStanding standingOf(HWND window)
{
    Win32Input::WindowStanding standing;
    if (!window) return standing;

    DWORD pid = 0;
    ::GetWindowThreadProcessId(window, &pid);
    if (pid == 0 || pid == ::GetCurrentProcessId()) return standing;

    HANDLE process = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return standing;
    standing.level = integrityLevel(process);

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

/// The top-level window under the pointer: what a click would land on.
HWND windowUnderCursor()
{
    POINT p = {};
    if (!::GetCursorPos(&p)) return nullptr;
    HWND hit = ::WindowFromPoint(p);
    return hit ? ::GetAncestor(hit, GA_ROOT) : nullptr;
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
bool toAbsolute(const capture::DesktopRect& rect, int x, int y, int refW, int refH, LONG& outX,
                LONG& outY)
{
    if (refW <= 0 || refH <= 0 || !rect.valid()) return false;

    // Clamp to the display: a client whose aspect ratio differs slightly can
    // report a point a pixel or two outside, and letting that through would
    // walk the cursor onto the neighbouring screen.
    const int64_t onDisplayX = rect.left + (static_cast<int64_t>(x) * rect.width()) / refW;
    const int64_t onDisplayY = rect.top + (static_cast<int64_t>(y) * rect.height()) / refH;
    const int64_t clampedX =
        std::min<int64_t>(std::max<int64_t>(onDisplayX, rect.left), rect.right - 1);
    const int64_t clampedY =
        std::min<int64_t>(std::max<int64_t>(onDisplayY, rect.top), rect.bottom - 1);

    return desktopToAbsolute(clampedX, clampedY, outX, outY);
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

} // namespace

Win32Input::Win32Input(const capture::DesktopRect& displayRect, VigemGamepad::RumbleSink onRumble)
    : m_DisplayRect(displayRect)
    , m_OnRumble(std::move(onRumble))
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
    log::info("[native] input: display now at " + std::to_string(m_DisplayRect.left) + "," +
              std::to_string(m_DisplayRect.top) + " " + std::to_string(m_DisplayRect.width()) +
              "x" + std::to_string(m_DisplayRect.height()));
}

bool Win32Input::start(std::string& error)
{
    if (m_Started) return true;

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
    {
        auto pads = std::make_unique<VigemGamepad>(m_OnRumble);
        std::string gamepadError;
        if (pads->start(gamepadError))
            m_Gamepad = std::move(pads);
        else
            log::info("[native] no virtual gamepad: " + gamepadError);
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
    // Before anything else it might report onto: the unblock thread ends by
    // telling the listener what the gate looks like now.
    {
        std::lock_guard<std::mutex> lock(m_UnblockMutex);
        if (m_UnblockThread.joinable()) m_UnblockThread.join();
    }
    releaseAll();
    // Unplugged before anything else: a virtual pad that outlived its session
    // would sit in the Windows game controller list forever, and the next game
    // to start would see a controller nobody is holding.
    m_Gamepad.reset();
    const uint64_t gated = m_Gated.load(std::memory_order_relaxed);
    log::info("[native] input: " + std::to_string(m_Injected.load(std::memory_order_relaxed)) +
              " event(s) injected this session" +
              (gated ? ", " + std::to_string(gated) + " press(es) dropped at the gate" : ""));
    m_Started = false;
}

void Win32Input::releaseAll()
{
    std::set<int> keys;
    std::set<int> buttons;
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        keys.swap(m_HeldKeys);
        buttons.swap(m_HeldButtons);
    }

    if (keys.empty() && buttons.empty()) return;

    // Non-normalized is irrelevant on release: what matters is that the same
    // key goes up, and makeKeyInput derives that from the VK either way.
    for (int vk : keys) {
        INPUT input = makeKeyInput(vk, false, false);
        sendOne(input);
    }
    // Straight to the raw sender: the tracking sets were just emptied, so the
    // deduplicating path would look at them, decide these buttons are already
    // up, and release nothing at all.
    for (int button : buttons)
        sendMouseButton(button, false);

    log::info("[native] input: released " + std::to_string(keys.size()) + " key(s) and " +
              std::to_string(buttons.size()) + " button(s) at session end");
}

/// One line the first time anything is injected, and a count at the end.
///
/// Worth its keep: "the stream works but nothing responds" has too many
/// plausible causes — the browser not forwarding, the relay dropping on policy,
/// the sink never constructed, Windows refusing — and they look identical from
/// the outside. This separates "nothing arrived" from "arrived and was
/// refused", which is the first question every time.
void Win32Input::inject(const InputEvent& event)
{
    if (!m_Started) return;

    // First of each KIND, not first overall. A stream where the pointer moves
    // but nothing clicks, or where the mouse works and the keyboard does not,
    // is the failure that actually happens, and one line per kind separates
    // those cases without a per-event log nobody can read.
    const uint32_t bit = 1u << static_cast<int>(event.type);
    if ((m_SeenTypes.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
        log::info(std::string("[native] input: first ") + describe(event.type) + " injected");
    // The gate — see InputGate in NativeHost.h. Keyboard goes to the focused
    // window; a click or a scroll goes to whatever is under the pointer, which
    // is not always the same window, and is exactly how a viewer shut out of
    // an administrator window still clicks on any other. Both are looked up
    // through a one-second cache, so the token queries cost nothing next to
    // the injection. Reported once per change, not once per drop.
    if (isPress(event.type)) {
        const bool mouse = isMouse(event.type);
        HWND target = mouse ? windowUnderCursor() : nullptr;
        if (!target) target = ::GetForegroundWindow();
        const WindowStanding& standing = cachedStanding(target, mouse ? m_UnderCursor : m_Focused);
        if (*gateReason(standing)) {
            m_Gated.fetch_add(1, std::memory_order_relaxed);
            reportGate(standing, target);
            return;
        }
    }
    HWND foreground = ::GetForegroundWindow();
    reportGate(cachedStanding(foreground, m_Focused), foreground);
    m_Injected.fetch_add(1, std::memory_order_relaxed);

    switch (event.type) {
    case InputEvent::Type::KeyDown: injectKey(event, true); break;
    case InputEvent::Type::KeyUp: injectKey(event, false); break;
    case InputEvent::Type::CharDown: injectChar(event.text, true); break;
    case InputEvent::Type::CharUp: injectChar(event.text, false); break;
    case InputEvent::Type::Utf8Text: injectText(event.text); break;
    case InputEvent::Type::MouseMoveRelative: injectMouseMove(event.deltaX, event.deltaY); break;
    case InputEvent::Type::MouseMoveAbsolute: injectMousePosition(event); break;
    case InputEvent::Type::MouseButtonDown: injectMouseButton(event.button, true); break;
    case InputEvent::Type::MouseButtonUp: injectMouseButton(event.button, false); break;
    case InputEvent::Type::MouseScrollVertical: injectScroll(event.scrollAmount, false); break;
    case InputEvent::Type::MouseScrollHorizontal: injectScroll(event.scrollAmount, true); break;
    case InputEvent::Type::LockKeySync: syncLockKeys(event); break;

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
}

void Win32Input::injectKey(const InputEvent& event, bool down)
{
    const int vk = event.keyCode;
    if (vk <= 0 || vk > 0xFF) return;

    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        // A heartbeat re-press of a key we never let go of would be an extra
        // character. A real repeat from the browser is NOT filtered here: the
        // user holding a key genuinely wants typematic, and only the resync
        // path is suppressed.
        if (down && event.resync && m_HeldKeys.count(vk)) return;

        if (down)
            m_HeldKeys.insert(vk);
        else
            m_HeldKeys.erase(vk);
    }

    // The modifier mask that rides along is NOT applied. The browser sends a
    // real keydown/keyup for Shift, Ctrl, Alt and Meta like any other key, so
    // pressing them again from the mask would double them — and the session's
    // input watchdog already owns reconciling what is still held.
    INPUT input = makeKeyInput(vk, down, (event.keyFlags & kFlagNonNormalized) != 0);
    sendOne(input);
}

void Win32Input::injectChar(const std::string& utf8, bool down)
{
    // One character the client's layout produced, which its US key position
    // would not produce here. VkKeyScanExW answers in the layout of the FOREGROUND
    // window's thread — the layout the character will actually be interpreted
    // in, which is the whole point, and which is also why it is read on every
    // keystroke rather than cached: the user can switch layouts mid-stream.
    if (utf8.empty()) return;
    wchar_t wide[2] = {};
    const int units =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide, 2);
    // A single UTF-16 unit only: a surrogate pair has no key on any layout, and
    // an emoji is not something a key press can express.
    if (units != 1) {
        if (down) injectText(utf8);
        return;
    }

    const HKL layout =
        ::GetKeyboardLayout(::GetWindowThreadProcessId(::GetForegroundWindow(), nullptr));
    const SHORT scan = ::VkKeyScanExW(wide[0], layout);
    const int vk = scan & 0xFF;
    const int needed = (scan >> 8) & 0xFF;
    if (scan == -1 || vk == 0) {
        // The host's layout cannot reach this character with any combination.
        // Fall back to the Unicode path, which needs no key at all — the
        // character still appears, it simply is not a key press.
        if (down) injectText(utf8);
        return;
    }

    // Bits of the high byte: 1 Shift, 2 Ctrl, 4 Alt. Press only what is missing,
    // and release on the way back out in reverse order, so a modifier the viewer
    // is genuinely holding is left exactly as it was.
    static constexpr struct
    {
        int bit;
        int vk;
    } kNeeded[] = {{1, VK_SHIFT}, {2, VK_CONTROL}, {4, VK_MENU}};

    std::vector<INPUT> inputs;
    inputs.reserve(4);
    {
        std::lock_guard<std::mutex> lock(m_HeldMutex);
        for (const auto& mod : kNeeded) {
            if (!(needed & mod.bit)) continue;
            if (m_HeldKeys.count(mod.vk)) continue; // the viewer is holding it for real
            inputs.push_back(makeKeyInput(mod.vk, down, false));
        }
        if (down)
            m_HeldKeys.insert(vk);
        else
            m_HeldKeys.erase(vk);
    }
    // The character's own key sits inside the modifiers: pressed after they go
    // down, released before they come back up.
    if (down)
        inputs.push_back(makeKeyInput(vk, true, false));
    else
        inputs.insert(inputs.begin(), makeKeyInput(vk, false, false));
    sendBatch(inputs.data(), static_cast<int>(inputs.size()));
}

void Win32Input::injectText(const std::string& utf8)
{
    if (utf8.empty()) return;

    const int needed =
        ::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return;

    std::vector<wchar_t> wide(static_cast<size_t>(needed));
    if (::MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(),
                              needed) != needed)
        return;

    // KEYEVENTF_UNICODE delivers the character itself rather than a key, which
    // is what a soft keyboard needs: the host layout is irrelevant and no
    // scancode has to exist for the glyph. Surrogate pairs are two units and
    // Windows expects them as two consecutive events, which falls out of
    // walking the UTF-16 sequence.
    std::vector<INPUT> inputs;
    inputs.reserve(wide.size() * 2);
    for (wchar_t unit : wide) {
        INPUT down = {};
        down.type = INPUT_KEYBOARD;
        down.ki.wScan = static_cast<WORD>(unit);
        down.ki.dwFlags = KEYEVENTF_UNICODE;
        inputs.push_back(down);

        INPUT up = down;
        up.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        inputs.push_back(up);
    }
    sendBatch(inputs.data(), static_cast<int>(inputs.size()));
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

    // Relative motion passes through the pointer speed and acceleration the
    // host has configured, exactly as a local mouse would. That is the right
    // default — a game reading raw input bypasses it anyway, and a desktop user
    // expects their own settings — but it does mean two hosts with different
    // pointer settings feel different for the same client movement.
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
    if (standing.level < 0) return "";
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
    LONG absX = 0;
    LONG absY = 0;
    if (!toAbsolute(m_DisplayRect, event.positionX, event.positionY, event.referenceWidth,
                    event.referenceHeight, absX, absY))
        return;

    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dx = absX;
    input.mi.dy = absY;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
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

    sendMouseButton(button, down);
}

void Win32Input::sendMouseButton(int button, bool down)
{
    INPUT input = {};
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
    default: return;
    }

    sendOne(input);
}

void Win32Input::injectScroll(int amount, bool horizontal)
{
    if (amount == 0) return;

    // Already in 120-unit notches: the relay quantizes high-resolution wheel
    // deltas upstream, for every transport, so re-scaling here would apply the
    // conversion twice.
    INPUT input = {};
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = horizontal ? MOUSEEVENTF_HWHEEL : MOUSEEVENTF_WHEEL;
    input.mi.mouseData = static_cast<DWORD>(amount);
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

} // namespace mw::native::input
