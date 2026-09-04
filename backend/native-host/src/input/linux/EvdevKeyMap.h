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

/// Virtual key → Linux evdev key code, on the US English layout ONLY.
///
/// ── Why a table, and why the same reasoning as Windows ──────────────────────
///
/// The browser sends the POSITION of the key it saw, expressed as the virtual
/// key that position carries on a US keyboard — that is what "normalized" means
/// in the protocol, and what every Moonlight client does. evdev codes are
/// positions too, so this is a position-to-position map and the host's own
/// layout must not enter into it: X11 or the Wayland compositor applies the
/// user's layout on top, exactly as it does for a real keyboard. A French host
/// therefore types French from an AZERTY client, without this table knowing
/// anything about either.
///
/// ── Why the numbers are spelled out ────────────────────────────────────────
///
/// The values are written as literals rather than as KEY_* macros so this
/// header compiles — and can be TESTED — on a machine that has no Linux headers
/// at all, which is every machine this engine has been developed on so far. The
/// codes are a kernel ABI: they are fixed for as long as Linux keeps running
/// existing binaries, which is longer than this file will exist.
///
/// That is a claim, so it is checked rather than asserted: when this does get
/// compiled on Linux, EvdevKeyMap.cpp cross-checks a sample of the table
/// against <linux/input-event-codes.h> at compile time. A drift breaks the
/// build there instead of typing the wrong letter on someone's desktop.
///
/// @returns the evdev code, or 0 for a key Linux does not name at a position
///          (media keys mostly), which the caller drops.
/// The alphabet is scattered over three rows, so it is the one part that has to
/// be spelled out. In VK order, A (0x41) through Z (0x5A).
///
/// At namespace scope rather than inside the function: a `static constexpr`
/// local in a constexpr function is C++23, and this module is C++17.
constexpr uint16_t kEvdevLetters[26] = {
    30, 48, 46, 32, 18, 33, 34, 35, 23, 36, 37, 38, 50,
    49, 24, 25, 16, 19, 31, 20, 22, 47, 17, 45, 21, 44,
};

/// Numpad, in VK order (VK_NUMPAD0 0x60 through VK_NUMPAD9). Its digits are
/// laid out bottom-up on the keypad, so they are not contiguous either.
constexpr uint16_t kEvdevNumpad[10] = {
    82, 79, 80, 81, 75, 76, 77, 71, 72, 73,
};

/// constexpr so the cross-check in UinputInput.cpp can be a static_assert: a
/// drift then breaks the build rather than a keyboard.
constexpr uint16_t evdevKeyCode(int vk)
{
    if (vk >= 0x41 && vk <= 0x5A) return kEvdevLetters[vk - 0x41];
    if (vk >= 0x60 && vk <= 0x69) return kEvdevNumpad[vk - 0x60];
    // Digit row: 1..9 run 2..10 and 0 closes the row rather than opening it.
    if (vk == 0x30) return 11;
    if (vk > 0x30 && vk <= 0x39) return static_cast<uint16_t>(2 + (vk - 0x31));
    // F1..F10 are contiguous; F11 and F12 were added later, elsewhere.
    if (vk >= 0x70 && vk <= 0x79) return static_cast<uint16_t>(59 + (vk - 0x70));

    // One key per line, grouped as they sit on the board: a table like this is
    // only checkable by eye if it reads like the thing it describes.
    // clang-format off
    switch (vk) {
    // Typewriter keys, reading the US board left to right, top to bottom.
    case 0x1B: return 1;   // VK_ESCAPE       KEY_ESC
    case 0xC0: return 41;  // VK_OEM_3        `~      KEY_GRAVE
    case 0xBD: return 12;  // VK_OEM_MINUS    -_      KEY_MINUS
    case 0xBB: return 13;  // VK_OEM_PLUS     =+      KEY_EQUAL
    case 0x08: return 14;  // VK_BACK                 KEY_BACKSPACE
    case 0x09: return 15;  // VK_TAB                  KEY_TAB
    case 0xDB: return 26;  // VK_OEM_4        [{      KEY_LEFTBRACE
    case 0xDD: return 27;  // VK_OEM_6        ]}      KEY_RIGHTBRACE
    case 0xDC: return 43;  // VK_OEM_5        \|      KEY_BACKSLASH
    case 0x14: return 58;  // VK_CAPITAL              KEY_CAPSLOCK
    case 0xBA: return 39;  // VK_OEM_1        ;:      KEY_SEMICOLON
    case 0xDE: return 40;  // VK_OEM_7        '"      KEY_APOSTROPHE
    case 0x0D: return 28;  // VK_RETURN               KEY_ENTER
    case 0xBC: return 51;  // VK_OEM_COMMA    ,<      KEY_COMMA
    case 0xBE: return 52;  // VK_OEM_PERIOD   .>      KEY_DOT
    case 0xBF: return 53;  // VK_OEM_2        /?      KEY_SLASH
    case 0x20: return 57;  // VK_SPACE                KEY_SPACE

    // Modifiers. The browser distinguishes left from right; so does evdev.
    case 0xA0: return 42;  // VK_LSHIFT               KEY_LEFTSHIFT
    case 0xA1: return 54;  // VK_RSHIFT               KEY_RIGHTSHIFT
    case 0xA2: return 29;  // VK_LCONTROL             KEY_LEFTCTRL
    case 0xA3: return 97;  // VK_RCONTROL             KEY_RIGHTCTRL
    case 0xA4: return 56;  // VK_LMENU        Alt     KEY_LEFTALT
    case 0xA5: return 100; // VK_RMENU        AltGr   KEY_RIGHTALT
    case 0x5B: return 125; // VK_LWIN                 KEY_LEFTMETA
    case 0x5C: return 126; // VK_RWIN                 KEY_RIGHTMETA
    case 0x5D: return 127; // VK_APPS         menu    KEY_COMPOSE
    // The undifferentiated forms, which a client may still send: pick left.
    case 0x10: return 42;  // VK_SHIFT
    case 0x11: return 29;  // VK_CONTROL
    case 0x12: return 56;  // VK_MENU

    // Function row beyond F10, and the keys above the arrows.
    case 0x7A: return 87;  // VK_F11                  KEY_F11
    case 0x7B: return 88;  // VK_F12                  KEY_F12
    case 0x2C: return 99;  // VK_SNAPSHOT     PrtSc   KEY_SYSRQ
    case 0x91: return 70;  // VK_SCROLL               KEY_SCROLLLOCK
    case 0x13: return 119; // VK_PAUSE                KEY_PAUSE
    case 0x2D: return 110; // VK_INSERT               KEY_INSERT
    case 0x2E: return 111; // VK_DELETE               KEY_DELETE
    case 0x24: return 102; // VK_HOME                 KEY_HOME
    case 0x23: return 107; // VK_END                  KEY_END
    case 0x21: return 104; // VK_PRIOR        PgUp    KEY_PAGEUP
    case 0x22: return 109; // VK_NEXT         PgDn    KEY_PAGEDOWN

    // Arrows.
    case 0x25: return 105; // VK_LEFT                 KEY_LEFT
    case 0x26: return 103; // VK_UP                   KEY_UP
    case 0x27: return 106; // VK_RIGHT                KEY_RIGHT
    case 0x28: return 108; // VK_DOWN                 KEY_DOWN

    // Numeric keypad, the non-digit keys.
    case 0x90: return 69;  // VK_NUMLOCK              KEY_NUMLOCK
    case 0x6F: return 98;  // VK_DIVIDE               KEY_KPSLASH
    case 0x6A: return 55;  // VK_MULTIPLY             KEY_KPASTERISK
    case 0x6D: return 74;  // VK_SUBTRACT             KEY_KPMINUS
    case 0x6B: return 78;  // VK_ADD                  KEY_KPPLUS
    case 0x6E: return 83;  // VK_DECIMAL              KEY_KPDOT

    // The extra key on 102-key boards, between left shift and Z.
    case 0xE2: return 86;  // VK_OEM_102              KEY_102ND
    default: break;
    }
    // clang-format on
    return 0;
}

} // namespace mw::native::input
