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

#include <cstddef>
#include <cstdint>
#include <string>
// The browser's microphone, as it reaches the relay: RTP packets on the
// stream's own audio track, answered sendrecv (MicUplink.h). libdatachannel
// hands Track::onMessage the RTCP of that track as well, as plain binary — the
// Control type it had internally is gone by then — so the two are told apart
// here, by the RFC 5761 rule every bundled stream relies on: a second byte in
// 192..223 is RTCP (SR 200, RR 201, SDES 202, BYE 203, APP 204, FB 205/206),
// anything else is RTP with a payload type below 96+32.
namespace mw::mic {

/// One RTP packet, read in place: `payload` points into the caller's buffer.
struct RtpPacket
{
    uint8_t payloadType = 0;
    uint16_t sequence = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
};

/// RTCP by RFC 5761 §4: the packet type of an RTCP header sits where RTP
/// keeps marker + payload type, and 192..223 is never a dynamic payload.
inline bool isRtcp(const uint8_t* data, size_t size)
{
    return size >= 2 && data[1] >= 192 && data[1] <= 223;
}

/// Reads an RTP packet: version 2, CSRCs, a header extension and padding are
/// all stepped over. False for anything that is not one (RTCP included), or
/// whose lengths do not add up — a truncated datagram must never be decoded.
inline bool parseRtp(const uint8_t* data, size_t size, RtpPacket& out)
{
    if (!data || size < 12 || isRtcp(data, size)) return false;
    if ((data[0] >> 6) != 2) return false;

    const bool padding = (data[0] & 0x20) != 0;
    const bool extension = (data[0] & 0x10) != 0;
    const size_t csrcs = data[0] & 0x0F;

    size_t offset = 12 + 4 * csrcs;
    if (offset > size) return false;
    if (extension) {
        if (offset + 4 > size) return false;
        const size_t words = (size_t(data[offset + 2]) << 8) | data[offset + 3];
        offset += 4 + 4 * words;
        if (offset > size) return false;
    }

    size_t end = size;
    if (padding) {
        const size_t pad = data[size - 1];
        if (pad == 0 || offset + pad > size) return false;
        end -= pad;
    }

    out.payloadType = data[1] & 0x7F;
    out.sequence = static_cast<uint16_t>((data[2] << 8) | data[3]);
    out.timestamp = (uint32_t(data[4]) << 24) | (uint32_t(data[5]) << 16) |
                    (uint32_t(data[6]) << 8) | uint32_t(data[7]);
    out.ssrc = (uint32_t(data[8]) << 24) | (uint32_t(data[9]) << 16) | (uint32_t(data[10]) << 8) |
               uint32_t(data[11]);
    out.payload = data + offset;
    out.payloadSize = end - offset;
    return true;
}

/// How far `next` is ahead of `last` on the 16-bit sequence circle: 1 for the
/// packet that follows, 0 for a repeat, negative for one that arrives late.
inline int sequenceDelta(uint16_t last, uint16_t next)
{
    return static_cast<int16_t>(static_cast<uint16_t>(next - last));
}

/// What the browser's answer says of the stream's audio section (mid `audio`).
struct AudioAnswer
{
    bool found = false;
    /// a=sendrecv or a=sendonly: the browser will send on it.
    bool sends = false;
    /// An a=ssrc line: libdatachannel routes incoming RTP by SSRC alone, so a
    /// sending section without one delivers nothing to the track.
    bool hasSsrc = false;
};

inline AudioAnswer readAudioAnswer(const std::string& sdp)
{
    AudioAnswer out;
    size_t start = 0;
    while (start < sdp.size()) {
        size_t next = sdp.find("\nm=", start);
        const size_t end = next == std::string::npos ? sdp.size() : next + 1;
        const std::string section = sdp.substr(start, end - start);
        const bool isAudio =
            section.find("a=mid:audio\r") != std::string::npos ||
            section.find("a=mid:audio\n") != std::string::npos ||
            section.size() >= 11 && section.compare(section.size() - 11, 11, "a=mid:audio") == 0;
        if (isAudio) {
            out.found = true;
            out.sends = section.find("a=sendrecv") != std::string::npos ||
                        section.find("a=sendonly") != std::string::npos;
            out.hasSsrc = section.find("a=ssrc:") != std::string::npos;
            return out;
        }
        if (next == std::string::npos) break;
        start = next + 1;
    }
    return out;
}

// ── The microphone on the WebSocket transport ──────────────────────────────
// That transport has no RTP track to answer, so the page sends its microphone
// as binary messages on the stream's own socket — the one way in through a
// tunnel that carries no UDP (Cloudflare Tunnel without a port forward):
//
//   [0x4D 'M'][codec:1][sequence:2, big-endian][payload]
//
// Opus is one 20 ms packet from the page's WebCodecs AudioEncoder; PCM is
// 16-bit little-endian mono at 48 kHz, for a browser without one. Every
// message the page sends otherwise is text, so the first byte cannot collide.

enum class MicCodec : uint8_t
{
    Opus = 1,
    Pcm16 = 2,
};

constexpr uint8_t kMicFrameTag = 0x4D;
constexpr size_t kMicFrameHeader = 4;

struct MicFrame
{
    MicCodec codec = MicCodec::Opus;
    uint16_t sequence = 0;
    const uint8_t* payload = nullptr;
    size_t payloadSize = 0;
};

/// False for anything that is not a microphone frame, an unknown codec, an
/// empty payload, or PCM cut through a sample.
inline bool parseMicFrame(const uint8_t* data, size_t size, MicFrame& out)
{
    if (!data || size <= kMicFrameHeader || data[0] != kMicFrameTag) return false;
    if (data[1] != uint8_t(MicCodec::Opus) && data[1] != uint8_t(MicCodec::Pcm16)) return false;
    out.codec = static_cast<MicCodec>(data[1]);
    out.sequence = static_cast<uint16_t>((data[2] << 8) | data[3]);
    out.payload = data + kMicFrameHeader;
    out.payloadSize = size - kMicFrameHeader;
    if (out.codec == MicCodec::Pcm16 && out.payloadSize % 2 != 0) return false;
    return true;
}

} // namespace mw::mic
