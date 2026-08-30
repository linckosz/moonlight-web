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

#include "Log.h"

#include <mutex>

namespace mw::native::log {
namespace {

// The sink is installed once at startup but read from every engine thread, so
// it is guarded. A shared_mutex would be the textbook answer; a plain mutex is
// the right one here because writes are rare, reads are already gated by
// enabled() on the hot paths, and a mutex has no ABI surprises across the
// compilers this module has to build under.
std::mutex g_Mutex;
std::function<void(int, const std::string&)> g_Sink;

} // namespace

void setSink(std::function<void(int level, const std::string& message)> sink)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Sink = std::move(sink);
}

bool enabled(Level)
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return static_cast<bool>(g_Sink);
}

void write(Level level, const std::string& message)
{
    std::function<void(int, const std::string&)> sink;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        sink = g_Sink;
    }
    // Called outside the lock on purpose: the host's logger may block on a file
    // or a mutex of its own, and holding ours across that would let a slow log
    // line serialize every thread in the engine.
    if (sink) sink(static_cast<int>(level), message);
}

} // namespace mw::native::log
