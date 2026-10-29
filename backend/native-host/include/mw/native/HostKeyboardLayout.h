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
#include <memory>
#include <string>

namespace mw::native {

/// One key to press, named the way GameStream names keys: the virtual key a US
/// keyboard carries at that position, which is the position a Linux host
/// presses (Wolf, Sunshine). Shift and AltGr are held around it.
struct LayoutKey
{
    uint8_t vk = 0;
    bool shift = false;
    bool altGr = false;
};

/// The keys that type one character, in order: one, or two when the layout
/// builds it from a dead key (`ê` on an AZERTY is the dead circumflex, then e).
struct LayoutChar
{
    LayoutKey keys[2];
    int count = 0;
};

/// The keyboard layout of the machine MoonlightWeb runs on, character by
/// character.
///
/// For a Sunshine or a Wolf on this same machine, which can only type a
/// phone's text by pressing keys, and press them where this machine's layout
/// puts them (issue #44). The server never guesses it: a layout counts only
/// when the system names one — XKB_DEFAULT_LAYOUT, /etc/default/keyboard or
/// the X11 keyboard file localectl writes. Wolf types in the layout of its own
/// container instead, US unless XKB_DEFAULT_LAYOUT is set there: the admin is
/// told so beside the box that turns this on.
class HostKeyboardLayout
{
public:
    virtual ~HostKeyboardLayout() = default;

    /// The keys for @p cp on this layout. False when the layout cannot type it.
    virtual bool find(char32_t cp, LayoutChar& out) const = 0;

    /// The layout as the system names it, "fr" or "fr azerty".
    virtual std::string name() const = 0;

    /// This machine's layout, or nullptr when the system names none, when no
    /// libxkbcommon is there to read it, and on every OS but Linux.
    static std::shared_ptr<const HostKeyboardLayout> detect();
};

} // namespace mw::native
