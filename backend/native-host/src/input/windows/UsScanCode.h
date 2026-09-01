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

/// Virtual key → set-1 scancode, on the US English layout ONLY.
///
/// The browser sends the position of the key it saw, expressed as the virtual
/// key that position carries on a US keyboard — that is what "normalized"
/// means in the protocol, and what every Moonlight client does. Getting back to
/// the position therefore requires the US layout, and nothing else.
///
/// Windows offers MapVirtualKeyW for this, but it answers with the layout of
/// the CALLING THREAD. On a French host that reads the question backwards: the
/// client's physical A key arrives as VK_Q, MapVirtualKey hands back the
/// scancode of the key that types Q on AZERTY — the physical A of a QWERTY
/// board — and the host types "q". Every AZERTY key came out at its QWERTY
/// position, which is exactly what a user sees as "the host is stuck in
/// English". The table below is the layout the protocol actually names, so the
/// conversion no longer depends on how the host is configured.
///
/// The obvious alternative, MapVirtualKeyExW against a US HKL, needs
/// LoadKeyboardLayout to obtain that HKL — which adds English (US) to the
/// user's own input-language list. A table costs nothing and changes nothing on
/// the host.
///
/// @returns the scancode, 0xE0-prefixed for extended keys exactly as
///          MAPVK_VK_TO_VSC_EX reports them, or 0 for a key the US layout does
///          not name (media and browser keys, Pause) — those are layout
///          independent, and the caller falls back to Windows for them.
inline uint16_t usScanCode(int vk)
{
    // The alphabet is scattered over three rows, so it is the one part that has
    // to be spelled out. In VK order, A (0x41) through Z (0x5A).
    static constexpr uint16_t kLetters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C,
    };
    // Numpad, in VK order (VK_NUMPAD0 0x60 through VK_NUMPAD9). Its digits are
    // laid out bottom-up on the keypad, so they are not contiguous either.
    static constexpr uint16_t kNumpad[10] = {
        0x52, 0x4F, 0x50, 0x51, 0x4B, 0x4C, 0x4D, 0x47, 0x48, 0x49,
    };

    if (vk >= 0x41 && vk <= 0x5A) return kLetters[vk - 0x41];
    if (vk >= 0x60 && vk <= 0x69) return kNumpad[vk - 0x60];
    // Digit row: 1..9 run 0x02..0x0A and 0 closes the row rather than opening it.
    if (vk == 0x30) return 0x0B;
    if (vk > 0x30 && vk <= 0x39) return static_cast<uint16_t>(0x02 + (vk - 0x31));
    // F1..F10 are contiguous; F11 and F12 were added later, elsewhere.
    if (vk >= 0x70 && vk <= 0x79) return static_cast<uint16_t>(0x3B + (vk - 0x70));

    // One key per line, grouped as they sit on the board: a table like this is
    // only checkable by eye if it reads like the thing it describes.
    // clang-format off
    switch (vk) {
    // Typewriter keys, reading the US board left to right, top to bottom.
    case 0x1B: return 0x0001; // VK_ESCAPE
    case 0xC0: return 0x0029; // VK_OEM_3        `~
    case 0xBD: return 0x000C; // VK_OEM_MINUS    -_
    case 0xBB: return 0x000D; // VK_OEM_PLUS     =+
    case 0x08: return 0x000E; // VK_BACK
    case 0x09: return 0x000F; // VK_TAB
    case 0xDB: return 0x001A; // VK_OEM_4        [{
    case 0xDD: return 0x001B; // VK_OEM_6        ]}
    case 0xDC: return 0x002B; // VK_OEM_5        \|
    case 0x14: return 0x003A; // VK_CAPITAL
    case 0xBA: return 0x0027; // VK_OEM_1        ;:
    case 0xDE: return 0x0028; // VK_OEM_7        '"
    case 0x0D: return 0x001C; // VK_RETURN
    case 0xE2: return 0x0056; // VK_OEM_102      the ISO <> key next to left Shift
    case 0xBC: return 0x0033; // VK_OEM_COMMA    ,<
    case 0xBE: return 0x0034; // VK_OEM_PERIOD   .>
    case 0xBF: return 0x0035; // VK_OEM_2        /?
    case 0x20: return 0x0039; // VK_SPACE

    // Modifiers. The unsided VK codes take the left key, as a keyboard does.
    case 0x10:                // VK_SHIFT
    case 0xA0: return 0x002A; // VK_LSHIFT
    case 0xA1: return 0x0036; // VK_RSHIFT
    case 0x11:                // VK_CONTROL
    case 0xA2: return 0x001D; // VK_LCONTROL
    case 0xA3: return 0xE01D; // VK_RCONTROL
    case 0x12:                // VK_MENU
    case 0xA4: return 0x0038; // VK_LMENU
    case 0xA5: return 0xE038; // VK_RMENU        AltGr
    case 0x5B: return 0xE05B; // VK_LWIN
    case 0x5C: return 0xE05C; // VK_RWIN
    case 0x5D: return 0xE05D; // VK_APPS

    case 0x7A: return 0x0057; // VK_F11
    case 0x7B: return 0x0058; // VK_F12

    // Navigation cluster — all extended, which is what separates them from the
    // numpad keys they share a scancode with.
    case 0x2C: return 0xE037; // VK_SNAPSHOT
    case 0x91: return 0x0046; // VK_SCROLL
    case 0x2D: return 0xE052; // VK_INSERT
    case 0x2E: return 0xE053; // VK_DELETE
    case 0x24: return 0xE047; // VK_HOME
    case 0x23: return 0xE04F; // VK_END
    case 0x21: return 0xE049; // VK_PRIOR        Page Up
    case 0x22: return 0xE051; // VK_NEXT         Page Down
    case 0x26: return 0xE048; // VK_UP
    case 0x28: return 0xE050; // VK_DOWN
    case 0x25: return 0xE04B; // VK_LEFT
    case 0x27: return 0xE04D; // VK_RIGHT

    // Numpad operators. NumLock's make code is NOT extended — the extended flag
    // is added on the way out (see isExtendedKey) so it is not read as Pause.
    case 0x90: return 0x0045; // VK_NUMLOCK
    case 0x6F: return 0xE035; // VK_DIVIDE
    case 0x6A: return 0x0037; // VK_MULTIPLY
    case 0x6D: return 0x004A; // VK_SUBTRACT
    case 0x6B: return 0x004E; // VK_ADD
    case 0x6E: return 0x0053; // VK_DECIMAL

    // Pause is deliberately absent: its make code is the two-scancode sequence
    // 0xE1 0x1D 0x45, which a single INPUT cannot express. Falling through to
    // 0 sends it as a virtual key instead of half a sequence.
    default: return 0;
    }
    // clang-format on
}

} // namespace mw::native::input
