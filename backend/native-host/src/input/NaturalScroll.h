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

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace mw::native::input {

/// The pure half of GnomeNaturalScroll (linux/GnomeNaturalScroll.h): which
/// desktop asks for it, and what gsettings answered. Pure strings, tested on
/// every platform.

/// Whether XDG_CURRENT_DESKTOP names GNOME. It is a colon-separated list
/// ("ubuntu:GNOME", "GNOME-Classic:GNOME"), and its entries are compared whole
/// and without case, so a desktop merely named like it ("GNOMEish") is not
/// taken for it. Null or empty: no.
inline bool desktopIsGnome(const char* xdgCurrentDesktop)
{
    if (!xdgCurrentDesktop) return false;
    const std::string_view all(xdgCurrentDesktop);
    size_t start = 0;
    while (start <= all.size()) {
        size_t end = all.find(':', start);
        if (end == std::string_view::npos) end = all.size();
        const std::string_view entry = all.substr(start, end - start);
        if (entry.size() == 5) {
            bool same = true;
            for (size_t i = 0; i < 5 && same; ++i) {
                char c = entry[i];
                if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
                same = c == "GNOME"[i];
            }
            if (same) return true;
        }
        start = end + 1;
    }
    return false;
}

/// Whether this process may run gsettings at all: never as root and never
/// set-uid (real and effective users differ). Under root the exec gives the
/// child every capability back, and gsettings loads GIO and dconf modules the
/// environment chooses; the wheel is not worth that.
inline bool mayAskGsettings(unsigned long uid, unsigned long euid)
{
    return uid != 0 && euid != 0 && uid == euid;
}

/// The environment gsettings runs in: ours, with the user's own home put back.
/// The stream worker starts from a fixed environment where HOME and
/// XDG_CONFIG_HOME are /nonexistent (packaging/linux/moonlightweb-launch-env.h),
/// and dconf, finding no user database there, answers the schema's default —
/// "false" — whatever the user chose. The launcher keeps the real home in
/// MW_USER_HOME: HOME takes it, and XDG_CONFIG_HOME is left out so that dconf
/// reads $HOME/.config/dconf/user. Without MW_USER_HOME, or with one that is
/// not an absolute path free of "..", the environment is passed as it is.
inline std::vector<std::string> gsettingsEnvironment(const std::vector<std::string_view>& env)
{
    constexpr std::string_view kUserHome = "MW_USER_HOME=";
    std::string_view home;
    for (const std::string_view entry : env)
        if (entry.substr(0, kUserHome.size()) == kUserHome) {
            home = entry.substr(kUserHome.size());
            break;
        }
    const bool usable =
        !home.empty() && home.front() == '/' && home.find("..") == std::string_view::npos;

    std::vector<std::string> out;
    out.reserve(env.size() + 1);
    for (const std::string_view entry : env) {
        if (usable && (entry.substr(0, 5) == "HOME=" || entry.substr(0, 16) == "XDG_CONFIG_HOME="))
            continue;
        out.emplace_back(entry);
    }
    if (usable) out.push_back("HOME=" + std::string(home));
    return out;
}

/// What `gsettings get … natural-scroll` printed: "true" or "false", with the
/// trailing newline or spaces it may carry. Anything else (an error message, a
/// missing schema, nothing at all) is no answer, never a guess.
inline std::optional<bool> parseGsettingsBool(std::string_view out)
{
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.remove_suffix(1);
    while (!out.empty() && out.front() == ' ')
        out.remove_prefix(1);
    if (out == "true") return true;
    if (out == "false") return false;
    return std::nullopt;
}

} // namespace mw::native::input
