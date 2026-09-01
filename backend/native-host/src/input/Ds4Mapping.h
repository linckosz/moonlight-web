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

/// The protocol's controller state → a DualShock 4 HID input report.
///
/// ── Why this is its own file ────────────────────────────────────────────────
///
/// Nothing here touches Windows, ViGEm or any device. That is the point: this
/// is the only part of the DualShock 4 profile that can be *wrong* in an
/// interesting way — an inverted axis, a D-pad diagonal, a stick that does not
/// centre — and all of it is arithmetic. Kept pure, it is verified on every
/// platform by mw-native-tests, with no driver and no pad plugged in, which is
/// what §16 of the design asks for.
///
/// ── Why not the vendor's XUSB_TO_DS4_REPORT ─────────────────────────────────
///
/// ViGEmUtil.h ships one. Its stick conversion negates a value that is about to
/// be range-shifted, is asymmetric between the two Y axes (one uses +1, the
/// other −1) and then patches the result up with "if it came out 0, make it
/// 0xFF". Ours is the same conversion written so that centre stays centre and
/// both extremes land exactly on 0 and 255 — see toStickByte().
namespace mw::native::input {

/// The protocol's button bits.
///
/// These are XInput's own values: GameStream modelled its flags on XInput, so
/// A is 0x1000 in both, and the browser's Gamepad API maps onto the same
/// layout. VigemGamepad.cpp static_asserts them against the XUSB_GAMEPAD_*
/// constants, so a drift between the two is a compile error rather than a
/// silently mis-mapped button.
namespace pad {
constexpr uint32_t kDpadUp = 0x0001;
constexpr uint32_t kDpadDown = 0x0002;
constexpr uint32_t kDpadLeft = 0x0004;
constexpr uint32_t kDpadRight = 0x0008;
constexpr uint32_t kStart = 0x0010;
constexpr uint32_t kBack = 0x0020;
constexpr uint32_t kLeftThumb = 0x0040;
constexpr uint32_t kRightThumb = 0x0080;
constexpr uint32_t kLeftShoulder = 0x0100;
constexpr uint32_t kRightShoulder = 0x0200;
constexpr uint32_t kGuide = 0x0400;
constexpr uint32_t kA = 0x1000;
constexpr uint32_t kB = 0x2000;
constexpr uint32_t kX = 0x4000;
constexpr uint32_t kY = 0x8000;
} // namespace pad

/// DualShock 4 report bits, as the driver's own header defines them. Repeated
/// here so this file needs no Windows header; VigemGamepad.cpp static_asserts
/// them against DS4_BUTTON_* for the same reason as above.
namespace ds4 {
constexpr uint16_t kSquare = 1u << 4;
constexpr uint16_t kCross = 1u << 5;
constexpr uint16_t kCircle = 1u << 6;
constexpr uint16_t kTriangle = 1u << 7;
constexpr uint16_t kShoulderLeft = 1u << 8;
constexpr uint16_t kShoulderRight = 1u << 9;
constexpr uint16_t kTriggerLeft = 1u << 10;
constexpr uint16_t kTriggerRight = 1u << 11;
constexpr uint16_t kShare = 1u << 12;
constexpr uint16_t kOptions = 1u << 13;
constexpr uint16_t kThumbLeft = 1u << 14;
constexpr uint16_t kThumbRight = 1u << 15;

constexpr uint8_t kSpecialPs = 1u << 0;
constexpr uint8_t kSpecialTouchpad = 1u << 1;

/// The D-pad is a hat, not four bits: one of eight directions, or "centred".
/// It lives in the low nibble of the button word.
constexpr uint16_t kDpadMask = 0x000F;
constexpr uint16_t kDpadNorth = 0x0;
constexpr uint16_t kDpadNorthEast = 0x1;
constexpr uint16_t kDpadEast = 0x2;
constexpr uint16_t kDpadSouthEast = 0x3;
constexpr uint16_t kDpadSouth = 0x4;
constexpr uint16_t kDpadSouthWest = 0x5;
constexpr uint16_t kDpadWest = 0x6;
constexpr uint16_t kDpadNorthWest = 0x7;
constexpr uint16_t kDpadNone = 0x8;
} // namespace ds4

/// One DualShock 4 input report, in plain types.
///
/// Field for field what the driver's DS4_REPORT holds; VigemGamepad copies it
/// across at the edge. Defaults are the resting pad: sticks centred, D-pad
/// released.
struct Ds4Report
{
    uint8_t thumbLX = 0x80;
    uint8_t thumbLY = 0x80;
    uint8_t thumbRX = 0x80;
    uint8_t thumbRY = 0x80;
    uint16_t buttons = ds4::kDpadNone;
    uint8_t special = 0;
    uint8_t triggerL = 0;
    uint8_t triggerR = 0;
};

/// A signed 16-bit axis as the DualShock 4 wants it: 0..255, centre 0x80.
///
/// The obvious `(v + 32768) >> 8` is exact at both ends and at the centre, so
/// that is what this is. Inversion is applied BEFORE the shift and saturates
/// −32768 to 32767, because negating −32768 in 16 bits does not fit — the
/// classic way this conversion ends up one step off at the very bottom of the
/// axis, and the reason the vendor's version patches its own output afterwards.
///
/// @param invert  true for the Y axes: the protocol has up positive (XInput's
///                convention), the DualShock 4 has up at 0.
constexpr uint8_t toStickByte(int16_t value, bool invert)
{
    int32_t v = value;
    if (invert) v = (v == -32768) ? 32767 : -v;
    return static_cast<uint8_t>((v + 32768) >> 8);
}

/// XInput's own "is this trigger pressed" threshold
/// (XINPUT_GAMEPAD_TRIGGER_THRESHOLD), used for the DualShock 4's DIGITAL
/// L2/R2 bits — the analog value is reported in full either way.
///
/// A real pad sets that bit past a hardware threshold, so something has to be
/// chosen here; taking the platform's own number beats inventing one, and beats
/// the vendor helper's "any value above zero", which reports a resting trigger
/// as held on a pad with the slightest offset.
constexpr uint8_t kTriggerThreshold = 30;

/// Build the report. Pure: same inputs, same output, no state anywhere.
Ds4Report toDs4(uint32_t buttonFlags, uint8_t leftTrigger, uint8_t rightTrigger, int16_t leftStickX,
                int16_t leftStickY, int16_t rightStickX, int16_t rightStickY);

} // namespace mw::native::input
