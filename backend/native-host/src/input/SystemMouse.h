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

/// The host's own mouse, made through the "MoonlightWeb Virtual HID" driver
/// on a Windows host with no pointing device (plan « souris HID virtuelle »,
/// option B, porte S1 chosen by Bruno on 19/10/2026).
///
/// ── Why a device ────────────────────────────────────────────────────────────
///
/// A Windows host with no mouse draws no pointer, and SendInput is not a
/// device: the pointer moves, nobody sees it, and a game that reads Raw Input
/// by device (hDevice) sees nothing at all. Mouse Keys (PointerPresence, the
/// fallback) make Windows show the pointer; a mouse makes it show the pointer
/// and gives games a device, whose relative moves, buttons and wheel arrive
/// as hardware input.
///
/// ── Why this descriptor and no other (S1) ───────────────────────────────────
///
/// The HID passthrough refuses every keyboard, mouse and system-control
/// collection, in the engine and again in the driver (hid::validate): a page
/// must never be able to make the host a pointing device out of network data.
/// That stays. The one exception is this descriptor, fixed here and compared
/// byte for byte by the driver: nothing a page sends can be it, since a page's
/// collections go through validate(), which refuses it, and the engine never
/// hands a page's bytes to the driver unchecked.
///
/// Pure: the bytes, their reports and the comparison, tested anywhere.
namespace mw::native::input::systemmouse {

/// One input report, no report id: buttons 1-5 (one bit each, three bits of
/// padding), X and Y relative on 16 bits, little-endian, the wheel and the
/// horizontal wheel (Consumer AC Pan) on 8 bits, all signed.
constexpr size_t kReportBytes = 7;
using Report = std::array<uint8_t, kReportBytes>;

/// The largest step a report carries on each axis; a longer move is split.
constexpr int kMaxDelta = 32767;
/// The largest number of wheel notches a report carries.
constexpr int kMaxNotches = 127;
/// What one wheel notch is worth in Windows' units (WHEEL_DELTA).
constexpr int kNotch = 120;

/// The descriptor, the same bytes every time.
const std::vector<uint8_t>& descriptor();

/// Whether @p data is exactly descriptor(): the driver's exception to
/// hid::validate(), and nothing else gets through it.
bool isDescriptor(const uint8_t* data, size_t size);

/// The report bit of a browser button (1 left, 2 middle, 3 right, 4 back,
/// 5 forward — the numbering InputEvent carries), or 0 for any other: HID's
/// buttons are primary, secondary, tertiary, then the two side buttons.
uint8_t buttonBit(int browserButton);

/// The reports one movement takes: @p buttons held (buttonBit()s), a move of
/// @p dx, @p dy and @p notches and @p panNotches of the wheels. At least one
/// report, even with nothing to move — a button that changed. A step beyond
/// a report's reach is split over as many as it takes, the buttons in each.
std::vector<Report> reports(uint8_t buttons, int dx, int dy, int notches = 0, int panNotches = 0);

/// Whole wheel notches out of Windows' wheel units: a Windows host gets its
/// scrolling unquantized (HostOsProbe.h), in units of WHEEL_DELTA (120 a
/// notch), a trackpad's often less than one; a report carries whole notches
/// only. What is left over waits for the next turn the same way, and is
/// dropped when the wheel turns back.
class Notches
{
public:
    /// The notches @p amount completes, signed, with what came before.
    int take(int amount)
    {
        if (amount == 0) return 0;
        if ((amount > 0) != (m_Rest > 0) && m_Rest != 0) m_Rest = 0;
        m_Rest += amount;
        const int whole = m_Rest / kNotch;
        m_Rest -= whole * kNotch;
        return whole;
    }
    void reset() { m_Rest = 0; }

private:
    int m_Rest = 0;
};

/// Who a report is for: a device's input goes to the session that has the
/// console, whichever session made the device. So the mouse speaks while the
/// console is the stream's own session, and is quiet otherwise; the stream's
/// input then stays where it was sent from, by SendInput. No thread watches for
/// the change, as one does for the keyboard: a mouse repeats nothing by itself,
/// so a device left silent does nothing, and the next movement finds out.
///
/// What the device last said was down is said up once on the way out, whatever
/// the console, as for the keyboard: it must not stay down until the console
/// comes back. A release alone clicks nothing.
class ConsoleGuard
{
public:
    struct Step
    {
        /// What to send now. Nothing else may go to the device.
        std::vector<Report> reports;
        /// The movement was not sent: the caller does it the other way.
        bool aside = false;
        /// The buttons it no longer holds: released here, not by the viewer.
        uint8_t forgotten = 0;
        /// The first step since the console changed hands, to be said once.
        bool changed = false;
    };

    /// One movement, as for reports(), with @p onConsole read just before it.
    Step step(bool onConsole, uint8_t buttons, int dx, int dy, int notches = 0, int panNotches = 0);

private:
    uint8_t m_Said = 0;
    bool m_Aside = false;
};

} // namespace mw::native::input::systemmouse
