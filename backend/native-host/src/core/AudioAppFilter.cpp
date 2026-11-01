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

#include "mw/native/AudioAppFilter.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <mutex>

namespace mw::native {

namespace {

std::mutex g_Mutex;
std::vector<std::string> g_Names;
std::atomic<uint64_t> g_Generation{0};

} // namespace

std::string AudioAppFilter::normalise(const std::string& exeName)
{
    std::string s = exeName;
    // File name only: "C:\Games\Foo\foo.exe" and "foo.exe" are the same app.
    const size_t slash = s.find_last_of("\\/");
    if (slash != std::string::npos) s = s.substr(slash + 1);
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    // ASCII lower case. Non-ASCII bytes (UTF-8) are left alone: Windows file
    // names compare case-insensitively, but an executable name outside ASCII
    // is rare enough that an exact match is the honest fallback.
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

void AudioAppFilter::set(const std::vector<std::string>& exeNames)
{
    std::vector<std::string> next;
    next.reserve(exeNames.size());
    for (const std::string& name : exeNames) {
        std::string n = normalise(name);
        if (!n.empty()) next.push_back(std::move(n));
    }
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());

    std::lock_guard<std::mutex> lock(g_Mutex);
    if (next == g_Names && g_Generation.load(std::memory_order_relaxed) != 0) return;
    g_Names = std::move(next);
    g_Generation.fetch_add(1, std::memory_order_release);
}

std::vector<std::string> AudioAppFilter::get()
{
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_Names;
}

uint64_t AudioAppFilter::generation()
{
    return g_Generation.load(std::memory_order_acquire);
}

} // namespace mw::native
