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

#include "SystemKeyboard.h"

#include <cstring>
#include <iterator>

namespace mw::native::input::systemkeyboard {

namespace {

// Changing a single byte here changes what the driver lets through: a new
// driver, signed, with new pins in the installer, and the old driver refusing
// the new bytes (the engine then stays on SendInput). A constant array: the
// driver compares against it, and a driver DLL may not run constructors.
//
// No output report: no LEDs. Lock keys are the host's own state either way
// (LockKeySync), and a keyboard with none is a keyboard Windows takes.
constexpr uint8_t kDescriptor[] = {
    0x05, 0x01, // Usage Page (Generic Desktop)
    0x09, 0x06, // Usage (Keyboard)
    0xA1, 0x01, // Collection (Application)
    0x05, 0x07, //   Usage Page (Keyboard)
    0x19, 0xE0, //   Usage Minimum (Left Control)
    0x29, 0xE7, //   Usage Maximum (Right GUI)
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x01, //   Logical Maximum (1)
    0x75, 0x01, //   Report Size (1)
    0x95, 0x08, //   Report Count (8)
    0x81, 0x02, //   Input (Data, Variable, Absolute): the modifiers
    0x75, 0x08, //   Report Size (8)
    0x95, 0x01, //   Report Count (1)
    0x81, 0x01, //   Input (Constant): padding
    0x15, 0x00, //   Logical Minimum (0)
    0x25, 0x65, //   Logical Maximum (0x65, kMaxUsage)
    0x19, 0x00, //   Usage Minimum (0)
    0x29, 0x65, //   Usage Maximum (0x65, kMaxUsage)
    0x75, 0x08, //   Report Size (8)
    0x95, 0x06, //   Report Count (6)
    0x81, 0x00, //   Input (Data, Array): the keys
    0xC0,       // End Collection
};

/// The usages of set-1 scancodes 0x01 to 0x58, 0 for none. The modifiers are
/// apart, in keyOfScan.
constexpr uint8_t kPlainUsages[0x59] = {
    // clang-format off
    /* 0x00 */ 0x00, 0x29, 0x1E, 0x1F, 0x20, 0x21, 0x22, 0x23, // Esc, 1..6
    /* 0x08 */ 0x24, 0x25, 0x26, 0x27, 0x2D, 0x2E, 0x2A, 0x2B, // 7, 8, 9, 0, -, =, Backspace, Tab
    /* 0x10 */ 0x14, 0x1A, 0x08, 0x15, 0x17, 0x1C, 0x18, 0x0C, // Q W E R T Y U I
    /* 0x18 */ 0x12, 0x13, 0x2F, 0x30, 0x28, 0x00, 0x04, 0x16, // O P [ ] Enter (LCtrl) A S
    /* 0x20 */ 0x07, 0x09, 0x0A, 0x0B, 0x0D, 0x0E, 0x0F, 0x33, // D F G H J K L ;
    /* 0x28 */ 0x34, 0x35, 0x00, 0x31, 0x1D, 0x1B, 0x06, 0x19, // ' ` (LShift) \ Z X C V
    /* 0x30 */ 0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0x00, 0x55, // B N M , . / (RShift) Keypad *
    /* 0x38 */ 0x00, 0x2C, 0x39, 0x3A, 0x3B, 0x3C, 0x3D, 0x3E, // (LAlt) Space Caps F1..F5
    /* 0x40 */ 0x3F, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5F, // F6..F10 NumLock ScrollLock Keypad 7
    /* 0x48 */ 0x60, 0x61, 0x56, 0x5C, 0x5D, 0x5E, 0x57, 0x59, // Keypad 8 9 - 4 5 6 + 1
    /* 0x50 */ 0x5A, 0x5B, 0x62, 0x63, 0x00, 0x00, 0x64, 0x44, // Keypad 2 3 0 . (none) (none) ISO F11
    /* 0x58 */ 0x45,                                           // F12
    // clang-format on
};

constexpr uint8_t kAnyCtrl = kLeftCtrl | kRightCtrl;
constexpr uint8_t kAnyAlt = kLeftAlt | kRightAlt;
constexpr uint8_t kAnyWin = kLeftWin | kRightWin;

} // namespace

const std::vector<uint8_t>& descriptor()
{
    static const std::vector<uint8_t> bytes(std::begin(kDescriptor), std::end(kDescriptor));
    return bytes;
}

bool isDescriptor(const uint8_t* data, size_t size)
{
    return data && size == sizeof(kDescriptor) &&
           std::memcmp(data, kDescriptor, sizeof(kDescriptor)) == 0;
}

Key keyOfScan(uint16_t scan)
{
    Key key;
    const uint8_t prefix = static_cast<uint8_t>(scan >> 8);
    const uint8_t code = static_cast<uint8_t>(scan & 0xFF);
    if (prefix == 0) {
        switch (code) {
        case 0x1D: key.modifier = kLeftCtrl; return key;
        case 0x2A: key.modifier = kLeftShift; return key;
        case 0x36: key.modifier = kRightShift; return key;
        case 0x38: key.modifier = kLeftAlt; return key;
        default: break;
        }
        if (code < std::size(kPlainUsages)) key.usage = kPlainUsages[code];
        return key;
    }
    if (prefix != 0xE0) return key;
    // clang-format off
    switch (code) {
    case 0x1D: key.modifier = kRightCtrl; break;
    case 0x38: key.modifier = kRightAlt; break;
    case 0x5B: key.modifier = kLeftWin; break;
    case 0x5C: key.modifier = kRightWin; break;
    case 0x1C: key.usage = 0x58; break; // Keypad Enter
    case 0x35: key.usage = 0x54; break; // Keypad /
    case 0x37: key.usage = 0x46; break; // Print Screen
    case 0x47: key.usage = 0x4A; break; // Home
    case 0x48: key.usage = 0x52; break; // Up
    case 0x49: key.usage = 0x4B; break; // Page Up
    case 0x4B: key.usage = 0x50; break; // Left
    case 0x4D: key.usage = 0x4F; break; // Right
    case 0x4F: key.usage = 0x4D; break; // End
    case 0x50: key.usage = 0x51; break; // Down
    case 0x51: key.usage = 0x4E; break; // Page Down
    case 0x52: key.usage = 0x49; break; // Insert
    case 0x53: key.usage = kUsageDelete; break;
    case 0x5D: key.usage = 0x65; break; // Application
    default: break;
    }
    // clang-format on
    return key;
}

bool withheld(uint8_t modifiers, uint8_t usage)
{
    if ((usage == kUsageDelete || usage == kUsageKeypadDecimal) && (modifiers & kAnyCtrl) &&
        (modifiers & kAnyAlt))
        return true;
    if (usage == kUsageL && (modifiers & kAnyWin)) return true;
    if (usage == kUsageScrollLock && (modifiers & kRightCtrl)) return true;
    return false;
}

Report reportOf(uint8_t modifiers, const std::vector<uint8_t>& usages, uint8_t otherModifiers)
{
    Report report{};
    report[0] = modifiers;
    const uint8_t all = static_cast<uint8_t>(modifiers | otherModifiers);
    size_t at = 2;
    for (uint8_t usage : usages) {
        if (at >= kReportBytes) break;
        if (usage == 0 || usage > kMaxUsage || withheld(all, usage)) continue;
        // Two virtual keys may share a position (VK_SHIFT and VK_LSHIFT do for
        // a modifier): a usage is said once.
        bool said = false;
        for (size_t i = 2; i < at; ++i)
            said = said || report[i] == usage;
        if (!said) report[at++] = usage;
    }
    return report;
}

bool usable(const Standing& standing)
{
    return standing.mode != 0 && standing.allowElevated && standing.gamingMode &&
           standing.onConsole && standing.onDefault;
}

bool mayMake(const Standing& standing, int realKeyboards)
{
    return usable(standing) && (standing.mode > 0 || realKeyboards == 0);
}

} // namespace mw::native::input::systemkeyboard
