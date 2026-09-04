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
#include <cstdint>
#include <string>
#include <vector>

struct OpusEncoder;

namespace mw::native::audio {

/// libopus, stereo 48 kHz, 5 ms frames, tuned for the one thing this stream
/// wants: the shortest path from the host's mixer to the player's ears.
///
/// Restricted low-delay mode (CELT only): the SILK/hybrid layers add
/// look-ahead and exist for speech at low bitrates, neither of which is this.
/// The consequence to know about is that Opus's in-band FEC is a SILK feature
/// and does not exist here — nor could it at 5 ms frames. Loss is handled where
/// it already is: the browser's PLC, and the RTP NACK responder on the track.
///
/// VBR on: digital silence — a paused game, a quiet desktop — then costs a few
/// bytes a packet instead of a full 128 kbps of encoded nothing, and the
/// cadence stays (the pacer keeps one frame every 5 ms whatever is in it).
class OpusEncoder
{
public:
    static constexpr int kSampleRate = 48000;
    static constexpr int kChannels = 2;
    static constexpr int kFrameSamples = 240;
    static constexpr int kBitrate = 128000;

    OpusEncoder() = default;
    ~OpusEncoder();
    OpusEncoder(const OpusEncoder&) = delete;
    OpusEncoder& operator=(const OpusEncoder&) = delete;

    bool open(std::string& error);

    /// Encode exactly one frame (kFrameSamples per channel, interleaved
    /// floats). Returns the packet size, 0 on error (logged once).
    size_t encode(const float* interleaved, std::vector<uint8_t>& out);

    /// What libopus reports as its own version, for the session log.
    static const char* libraryVersion();

private:
    ::OpusEncoder* m_Encoder = nullptr;
    bool m_LoggedError = false;
};

} // namespace mw::native::audio
