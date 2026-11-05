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

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

/// The host's own keyboard, made through the "MoonlightWeb Virtual HID" driver
/// on a Windows host with no keyboard, for the keys of the client's gaming mode
/// on the normal desktop: what a game reading Raw Input by device needs (S1 for
/// the keyboard, Bruno, 04/11/2026, within the limits a security review set;
/// gaming mode and the normal desktop only, his choice of 05/11/2026).
///
/// ── What a report is not ────────────────────────────────────────────────────
///
/// A report is hardware input, where SendInput is not: it would reach the
/// secure desktop and its UAC prompt, the key combinations Windows keeps for
/// itself listen to it, and Windows repeats a key it holds. So the bytes are
/// fixed and narrow (no power, media or system-control key can be said in
/// them), the combinations that lock or sign out are taken out of every report,
/// and no key goes through it unless the input desktop is the normal one.
///
/// As for the mouse (SystemMouse.h), the passthrough refuses every keyboard
/// (hid::validate), a page can never name one, and this descriptor is the
/// driver's one further exception, compared byte for byte.
///
/// Pure: the bytes, their reports, the rules, tested anywhere.
namespace mw::native::input::systemkeyboard {

/// One input report, no report id: the eight modifiers (one bit each), a byte
/// of padding, and up to six keys held, as HID usages in any order, 0 for none.
constexpr size_t kReportBytes = 8;
using Report = std::array<uint8_t, kReportBytes>;

constexpr size_t kMaxKeys = 6;
/// The last usage the descriptor can say, the Application key: 0x66 is Power,
/// and after it come F13-F24, Mute and the volume, which a keyboard of ours
/// has no business pressing. Beyond it a key stays on SendInput.
constexpr uint8_t kMaxUsage = 0x65;

constexpr uint8_t kLeftCtrl = 0x01;
constexpr uint8_t kLeftShift = 0x02;
constexpr uint8_t kLeftAlt = 0x04;
constexpr uint8_t kLeftWin = 0x08;
constexpr uint8_t kRightCtrl = 0x10;
constexpr uint8_t kRightShift = 0x20;
constexpr uint8_t kRightAlt = 0x40;
constexpr uint8_t kRightWin = 0x80;

/// The usages the rules name.
constexpr uint8_t kUsageL = 0x0F;
constexpr uint8_t kUsageScrollLock = 0x47;
constexpr uint8_t kUsageDelete = 0x4C;
constexpr uint8_t kUsageKeypadDecimal = 0x63;

/// The descriptor, the same bytes every time.
const std::vector<uint8_t>& descriptor();

/// Whether @p data is exactly descriptor(): the driver's exception to
/// hid::validate(), and nothing else gets through it.
bool isDescriptor(const uint8_t* data, size_t size);

/// One key of the report: a modifier's bit, or a usage. Both 0: this keyboard
/// cannot say it.
struct Key
{
    uint8_t modifier = 0;
    uint8_t usage = 0;
    bool valid() const { return modifier != 0 || usage != 0; }
};

/// The key at a set-1 scancode, 0xE0 in the high byte for an extended one, as
/// UsScanCode.h gives them. Nothing for a scancode with no usage in reach:
/// Pause, whose sequence is no scancode, the media keys, a Japanese keyboard's
/// extra keys.
Key keyOfScan(uint16_t scan);

/// Whether @p usage is taken out of a report that holds @p modifiers: Delete,
/// and the keypad's, under Ctrl and Alt; L under either Windows key; Scroll
/// Lock under the right Ctrl, which is how a keyboard asks for a crash dump.
/// On the whole state, so the order they went down in changes nothing. This
/// is behaviour and not a boundary: the list can never be complete.
bool withheld(uint8_t modifiers, uint8_t usage);

/// The report for @p modifiers and @p usages held on this keyboard.
/// @p otherModifiers are held elsewhere (by SendInput): they go in no report,
/// but Windows adds them up, so the rules count them. What is out of reach or
/// beyond six keys is not said. Ctrl+Alt+Del itself stays the secure attention
/// event (InputEvent.h), which the gate decides on.
Report reportOf(uint8_t modifiers, const std::vector<uint8_t>& usages, uint8_t otherModifiers = 0);

/// What decides whether a key goes through this keyboard.
struct Standing
{
    int mode = -1;              ///< EncoderTuning::virtualKeyboard
    bool allowElevated = false; ///< the viewer may reach administrator windows
    bool gamingMode = false;    ///< the key came from the client's gaming mode
    bool onConsole = false;     ///< the console session is this worker's
    bool onDefault = false;     ///< the input desktop is "Default", not a secure one
};

/// Whether keys may go through it now. SessionConfig::allowElevatedInput
/// defaults to true: the viewer held back is the case that must be said.
bool usable(const Standing& standing);

/// Whether to make it now. @p realKeyboards is how many Windows lists, not
/// counting its own and ours; below 0, it could not tell. A doubt is a
/// keyboard. The bench's vkeyboard=1 lifts this test and no other.
bool mayMake(const Standing& standing, int realKeyboards);

} // namespace mw::native::input::systemkeyboard
