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
#pragma once

#include <algorithm>
#include <cctype>
#include <string>

// Which Windows playback device the viewer's microphone may play into, by its
// friendly name (MicOutputWindows.cpp). Apart from that file so the tests
// read the same rule on every OS.
//
// Only the render side of a virtual cable, whose capture side is what the
// host's apps record. Never a real output, and never on its own:
//   - "Steam Streaming Speakers": Sunshine captures it, and the viewer would
//     hear their own voice come back;
//   - VoiceMeeter's inputs, although they are VB-Audio's too: VoiceMeeter
//     mixes them into its outputs, which are the PC's speakers;
//   - "Steam Streaming Microphone": its driver hands the bytes of its render
//     side (48 kHz stereo as Steam installs it) to its capture side (44.1 kHz
//     mono) unconverted, so a voice arrives more than an octave low and
//     slowed — a 440 Hz tone recorded at 202 Hz (01/11/2026). An admin who
//     lined both formats up can still name it in mic_device.
namespace mw::mic {

constexpr int kNotACable = -1;

inline std::string lowerAscii(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Preference among the cables the automatic choice takes, lower first:
///   0 — VB-CABLE and its A/B/C/D and Hi-Fi variants, whose render sides are
///       all named "… Input (VB-Audio … Cable …)".
/// kNotACable for anything else. `friendlyName` is UTF-8.
inline int cableRank(const std::string& friendlyName)
{
    const std::string n = lowerAscii(friendlyName);
    auto has = [&n](const char* part) { return n.find(part) != std::string::npos; };
    if (has("voicemeeter")) return kNotACable;
    if (has("vb-audio") && has("cable") && has("input")) return 0;
    return kNotACable;
}

/// Steam Remote Play's microphone cable, which the automatic choice passes
/// over (see the top of this file): named, so the viewer is told why.
inline bool isSteamMicrophone(const std::string& friendlyName)
{
    return lowerAscii(friendlyName).find("steam streaming microphone") != std::string::npos;
}

/// The admin's `mic_device`, matched as part of a device's name, any case.
inline bool nameMatches(const std::string& friendlyName, const std::string& wanted)
{
    return !wanted.empty() &&
           lowerAscii(friendlyName).find(lowerAscii(wanted)) != std::string::npos;
}

/// Empty or "auto": the first cable by cableRank().
inline bool wantsAutoDevice(const std::string& device)
{
    return device.empty() || lowerAscii(device) == "auto";
}

} // namespace mw::mic
