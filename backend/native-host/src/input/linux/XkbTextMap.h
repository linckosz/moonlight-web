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
#include <string>
#include <unordered_map>
#include <vector>

namespace mw::native::input {

/// UTF-8 bytes to code points, appended to @p out.
///
/// Deliberately forgiving and deliberately silent: the payload comes from a
/// browser and is already valid UTF-8 in every case that matters, so the error
/// paths exist to make sure a malformed byte costs one dropped character rather
/// than a run off the end of the string. A stray continuation byte or a bad
/// sequence is skipped, a sequence truncated by the end of the buffer stops the
/// walk, and lone surrogates — which UTF-8 must not carry — are dropped.
///
/// In the header, and free of anything Linux, so the tests reach it on every
/// platform: this is the half of text injection that can be wrong in a boring
/// way, and it has no business being testable only on the host it ships to.
inline void decodeUtf8(const std::string& in, std::vector<char32_t>& out)
{
    const auto* p = reinterpret_cast<const unsigned char*>(in.data());
    const size_t n = in.size();
    for (size_t i = 0; i < n;) {
        const unsigned char c = p[i];
        uint32_t cp = 0;
        size_t extra = 0;
        if (c < 0x80) {
            cp = c;
        } else if ((c & 0xE0) == 0xC0) {
            cp = c & 0x1Fu;
            extra = 1;
        } else if ((c & 0xF0) == 0xE0) {
            cp = c & 0x0Fu;
            extra = 2;
        } else if ((c & 0xF8) == 0xF0) {
            cp = c & 0x07u;
            extra = 3;
        } else {
            ++i; // a continuation byte with no lead, or 0xFE/0xFF
            continue;
        }

        if (extra >= n - i) return; // truncated by the end of the buffer
        bool ok = true;
        for (size_t k = 1; k <= extra; ++k) {
            const unsigned char cc = p[i + k];
            if ((cc & 0xC0) != 0x80) {
                ok = false;
                break;
            }
            cp = (cp << 6) | (cc & 0x3Fu);
        }
        if (!ok) {
            ++i;
            continue;
        }
        i += extra + 1;
        if (cp >= 0xD800 && cp <= 0xDFFF) continue;
        if (cp > 0x10FFFF) continue;
        out.push_back(static_cast<char32_t>(cp));
    }
}

/// The five names XKB compiles a keymap from — rules, model, layout, variant,
/// options. Empty means "let libxkbcommon use its own default for this one".
struct XkbNames
{
    std::string rules;
    std::string model;
    std::string layout;
    std::string variant;
    std::string options;
};

/// Read the XKB* assignments out of the contents of /etc/default/keyboard.
///
/// The file is a shell fragment — `XKBLAYOUT="fr"`, comments on # lines — and is
/// where a Debian or Ubuntu host records the layout chosen at install time. It
/// is parsed rather than sourced, because sourcing it would mean running a shell
/// as a process that holds CAP_SYS_ADMIN.
///
/// @returns whether a layout was found; the other fields are optional and stay
///          empty when the file does not name them.
inline bool parseKeyboardConfig(const std::string& text, XkbNames& out)
{
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t eol = text.find('\n', pos);
        std::string line =
            text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos);
        pos = eol == std::string::npos ? text.size() + 1 : eol + 1;

        const size_t begin = line.find_first_not_of(" \t\r");
        if (begin == std::string::npos || line[begin] == '#') continue;
        const size_t eq = line.find('=', begin);
        if (eq == std::string::npos) continue;

        std::string key = line.substr(begin, eq - begin);
        while (!key.empty() && (key.back() == ' ' || key.back() == '\t'))
            key.pop_back();

        std::string value = line.substr(eq + 1);
        const size_t vBegin = value.find_first_not_of(" \t");
        const size_t vEnd = value.find_last_not_of(" \t\r");
        value =
            vBegin == std::string::npos ? std::string() : value.substr(vBegin, vEnd - vBegin + 1);
        if (value.size() >= 2 && (value.front() == '"' || value.front() == '\'') &&
            value.back() == value.front())
            value = value.substr(1, value.size() - 2);

        if (key == "XKBMODEL")
            out.model = value;
        else if (key == "XKBLAYOUT")
            out.layout = value;
        else if (key == "XKBVARIANT")
            out.variant = value;
        else if (key == "XKBOPTIONS")
            out.options = value;
    }
    return !out.layout.empty();
}

/// What has to be pressed to type one character on the host's own layout.
struct XkbStroke
{
    uint16_t code = 0;         ///< evdev key code (KEY_*)
    uint16_t mods[2] = {0, 0}; ///< evdev modifier keys to hold; 0 = unused
};

/// Character → keystroke, for the layout the host is actually using.
///
/// ── Why this is needed at all ───────────────────────────────────────────────
///
/// Windows and macOS can inject a CHARACTER: KEYEVENTF_UNICODE and
/// CGEventKeyboardSetUnicodeString both take the glyph itself, and no key on the
/// host has to be able to produce it. Linux has no such thing. uinput reports
/// key POSITIONS, and the compositor turns a position into a character using the
/// user's layout — so to make an 'a' appear we have to know which key produces
/// an 'a' HERE. On the AZERTY host this was written against, that is the key a
/// US keyboard calls Q.
///
/// That is why the soft keyboard of a phone typed nothing on a Linux host: it
/// sends text, not keys (a touch keyboard has no key positions to send), and
/// text was the one input type UinputInput dropped on the floor.
///
/// ── Where the layout comes from ─────────────────────────────────────────────
///
/// libxkbcommon compiles a keymap from rules/model/layout/variant/options, and
/// we take those from XKB_DEFAULT_* if the session sets them, then from
/// /etc/default/keyboard, and otherwise leave them to libxkbcommon's own default
/// (a US layout). The resolved names are logged, because they are a guess about
/// somebody else's desktop: if a host ever types the wrong letter, the log line
/// says in one glance which layout was assumed. GNOME keeps its own copy of the
/// setting in dconf, which a user who changed layout AFTER installing can have
/// drift from the file; reading it would mean running gsettings as a child of a
/// process holding CAP_SYS_ADMIN, which is a worse trade than being wrong on a
/// host that can set XKB_DEFAULT_LAYOUT.
///
/// ── Loaded, not linked ──────────────────────────────────────────────────────
///
/// libxkbcommon is opened with dlopen, for the reason X11Pointer gives: the
/// engine builds on a Linux with no -dev package at all, and a host without the
/// library keeps exactly the behaviour it has today — text is ignored, keys
/// still work.
class XkbTextMap
{
public:
    XkbTextMap() = default;
    ~XkbTextMap();

    XkbTextMap(const XkbTextMap&) = delete;
    XkbTextMap& operator=(const XkbTextMap&) = delete;

    /// Load the library, compile the keymap and walk it into the table. False
    /// when there is no libxkbcommon, or when the keymap yields nothing typable
    /// — in both cases text injection stays off and says so once.
    bool open();
    void close();
    bool isOpen() const { return !m_Table.empty(); }

    /// The keystrokes for @p cp, in order, into @p out — one for a character the
    /// layout has a key for, two when it is reached through a dead key (on an
    /// AZERTY host `ê` is the dead circumflex then `e`, which is how a person
    /// types it). False when the layout cannot produce the character at all.
    bool find(char32_t cp, XkbStroke out[2], int& count) const;

    /// The unmodified character this layout puts on @p code, or 0 when the key
    /// types nothing (a modifier, an arrow) or is not in the table.
    ///
    /// find() read backwards, and deliberately by scan rather than by a second
    /// index: it exists only for the keyboard diagnostics, which run on a key
    /// down in a debug mode, over a table of a few hundred entries. A permanent
    /// reverse index would be a second thing to keep in step for a line nobody
    /// reads in normal use.
    char32_t characterAt(uint16_t code) const;

    /// "fr+azerty (pc105)" — for the log line, and for a human deciding whether
    /// the host guessed right.
    const std::string& description() const { return m_Description; }
    size_t size() const { return m_Table.size(); }

private:
    void* m_Lib = nullptr;
    std::string m_Description;
    std::unordered_map<char32_t, XkbStroke> m_Table;
    /// The dead keys this layout has, by keysym. A dead key produces no
    /// character of its own — which is exactly why the walk above skips it — and
    /// is the only way to reach an accented letter the keyboard has no key for.
    std::unordered_map<uint32_t, XkbStroke> m_Dead;
};

} // namespace mw::native::input
