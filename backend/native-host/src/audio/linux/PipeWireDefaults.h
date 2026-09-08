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

#include <cstring>
#include <string>

/// The two things HostMute decides by reading text out of the PipeWire graph:
/// which sink is the default output, and whether that sink's monitor carries
/// its volume. Kept apart from HostMute.h, and free of any PipeWire header, so
/// they can be tested on a machine with no PipeWire at all — which is every
/// machine this project builds on but the Linux bench.
namespace mw::native::audio {

/// The sink named by the `default.audio.sink` metadata value, which the session
/// manager writes as `{"name":"alsa_output.pci-0000_c4_00.6…"}`. Empty for
/// anything that is not that shape — an unset default included.
inline std::string sinkNameFromDefaultJson(const std::string& json)
{
    static const char kKey[] = "\"name\"";
    size_t at = json.find(kKey);
    if (at == std::string::npos) return {};
    at = json.find(':', at + sizeof(kKey) - 1);
    if (at == std::string::npos) return {};
    for (++at; at < json.size() && (json[at] == ' ' || json[at] == '\t'); ++at) {}
    if (at >= json.size() || json[at] != '"') return {};
    std::string name;
    for (++at; at < json.size(); ++at) {
        if (json[at] == '\\' && at + 1 < json.size()) {
            name.push_back(json[++at]);
            continue;
        }
        if (json[at] == '"') return name;
        name.push_back(json[at]);
    }
    // Unterminated: a name we cannot trust is no name at all.
    return {};
}

/// The value of a `monitor.channel-volumes` node property as PipeWire reads a
/// boolean property. Absent (nullptr) means false, and that is the case that
/// decides for a mute: on a real output the property is not there at all, while
/// the null sinks the PulseAudio compat layer creates carry it as "true".
/// HostMute.h has the measurement.
inline bool monitorCarriesVolume(const char* property)
{
    return property && (std::strcmp(property, "true") == 0 || std::strcmp(property, "1") == 0);
}

} // namespace mw::native::audio
