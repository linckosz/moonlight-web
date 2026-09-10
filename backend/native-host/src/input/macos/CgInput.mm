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

#include "CgInput.h"

#include "../../core/Log.h"
#include "MacKeyMap.h"

#include <ApplicationServices/ApplicationServices.h>
#include <Carbon/Carbon.h>
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hidsystem/IOHIDLib.h>
#include <IOKit/hidsystem/IOHIDParameter.h>

#include <algorithm>
#include <chrono>
#include <vector>

namespace mw::native::input {
namespace {

int64_t steadyNowUs()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/// Presses closer than this on the same button count as one multi-click.
constexpr int64_t kDoubleClickUs = 500 * 1000;

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

// ── Keyboard diagnostics ────────────────────────────────────────────────────
//
// Only when NativeHost::setKeyboardDiagnostics(true) was called, only on a key
// down. Two verdicts, because a keystroke has two jobs that fail separately:
// Notepad (the character a text field shows) and Game (the physical key a title
// reading raw HID sees, named by its US label, which is how bindings read).
//
// As on Linux and unlike Windows, there is no round trip to make: m_CharMap is
// built FORWARD, by asking UCKeyTranslate what each key code of the active
// layout produces, so a character found in it is one this layout really types
// there. What the line adds is which key it landed on.

/// The US label of a CGKeyCode, found by searching the US table rather than
/// storing a second one the two could drift apart on.
std::string usKeyLabel(uint16_t code)
{
    for (int vk = 0x08; vk <= 0xFE; ++vk) {
        if (macKeyCode(vk) != code) continue;
        if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z'))
            return std::string(1, static_cast<char>(vk));
        switch (vk) {
        case 0x20: return "Space";
        case 0x0D: return "Return";
        case 0x09: return "Tab";
        case 0x08: return "Delete";
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
        default: break;
        }
        break;
    }
    return "key code " + std::to_string(code);
}

/// One UTF-16 unit as UTF-8, for a log line.
std::string utf8Of(uint16_t unit)
{
    const UniChar value = unit;
    CFStringRef text = CFStringCreateWithCharacters(kCFAllocatorDefault, &value, 1);
    if (!text) return std::string();
    char buffer[8] = {};
    const bool ok = CFStringGetCString(text, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    CFRelease(text);
    return ok ? std::string(buffer) : std::string();
}

/// The level modifiers a character's key needs, spelled out.
std::string modifierText(uint64_t flags)
{
    std::string out;
    if (flags & kCGEventFlagMaskShift) out += "+Shift";
    if (flags & kCGEventFlagMaskAlternate) out += "+Option";
    if (flags & kCGEventFlagMaskControl) out += "+Control";
    return out;
}

/// The modifier flag a virtual key sets, or 0 for an ordinary key.
CGEventFlags modifierFlag(int vk)
{
    switch (vk) {
    case 0x10:
    case 0xA0:
    case 0xA1: return kCGEventFlagMaskShift;
    case 0x11:
    case 0xA2:
    case 0xA3: return kCGEventFlagMaskControl;
    case 0x12:
    case 0xA4:
    case 0xA5: return kCGEventFlagMaskAlternate;
    case 0x5B:
    case 0x5C: return kCGEventFlagMaskCommand;
    case 0x14: return kCGEventFlagMaskAlphaShift;
    default: return 0;
    }
}

/// CoreGraphics' button number for the browser's: 0 left, 1 right, 2 middle,
/// then the extras.
CGMouseButton cgButton(int button)
{
    switch (button) {
    case 1: return kCGMouseButtonLeft;
    case 3: return kCGMouseButtonRight;
    case 2: return kCGMouseButtonCenter;
    case 4: return static_cast<CGMouseButton>(3);
    case 5: return static_cast<CGMouseButton>(4);
    default: return kCGMouseButtonLeft;
    }
}

void post(CGEventRef event)
{
    if (!event) return;
    CGEventPost(kCGHIDEventTap, event);
    CFRelease(event);
}

} // namespace

CgInput::~CgInput()
{
    stop();
}

bool CgInput::start(std::string& error)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Started) return true;

    // Accessibility. Without it every CGEventPost is accepted and ignored,
    // which from the browser is a stream that shows but does not answer. The
    // OS prompt is asked for once; the switch itself is the user's to flip
    // (System Settings → Privacy & Security → Accessibility).
    if (!CGPreflightPostEventAccess()) {
        CGRequestPostEventAccess();
        log::warning("[native] input: macOS has not granted Accessibility to this program — "
                     "keyboard and mouse will not reach the desktop until it is allowed in "
                     "System Settings › Privacy & Security › Accessibility");
    }

    // Where the pointer is now, so relative motion starts from the truth.
    if (CGEventRef probe = CGEventCreate(nullptr)) {
        const CGPoint at = CGEventGetLocation(probe);
        m_X = at.x;
        m_Y = at.y;
        CFRelease(probe);
    }
    m_Modifiers = 0;
    m_Started = true;
    log::info("[native] input: Quartz events on the display at " + std::to_string(m_Left) + "," +
              std::to_string(m_Top) + " " + std::to_string(m_Right - m_Left) + "x" +
              std::to_string(m_Bottom - m_Top) + " pt");
    (void)error;
    return true;
}

void CgInput::stop()
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Started) return;
    releaseAll();
    log::info("[native] input: " + std::to_string(m_Injected.load(std::memory_order_relaxed)) +
              " event(s) injected this session");
    m_Started = false;
}

void CgInput::setDisplayRect(int left, int top, int right, int bottom)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (left == m_Left && top == m_Top && right == m_Right && bottom == m_Bottom) return;
    m_Left = left;
    m_Top = top;
    m_Right = right;
    m_Bottom = bottom;
    if (m_Started)
        log::info("[native] input: display now at " + std::to_string(left) + "," +
                  std::to_string(top) + " " + std::to_string(right - left) + "x" +
                  std::to_string(bottom - top) + " pt");
}

void CgInput::releaseAll()
{
    // The caller holds m_Mutex.
    std::set<int> keys;
    std::set<int> buttons;
    std::set<uint16_t> charCodes;
    keys.swap(m_HeldKeys);
    buttons.swap(m_HeldButtons);
    charCodes.swap(m_HeldCharCodes);
    if (keys.empty() && buttons.empty() && charCodes.empty()) return;

    // Characters first: they were pressed as real keys of the host's layout and
    // would otherwise stay down, and their release wants the modifiers still in
    // the state they were pressed under.
    for (uint16_t code : charCodes) {
        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, code, false);
        CGEventSetFlags(up, static_cast<CGEventFlags>(m_Modifiers));
        post(up);
    }

    for (int vk : keys) {
        const uint16_t code = macKeyCode(vk);
        if (code == kMacNoKey) continue;
        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, code, false);
        if (const CGEventFlags flag = modifierFlag(vk)) {
            m_Modifiers &= ~flag;
            CGEventSetType(up, kCGEventFlagsChanged);
        }
        CGEventSetFlags(up, static_cast<CGEventFlags>(m_Modifiers));
        post(up);
    }
    for (int button : buttons)
        postButton(button, false, m_X, m_Y);
    m_Modifiers = 0;
    log::info("[native] input: released " + std::to_string(keys.size() + charCodes.size()) +
              " key(s) and " + std::to_string(buttons.size()) + " button(s) at session end");
}

void CgInput::inject(const InputEvent& event)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Started) return;

    const uint32_t bit = 1u << static_cast<int>(event.type);
    if ((m_SeenTypes.fetch_or(bit, std::memory_order_relaxed) & bit) == 0)
        log::info(std::string("[native] input: first ") + describe(event.type) + " injected");
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
    // No virtual gamepad on macOS (see §8 of the plan: a signed DriverKit
    // extension, which needs an Apple entitlement). Ignored, never mapped
    // onto the mouse.
    case InputEvent::Type::ControllerArrival:
    case InputEvent::Type::ControllerState:
    case InputEvent::Type::ControllerRemoval: break;
    }
}

void CgInput::injectKey(const InputEvent& event, bool down)
{
    const int vk = event.keyCode;
    if (vk <= 0 || vk > 0xFF) return;
    const uint16_t code = macKeyCode(vk);
    if (code == kMacNoKey) return;

    // A heartbeat re-press of a key still held would be an extra character;
    // a real repeat is passed through (the user holding a key wants typematic).
    if (down && event.resync && m_HeldKeys.count(vk)) return;
    if (down)
        m_HeldKeys.insert(vk);
    else
        m_HeldKeys.erase(vk);

    CGEventRef key = CGEventCreateKeyboardEvent(nullptr, code, down);
    if (!key) return;
    if (const CGEventFlags flag = modifierFlag(vk)) {
        // A modifier is a change of flags, not a keystroke: the event says
        // which key moved and carries the whole modifier state after it.
        if (vk == 0x14) {
            // Caps Lock toggles on press; its flag reflects the lock state.
            if (down) m_Modifiers ^= flag;
        } else if (down) {
            m_Modifiers |= flag;
        } else {
            m_Modifiers &= ~flag;
        }
        CGEventSetType(key, kCGEventFlagsChanged);
    }
    CGEventSetFlags(key, static_cast<CGEventFlags>(m_Modifiers));
    post(key);

    if (down && keyboardDiagnostics() && ensureCharMap()) {
        // Positional path: the key's US position went out and the host's layout
        // decides. No verdict — there is no client character here to check it
        // against; the transport's line for the same keystroke carries that,
        // and the two read together.
        for (const auto& entry : m_CharMap) {
            if (entry.second.code != code || entry.second.flags != 0) continue;
            log::info(
                "[KBD] host position key code " + std::to_string(code) + " (US '" +
                usKeyLabel(code) + "') -> " + m_CharMapSource + " types '" + utf8Of(entry.first) +
                "' | Notepad: no client character to check against | Game: OK real key, US '" +
                usKeyLabel(code) + "'");
            break;
        }
    }
}

bool CgInput::ensureCharMap()
{
    // Rebuilt whenever the active input source changes, because the viewer — or
    // the host's own user — can switch layouts mid-stream and every key code in
    // the map would then mean a different character.
    TISInputSourceRef source = TISCopyCurrentKeyboardLayoutInputSource();
    if (!source) return !m_CharMap.empty();

    std::string id;
    if (auto* name = static_cast<CFStringRef>(
            TISGetInputSourceProperty(source, kTISPropertyInputSourceID))) {
        char buffer[256] = {};
        if (CFStringGetCString(name, buffer, sizeof(buffer), kCFStringEncodingUTF8)) id = buffer;
    }
    if (!id.empty() && id == m_CharMapSource) {
        CFRelease(source);
        return !m_CharMap.empty();
    }

    auto* data =
        static_cast<CFDataRef>(TISGetInputSourceProperty(source, kTISPropertyUnicodeKeyLayoutData));
    if (!data) {
        CFRelease(source);
        return false;
    }
    const auto* layout = reinterpret_cast<const UCKeyboardLayout*>(CFDataGetBytePtr(data));

    m_CharMap.clear();
    m_CharMapSource = id;
    // The four levels a single key can carry on a Mac layout. Listed cheapest
    // first so a character reachable without modifiers wins over the same
    // character behind Option — first writer keeps the slot.
    static constexpr struct
    {
        uint32_t carbon; ///< UCKeyTranslate's modifier field (already >> 8)
        CGEventFlags flags;
    } kLevels[] = {
        {0, 0},
        {shiftKey >> 8, kCGEventFlagMaskShift},
        {optionKey >> 8, kCGEventFlagMaskAlternate},
        {(shiftKey | optionKey) >> 8, kCGEventFlagMaskShift | kCGEventFlagMaskAlternate},
    };

    for (uint16_t code = 0; code < 128; ++code) {
        for (const auto& level : kLevels) {
            UInt32 deadState = 0;
            UniChar chars[4] = {};
            UniCharCount length = 0;
            if (UCKeyTranslate(layout, code, kUCKeyActionDown, level.carbon, LMGetKbdType(),
                               kUCKeyTranslateNoDeadKeysBit, &deadState, 4, &length,
                               chars) != noErr)
                continue;
            // One unit only: a dead key produces none, and anything longer is
            // not something a single key press expresses.
            if (length != 1 || chars[0] < 0x20 || chars[0] == 0x7f) continue;
            m_CharMap.emplace(chars[0], CharKey{code, level.flags});
        }
    }
    CFRelease(source);
    log::info("[native] input: host keyboard layout " +
              (id.empty() ? std::string("(unnamed)") : id) + ", " +
              std::to_string(m_CharMap.size()) + " characters reachable as real keys");
    return !m_CharMap.empty();
}

void CgInput::injectChar(const std::string& utf8, bool down)
{
    if (utf8.empty()) return;

    CFStringRef text =
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()),
                                static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
    if (!text) return;
    const bool single = CFStringGetLength(text) == 1;
    const UniChar unit = single ? CFStringGetCharacterAtIndex(text, 0) : 0;
    CFRelease(text);

    const auto it = single && ensureCharMap() ? m_CharMap.find(unit) : m_CharMap.end();
    if (it == m_CharMap.end()) {
        // No key on this layout carries the character — type it as Unicode
        // instead, which needs no key at all. The press does it; the release
        // has nothing left to do.
        if (down) {
            injectText(utf8);
            if (keyboardDiagnostics())
                log::warning("[KBD] host '" + utf8 +
                             "' -> no key on this layout | Notepad: OK as Unicode text | Game: KO "
                             "nothing was pressed");
        }
        return;
    }

    CGEventRef key = CGEventCreateKeyboardEvent(nullptr, it->second.code, down);
    if (!key) return;
    // The LEVEL modifiers — Shift and Option — are the layout's decision, not
    // the viewer's hand's: the viewer pressed Shift for their own layout, where
    // "1" on AZERTY is Shift+&, and on a US host that character wants no Shift
    // at all. Left in the flags, the viewer's Shift turned every AZERTY digit
    // into a US symbol. So the viewer's level flags are replaced by the map's,
    // and only those: Command and Control are chords (Cmd+A) and ride through
    // untouched. On macOS a modifier is a flag on the event, so nothing has to
    // be pressed or released around the key — and nothing has to be put back.
    constexpr CGEventFlags kLevel = kCGEventFlagMaskShift | kCGEventFlagMaskAlternate;
    CGEventSetFlags(key, (static_cast<CGEventFlags>(m_Modifiers) & ~kLevel) | it->second.flags);
    post(key);

    if (down)
        m_HeldCharCodes.insert(it->second.code);
    else
        m_HeldCharCodes.erase(it->second.code);

    if (down && keyboardDiagnostics()) {
        const std::string label = usKeyLabel(it->second.code);
        log::info("[KBD] host '" + utf8 + "' -> key code " + std::to_string(it->second.code) +
                  modifierText(it->second.flags) + " on " + m_CharMapSource +
                  " | Notepad: OK | Game: OK real key, US '" + label + "'");
    }
}

void CgInput::injectText(const std::string& utf8)
{
    if (utf8.empty()) return;
    CFStringRef text =
        CFStringCreateWithBytes(kCFAllocatorDefault, reinterpret_cast<const UInt8*>(utf8.data()),
                                static_cast<CFIndex>(utf8.size()), kCFStringEncodingUTF8, false);
    if (!text) return;
    const CFIndex length = CFStringGetLength(text);
    std::vector<UniChar> units(static_cast<size_t>(length));
    CFStringGetCharacters(text, CFRangeMake(0, length), units.data());
    CFRelease(text);

    // The character itself rather than a key: a soft keyboard's glyph needs
    // no key code to exist for it, and the host layout is irrelevant. One
    // event per UTF-16 unit, down then up, the way the window server wants
    // typed text delivered.
    for (UniChar unit : units) {
        CGEventRef down = CGEventCreateKeyboardEvent(nullptr, 0, true);
        CGEventKeyboardSetUnicodeString(down, 1, &unit);
        CGEventSetFlags(down, static_cast<CGEventFlags>(m_Modifiers));
        post(down);
        CGEventRef up = CGEventCreateKeyboardEvent(nullptr, 0, false);
        CGEventKeyboardSetUnicodeString(up, 1, &unit);
        CGEventSetFlags(up, static_cast<CGEventFlags>(m_Modifiers));
        post(up);
    }
}

void CgInput::moveTo(double x, double y, int deltaX, int deltaY)
{
    // Kept on the display: a pointer that wanders onto another screen from a
    // stream of this one is the classic remote-desktop surprise.
    if (m_Right > m_Left && m_Bottom > m_Top) {
        x = std::min(std::max(x, static_cast<double>(m_Left)), static_cast<double>(m_Right - 1));
        y = std::min(std::max(y, static_cast<double>(m_Top)), static_cast<double>(m_Bottom - 1));
    }
    m_X = x;
    m_Y = y;
    postPointer(x, y, deltaX, deltaY);
}

void CgInput::postPointer(double x, double y, int deltaX, int deltaY)
{
    // A move with a button held is a drag, and the event has to say so — the
    // window server does not infer it from the buttons it saw go down.
    CGEventType type = kCGEventMouseMoved;
    CGMouseButton button = kCGMouseButtonLeft;
    if (m_HeldButtons.count(1)) {
        type = kCGEventLeftMouseDragged;
    } else if (m_HeldButtons.count(3)) {
        type = kCGEventRightMouseDragged;
        button = kCGMouseButtonRight;
    } else if (!m_HeldButtons.empty()) {
        type = kCGEventOtherMouseDragged;
        button = cgButton(*m_HeldButtons.begin());
    }
    CGEventRef move = CGEventCreateMouseEvent(nullptr, type, CGPointMake(x, y), button);
    if (!move) return;
    // The deltas are what a game reading raw motion sees — with the pointer
    // decoupled from the cursor (a captured mouse) the position is ignored
    // and only these move the camera.
    CGEventSetIntegerValueField(move, kCGMouseEventDeltaX, deltaX);
    CGEventSetIntegerValueField(move, kCGMouseEventDeltaY, deltaY);
    CGEventSetFlags(move, static_cast<CGEventFlags>(m_Modifiers));
    post(move);
}

void CgInput::injectMouseMove(int deltaX, int deltaY)
{
    if (deltaX == 0 && deltaY == 0) return;
    // Points and mouse counts are taken as one and the same, which is what
    // a local mouse at the default speed does. Acceleration is the host's.
    moveTo(m_X + deltaX, m_Y + deltaY, deltaX, deltaY);
}

void CgInput::injectMousePosition(const InputEvent& event)
{
    if (event.referenceWidth <= 0 || event.referenceHeight <= 0) return;
    if (m_Right <= m_Left || m_Bottom <= m_Top) return;
    const double x =
        m_Left + static_cast<double>(event.positionX) * (m_Right - m_Left) / event.referenceWidth;
    const double y =
        m_Top + static_cast<double>(event.positionY) * (m_Bottom - m_Top) / event.referenceHeight;
    moveTo(x, y, static_cast<int>(x - m_X), static_cast<int>(y - m_Y));
}

void CgInput::injectMouseButton(int button, bool down)
{
    if (button < 1 || button > 5) return;
    // Never pressed twice without a release (see Win32Input: a trackpad tap
    // arrived as a double click through the heartbeat).
    if (down == (m_HeldButtons.count(button) != 0)) return;
    if (down)
        m_HeldButtons.insert(button);
    else
        m_HeldButtons.erase(button);
    postButton(button, down, m_X, m_Y);
}

void CgInput::postButton(int button, bool down, double x, double y)
{
    CGEventType type;
    switch (button) {
    case 1: type = down ? kCGEventLeftMouseDown : kCGEventLeftMouseUp; break;
    case 3: type = down ? kCGEventRightMouseDown : kCGEventRightMouseUp; break;
    default: type = down ? kCGEventOtherMouseDown : kCGEventOtherMouseUp; break;
    }
    // The click count macOS wants declared: a second press of the same button
    // within the interval is click 2, and its release repeats the count.
    if (down) {
        const int64_t now = steadyNowUs();
        if (button == m_LastButton && now - m_LastPressUs < kDoubleClickUs)
            m_ClickCount = std::min(m_ClickCount + 1, 3);
        else
            m_ClickCount = 1;
        m_LastButton = button;
        m_LastPressUs = now;
    }
    CGEventRef click = CGEventCreateMouseEvent(nullptr, type, CGPointMake(x, y), cgButton(button));
    if (!click) return;
    CGEventSetIntegerValueField(click, kCGMouseEventClickState,
                                m_ClickCount > 0 ? m_ClickCount : 1);
    CGEventSetFlags(click, static_cast<CGEventFlags>(m_Modifiers));
    post(click);
}

void CgInput::injectScroll(int amount, bool horizontal)
{
    if (amount == 0) return;
    // 120-unit notches from the relay, one line per notch here. Vertical
    // signs agree between Windows and Quartz (positive is away from the
    // user); horizontal is mirrored.
    const int32_t lines = amount / 120 != 0 ? amount / 120 : (amount > 0 ? 1 : -1);
    CGEventRef wheel =
        horizontal ? CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 2, 0, -lines)
                   : CGEventCreateScrollWheelEvent(nullptr, kCGScrollEventUnitLine, 1, lines);
    if (!wheel) return;
    CGEventSetFlags(wheel, static_cast<CGEventFlags>(m_Modifiers));
    post(wheel);
}

void CgInput::syncLockKeys(const InputEvent& event)
{
    // Only Caps Lock exists on a Mac. It is state, not a keystroke, and IOKit
    // sets it directly — a synthetic press would toggle a host that already
    // matched.
    io_service_t service =
        IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching(kIOHIDSystemClass));
    if (!service) return;
    io_connect_t connect = 0;
    if (IOServiceOpen(service, mach_task_self(), kIOHIDParamConnectType, &connect) ==
        KERN_SUCCESS) {
        bool current = false;
        if (IOHIDGetModifierLockState(connect, kIOHIDCapsLockState, &current) == KERN_SUCCESS &&
            current != event.capsLock)
            IOHIDSetModifierLockState(connect, kIOHIDCapsLockState, event.capsLock);
        IOServiceClose(connect);
    }
    IOObjectRelease(service);
    if (event.capsLock)
        m_Modifiers |= kCGEventFlagMaskAlphaShift;
    else
        m_Modifiers &= ~static_cast<uint64_t>(kCGEventFlagMaskAlphaShift);
}

} // namespace mw::native::input
