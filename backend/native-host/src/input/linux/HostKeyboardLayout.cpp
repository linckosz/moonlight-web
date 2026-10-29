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

#include "mw/native/HostKeyboardLayout.h"

#include "EvdevKeyMap.h"
#include "XkbTextMap.h"

#include <linux/input-event-codes.h>

namespace mw::native {
namespace {

/// The layout XkbTextMap compiled, its strokes turned into GameStream keys.
class XkbHostLayout final : public HostKeyboardLayout
{
public:
    bool open()
    {
        if (!m_Map.open() || !m_Map.fromSystem()) return false;
        m_Name = m_Map.names().layout;
        if (!m_Map.names().variant.empty()) m_Name += " " + m_Map.names().variant;
        return true;
    }

    bool find(char32_t cp, LayoutChar& out) const override
    {
        input::XkbStroke strokes[2];
        int count = 0;
        if (!m_Map.find(cp, strokes, count)) return false;
        LayoutChar result;
        for (int i = 0; i < count; ++i) {
            LayoutKey& key = result.keys[i];
            key.vk = input::typingVkAt(strokes[i].code);
            // A character on the keypad, or past it: not a key a host that
            // presses US positions can be told about.
            if (key.vk == 0) return false;
            for (const uint16_t mod : strokes[i].mods) {
                if (mod == KEY_LEFTSHIFT)
                    key.shift = true;
                else if (mod == KEY_RIGHTALT)
                    key.altGr = true;
                else if (mod != 0)
                    return false;
            }
        }
        result.count = count;
        out = result;
        return true;
    }

    std::string name() const override { return m_Name; }

private:
    input::XkbTextMap m_Map;
    std::string m_Name;
};

} // namespace

std::shared_ptr<const HostKeyboardLayout> HostKeyboardLayout::detect()
{
    auto layout = std::make_shared<XkbHostLayout>();
    if (!layout->open()) return nullptr;
    return layout;
}

} // namespace mw::native
