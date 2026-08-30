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

#include <functional>
#include <string>

// Internal logging. The module never picks a destination for itself — a library
// that writes to stderr on its own is a library that cannot be embedded — so
// everything goes through the sink the host installs via
// NativeHost::setLogSink(). With no sink, logging is a no-op and costs nothing
// beyond the level check.

namespace mw::native::log {

enum Level
{
    Debug = 0,
    Info = 1,
    Warning = 2,
    Error = 3,
};

void setSink(std::function<void(int level, const std::string& message)> sink);

/// True when a sink is installed and would accept this level. Check it before
/// building an expensive message — a per-frame log line must not pay for its
/// own string concatenation when nobody is listening.
bool enabled(Level level);

void write(Level level, const std::string& message);

inline void debug(const std::string& m)
{
    write(Debug, m);
}
inline void info(const std::string& m)
{
    write(Info, m);
}
inline void warning(const std::string& m)
{
    write(Warning, m);
}
inline void error(const std::string& m)
{
    write(Error, m);
}

} // namespace mw::native::log
