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

#include <windows.h>

#include <algorithm>
#include <atomic>
#include <string>
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
/// The failure that matters is UIPI: an unelevated process cannot inject into
/// an elevated window, so input stops working the moment such a window takes
/// focus — Task Manager, an installer, a game launched as administrator. It
/// looks exactly like a frozen session from the browser, so it has to be said
/// once, plainly, rather than left to be guessed.
void sendBatch(INPUT* inputs, int count)
{
    if (count <= 0) return;
    const UINT sent = ::SendInput(static_cast<UINT>(count), inputs, sizeof(INPUT));
    if (sent == static_cast<UINT>(count)) return;

    static std::atomic<bool> reported{false};
    bool expected = false;
    if (!reported.compare_exchange_strong(expected, true)) return;

    const DWORD err = ::GetLastError();
    if (err == ERROR_ACCESS_DENIED) {
        log::warning("[native] input refused by Windows (UIPI) — the focused window runs at a "
                     "higher integrity level than MoonlightWeb; run MoonlightWeb elevated to "
                     "control it");
    } else {
        log::warning("[native] SendInput refused an event (error " + std::to_string(err) + ")");
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
/// when the key has no scancode on this layout — a dead mapping injected as
/// scancode 0 would land as a keypress nobody asked for.
INPUT makeKeyInput(int vk, bool down, bool nonNormalized)
{
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.dwFlags = down ? 0 : KEYEVENTF_KEYUP;

    const UINT mapped =
        nonNormalized ? 0u : ::MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC_EX);
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

/// Map a point on the captured display to SendInput's absolute space.
///
/// That space is 0..65535 across the WHOLE virtual desktop, not across one
/// screen, so the display's origin has to be added before normalising —
/// otherwise every secondary monitor would be aimed at as if it were the
/// primary. Both rectangles come from the same DPI-virtualized coordinate
/// system (see DesktopRect), so no scaling correction belongs here.
bool toAbsolute(const capture::DesktopRect& rect, int x, int y, int refW, int refH, LONG& outX,
                LONG& outY)
{
    if (refW <= 0 || refH <= 0 || !rect.valid()) return false;

    const int virtualLeft = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int virtualTop = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int virtualWidth = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int virtualHeight = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (virtualWidth <= 1 || virtualHeight <= 1) return false;

    // Clamp to the display: a client whose aspect ratio differs slightly can
    // report a point a pixel or two outside, and letting that through would
    // walk the cursor onto the neighbouring screen.
    const int64_t onDisplayX = rect.left + (static_cast<int64_t>(x) * rect.width()) / refW;
    const int64_t onDisplayY = rect.top + (static_cast<int64_t>(y) * rect.height()) / refH;
    const int64_t clampedX =
        std::min<int64_t>(std::max<int64_t>(onDisplayX, rect.left), rect.right - 1);
    const int64_t clampedY =
        std::min<int64_t>(std::max<int64_t>(onDisplayY, rect.top), rect.bottom - 1);

    outX = static_cast<LONG>(((clampedX - virtualLeft) * 65535) / (virtualWidth - 1));
    outY = static_cast<LONG>(((clampedY - virtualTop) * 65535) / (virtualHeight - 1));
    return true;
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
    releaseAll();
    // Unplugged before anything else: a virtual pad that outlived its session
    // would sit in the Windows game controller list forever, and the next game
    // to start would see a controller nobody is holding.
    m_Gamepad.reset();
    log::info("[native] input: " + std::to_string(m_Injected.load(std::memory_order_relaxed)) +
              " event(s) injected this session");
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
    m_Injected.fetch_add(1, std::memory_order_relaxed);

    switch (event.type) {
    case InputEvent::Type::KeyDown: injectKey(event, true); break;
    case InputEvent::Type::KeyUp: injectKey(event, false); break;
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
