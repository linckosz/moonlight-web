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

#include "SystemMouse.h"

#include <algorithm>
#include <cstring>
#include <iterator>

namespace mw::native::input::systemmouse {

namespace {

// Changing a single byte here changes what the driver lets through: a new
// driver, signed, with new pins in the installer, and the old driver refusing
// the new bytes (the engine then falls back on Mouse Keys). A constant array:
// the driver compares against it, and a driver DLL may not run constructors.
constexpr uint8_t kDescriptor[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x02,       // Usage (Mouse)
    0xA1, 0x01,       // Collection (Application)
    0x09, 0x01,       //   Usage (Pointer)
    0xA1, 0x00,       //   Collection (Physical)
    0x05, 0x09,       //     Usage Page (Button)
    0x19, 0x01,       //     Usage Minimum (1)
    0x29, 0x05,       //     Usage Maximum (5)
    0x15, 0x00,       //     Logical Minimum (0)
    0x25, 0x01,       //     Logical Maximum (1)
    0x95, 0x05,       //     Report Count (5)
    0x75, 0x01,       //     Report Size (1)
    0x81, 0x02,       //     Input (Data, Variable, Absolute)
    0x95, 0x01,       //     Report Count (1)
    0x75, 0x03,       //     Report Size (3)
    0x81, 0x01,       //     Input (Constant): padding
    0x05, 0x01,       //     Usage Page (Generic Desktop)
    0x09, 0x30,       //     Usage (X)
    0x09, 0x31,       //     Usage (Y)
    0x16, 0x01, 0x80, //     Logical Minimum (-32767)
    0x26, 0xFF, 0x7F, //     Logical Maximum (32767)
    0x75, 0x10,       //     Report Size (16)
    0x95, 0x02,       //     Report Count (2)
    0x81, 0x06,       //     Input (Data, Variable, Relative)
    0x09, 0x38,       //     Usage (Wheel)
    0x15, 0x81,       //     Logical Minimum (-127)
    0x25, 0x7F,       //     Logical Maximum (127)
    0x75, 0x08,       //     Report Size (8)
    0x95, 0x01,       //     Report Count (1)
    0x81, 0x06,       //     Input (Data, Variable, Relative)
    0x05, 0x0C,       //     Usage Page (Consumer)
    0x0A, 0x38, 0x02, //     Usage (AC Pan): the horizontal wheel
    0x95, 0x01,       //     Report Count (1)
    0x81, 0x06,       //     Input (Data, Variable, Relative)
    0xC0,             //   End Collection
    0xC0,             // End Collection
};

void put16(uint8_t* at, int value)
{
    const auto v = static_cast<uint16_t>(static_cast<int16_t>(value));
    at[0] = static_cast<uint8_t>(v & 0xFF);
    at[1] = static_cast<uint8_t>(v >> 8);
}

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

uint8_t buttonBit(int browserButton)
{
    switch (browserButton) {
    case 1: return 0x01; // left: primary
    case 3: return 0x02; // right: secondary
    case 2: return 0x04; // middle: tertiary
    case 4: return 0x08; // back
    case 5: return 0x10; // forward
    default: return 0;
    }
}

std::vector<Report> reports(uint8_t buttons, int dx, int dy, int notches, int panNotches)
{
    std::vector<Report> out;
    // Widened: a delta near INT_MIN must not overflow on its way down.
    int64_t restX = dx;
    int64_t restY = dy;
    int64_t restWheel = notches;
    int64_t restPan = panNotches;
    do {
        const int x = static_cast<int>(std::clamp<int64_t>(restX, -kMaxDelta, kMaxDelta));
        const int y = static_cast<int>(std::clamp<int64_t>(restY, -kMaxDelta, kMaxDelta));
        const int w = static_cast<int>(std::clamp<int64_t>(restWheel, -kMaxNotches, kMaxNotches));
        const int p = static_cast<int>(std::clamp<int64_t>(restPan, -kMaxNotches, kMaxNotches));
        Report r{};
        r[0] = static_cast<uint8_t>(buttons & 0x1F);
        put16(&r[1], x);
        put16(&r[3], y);
        r[5] = static_cast<uint8_t>(static_cast<int8_t>(w));
        r[6] = static_cast<uint8_t>(static_cast<int8_t>(p));
        out.push_back(r);
        restX -= x;
        restY -= y;
        restWheel -= w;
        restPan -= p;
    } while (restX != 0 || restY != 0 || restWheel != 0 || restPan != 0);
    return out;
}

ConsoleGuard::Step ConsoleGuard::step(bool onConsole, uint8_t buttons, int dx, int dy, int notches,
                                      int panNotches)
{
    Step out;
    out.changed = onConsole == m_Aside;
    m_Aside = !onConsole;
    out.aside = !onConsole;
    if (onConsole) {
        out.reports = reports(buttons, dx, dy, notches, panNotches);
        m_Said = buttons;
        return out;
    }
    if (m_Said != 0) out.reports = reports(0, 0, 0);
    m_Said = 0;
    out.forgotten = buttons;
    return out;
}

} // namespace mw::native::input::systemmouse
