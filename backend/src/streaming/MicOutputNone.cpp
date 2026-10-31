/*
 * MoonlightWeb — browser-based Sunshine/GameStream client.
 * Copyright (C) 2026 MoonlightWeb contributors
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

// Where there is no backend: macOS for now, and a Linux build configured
// without the PipeWire headers. Says so, and never claims a device.

#include "MicOutput.h"

namespace mw::mic {

std::unique_ptr<IMicOutput> makeOutput(const std::string& /*device*/, LogFn /*log*/)
{
    return nullptr;
}

std::string unavailable(const std::string& /*device*/)
{
#if defined(__APPLE__)
    return "a microphone cannot be played into a Mac host yet";
#else
    return "this build has no microphone output (it was configured without "
           "libpipewire-0.3-dev)";
#endif
}

} // namespace mw::mic
