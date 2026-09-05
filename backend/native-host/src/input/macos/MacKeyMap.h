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

#pragma once

#include <cstdint>

namespace mw::native::input {

/// Virtual key → macOS virtual key code (the kVK_* of Carbon's Events.h), on
/// the US ANSI layout ONLY.
///
/// ── The same reasoning as the Windows and Linux tables ──────────────────────
///
/// The browser sends the POSITION of the key, as the US virtual key that
/// position carries. macOS key codes are positions too — kVK_ANSI_A is the key
/// left of S whatever is printed on it — so this is a position-to-position map,
/// and the host's own layout is applied by the OS on top exactly as it is for a
/// real keyboard. A French host types French from an AZERTY client without
/// this table knowing about either.
///
/// ── Why the numbers are spelled out ─────────────────────────────────────────
///
/// Literals rather than kVK_* macros so the header compiles — and is tested —
/// on every machine the suite runs on, not only a Mac. The codes are Apple's
/// ADB scan codes, unchanged since the original Macintosh keyboard; they are
/// not going to move.
///
/// ── What the modifiers become ───────────────────────────────────────────────
///
/// Windows's Win key is the Command key, Alt is Option, Ctrl is Control. That
/// is how every Moonlight client on a Mac maps them and how a Mac user expects
/// a PC keyboard to behave when plugged in. The application-menu key has no
/// Mac equivalent and is dropped.
///
/// @returns the key code, or 0xFFFF for a key macOS does not name at a
///          position (browser and media keys), which the caller drops. 0 is
///          kVK_ANSI_A and therefore cannot be the "no key" value.
constexpr uint16_t kMacNoKey = 0xFFFF;

/// The alphabet in VK order, A (0x41) through Z (0x5A). Scattered over the
/// ANSI code space the way the original keyboard's matrix was wired.
constexpr uint16_t kMacLetters[26] = {
    0x00, 0x0B, 0x08, 0x02, 0x0E, 0x03, 0x05, 0x04, 0x22, 0x26, 0x28, 0x25, 0x2E,
    0x2D, 0x1F, 0x23, 0x0C, 0x0F, 0x01, 0x11, 0x20, 0x09, 0x0D, 0x07, 0x10, 0x06,
};

/// The digit row in VK order, 0 (0x30) through 9 (0x39). Not contiguous.
constexpr uint16_t kMacDigits[10] = {
    0x1D, 0x12, 0x13, 0x14, 0x15, 0x17, 0x16, 0x1A, 0x1C, 0x19,
};

/// The keypad in VK order, NUMPAD0 (0x60) through NUMPAD9 (0x69).
constexpr uint16_t kMacKeypad[10] = {
    0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5B, 0x5C,
};

constexpr uint16_t macKeyCode(int vk)
{
    if (vk >= 0x41 && vk <= 0x5A) return kMacLetters[vk - 0x41];
    if (vk >= 0x30 && vk <= 0x39) return kMacDigits[vk - 0x30];
    if (vk >= 0x60 && vk <= 0x69) return kMacKeypad[vk - 0x60];

    // One key per line, grouped as they sit on the board: a table like this is
    // only checkable by eye if it reads like the thing it describes.
    // clang-format off
    switch (vk) {
    // Typewriter keys, reading the US board left to right, top to bottom.
    case 0x1B: return 0x35; // VK_ESCAPE       kVK_Escape
    case 0xC0: return 0x32; // VK_OEM_3   `~   kVK_ANSI_Grave
    case 0xBD: return 0x1B; // VK_OEM_MINUS -_ kVK_ANSI_Minus
    case 0xBB: return 0x18; // VK_OEM_PLUS  =+ kVK_ANSI_Equal
    case 0x08: return 0x33; // VK_BACK         kVK_Delete (backspace)
    case 0x09: return 0x30; // VK_TAB          kVK_Tab
    case 0xDB: return 0x21; // VK_OEM_4   [{   kVK_ANSI_LeftBracket
    case 0xDD: return 0x1E; // VK_OEM_6   ]}   kVK_ANSI_RightBracket
    case 0xDC: return 0x2A; // VK_OEM_5   \|   kVK_ANSI_Backslash
    case 0x14: return 0x39; // VK_CAPITAL      kVK_CapsLock
    case 0xBA: return 0x29; // VK_OEM_1   ;:   kVK_ANSI_Semicolon
    case 0xDE: return 0x27; // VK_OEM_7   '"   kVK_ANSI_Quote
    case 0x0D: return 0x24; // VK_RETURN       kVK_Return
    case 0x10: return 0x38; // VK_SHIFT        kVK_Shift
    case 0xA0: return 0x38; // VK_LSHIFT       kVK_Shift
    case 0xBC: return 0x2B; // VK_OEM_COMMA ,< kVK_ANSI_Comma
    case 0xBE: return 0x2F; // VK_OEM_PERIOD . kVK_ANSI_Period
    case 0xBF: return 0x2C; // VK_OEM_2   /?   kVK_ANSI_Slash
    case 0xA1: return 0x3C; // VK_RSHIFT       kVK_RightShift
    case 0x11: return 0x3B; // VK_CONTROL      kVK_Control
    case 0xA2: return 0x3B; // VK_LCONTROL     kVK_Control
    case 0x5B: return 0x37; // VK_LWIN         kVK_Command
    case 0x12: return 0x3A; // VK_MENU         kVK_Option
    case 0xA4: return 0x3A; // VK_LMENU        kVK_Option
    case 0x20: return 0x31; // VK_SPACE        kVK_Space
    case 0xA5: return 0x3D; // VK_RMENU        kVK_RightOption
    case 0x5C: return 0x36; // VK_RWIN         kVK_RightCommand
    case 0xA3: return 0x3E; // VK_RCONTROL     kVK_RightControl
    case 0xE2: return 0x0A; // VK_OEM_102 <>   kVK_ISO_Section (the 102nd key)

    // The function row. F1..F12 are not contiguous on the Mac either.
    case 0x70: return 0x7A; // F1
    case 0x71: return 0x78; // F2
    case 0x72: return 0x63; // F3
    case 0x73: return 0x76; // F4
    case 0x74: return 0x60; // F5
    case 0x75: return 0x61; // F6
    case 0x76: return 0x62; // F7
    case 0x77: return 0x64; // F8
    case 0x78: return 0x65; // F9
    case 0x79: return 0x6D; // F10
    case 0x7A: return 0x67; // F11
    case 0x7B: return 0x6F; // F12
    case 0x7C: return 0x69; // F13 (Print Screen's place on an Apple board)
    case 0x7D: return 0x6B; // F14 (Scroll Lock's)
    case 0x7E: return 0x71; // F15 (Pause's)
    case 0x7F: return 0x6A; // F16
    case 0x80: return 0x40; // F17
    case 0x81: return 0x4F; // F18
    case 0x82: return 0x50; // F19
    case 0x83: return 0x5A; // F20
    case 0x2C: return 0x69; // VK_SNAPSHOT     F13 — where an Apple keyboard puts it
    case 0x91: return 0x6B; // VK_SCROLL       F14
    case 0x13: return 0x71; // VK_PAUSE        F15

    // The navigation cluster.
    case 0x2D: return 0x72; // VK_INSERT       kVK_Help (the Insert position on an extended board)
    case 0x2E: return 0x75; // VK_DELETE       kVK_ForwardDelete
    case 0x24: return 0x73; // VK_HOME         kVK_Home
    case 0x23: return 0x77; // VK_END          kVK_End
    case 0x21: return 0x74; // VK_PRIOR        kVK_PageUp
    case 0x22: return 0x79; // VK_NEXT         kVK_PageDown
    case 0x25: return 0x7B; // VK_LEFT         kVK_LeftArrow
    case 0x27: return 0x7C; // VK_RIGHT        kVK_RightArrow
    case 0x26: return 0x7E; // VK_UP           kVK_UpArrow
    case 0x28: return 0x7D; // VK_DOWN         kVK_DownArrow

    // The keypad, around the digits.
    case 0x90: return 0x47; // VK_NUMLOCK      kVK_ANSI_KeypadClear (Num Lock's key)
    case 0x6F: return 0x4B; // VK_DIVIDE       kVK_ANSI_KeypadDivide
    case 0x6A: return 0x43; // VK_MULTIPLY     kVK_ANSI_KeypadMultiply
    case 0x6D: return 0x4E; // VK_SUBTRACT     kVK_ANSI_KeypadMinus
    case 0x6B: return 0x45; // VK_ADD          kVK_ANSI_KeypadPlus
    case 0x6E: return 0x41; // VK_DECIMAL      kVK_ANSI_KeypadDecimal
    case 0x6C: return 0x5F; // VK_SEPARATOR    kVK_JIS_KeypadComma
    case 0x0C: return 0x47; // VK_CLEAR        kVK_ANSI_KeypadClear

    // Media keys ride on a different HID page on the Mac; not reachable by
    // key code, so dropped rather than typed as something else.
    default: return kMacNoKey;
    }
    // clang-format on
}

/// Whether @p vk is a modifier: pressed, it changes the FLAGS of every event
/// that follows rather than typing anything, and macOS wants it posted as a
/// flags-changed event with the new modifier state.
constexpr bool macIsModifier(int vk)
{
    switch (vk) {
    case 0x10:
    case 0xA0:
    case 0xA1: // shift
    case 0x11:
    case 0xA2:
    case 0xA3: // control
    case 0x12:
    case 0xA4:
    case 0xA5: // option
    case 0x5B:
    case 0x5C: // command
    case 0x14: // caps lock
        return true;
    default: return false;
    }
}

} // namespace mw::native::input
