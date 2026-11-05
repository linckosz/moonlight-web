/*
 * MoonlightWeb — "MoonlightWeb Virtual HID" driver.
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

// The driver asks the engine's own validate() before it creates a device: the
// host checks the descriptor already, and this keeps a process that reaches
// the control device some other way from making a keyboard, a mouse or a
// system-control device out of it. Defence in depth, same rules, same code.
//
// Two exceptions, and only two (S1, Bruno, 19/10/2026 and 04/11/2026): the
// host's own mouse and its own keyboard, whose descriptors are fixed in the
// engine and compared here byte for byte (input/SystemMouse.h,
// input/SystemKeyboard.h). A host with none gets them from the stream's worker;
// nothing a page sends can be them, since a page's collections go through
// validate(), which refuses them.

#include "input/HidDescriptor.h"
#include "input/SystemKeyboard.h"
#include "input/SystemMouse.h"

#include <cstdint>
#include <vector>

extern "C" int MwHidDescriptorAllowed(const unsigned char* data, unsigned long size)
{
    if (!data || size == 0 || size > mw::native::input::hid::kMaxDescriptorBytes) return 0;
    if (mw::native::input::systemmouse::isDescriptor(data, size)) return 1;
    if (mw::native::input::systemkeyboard::isDescriptor(data, size)) return 1;
    try {
        const std::vector<uint8_t> descriptor(data, data + size);
        return mw::native::input::hid::validate(descriptor).empty() ? 1 : 0;
    } catch (...) {
        return 0; // out of memory, say: refuse rather than guess
    }
}
