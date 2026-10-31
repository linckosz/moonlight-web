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

#include "MicJitter.h"
#include "MicOutput.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

struct OpusDecoder;

// The viewer's microphone, played into this machine's virtual microphone.
//
// On WebRTC it rides the stream's own audio track: when the browser asked for
// it and the session can take it (Session.cpp: the host is this machine, the
// viewer is the owner), the relay offers that m=audio section sendrecv instead
// of sendonly, the page answers it with its getUserMedia track, and every RTP
// packet the browser sends on it lands in onPacket(). No second track, no
// renegotiation, and the host's own sound is not touched: it keeps the same
// section the other way, with its NetEq on the browser's side.
//
// On the WebSocket transport — the one a tunnel without UDP leaves — the page
// encodes the track itself (WebCodecs Opus, else PCM) and sends it on the
// stream's socket; StreamRelay hands each message to onSocketFrame().
//
// Here: Opus (libopus, mono 48 kHz, in-band FEC for a single lost packet and
// PLC beyond) or PCM → JitterRing → the OS's device (MicOutput.h). One
// instance per session; the relay holds it and stops it with its connection.
class MicUplink
{
public:
    /// @param device settings.json `mic_device` (MicOutput.h, makeOutput).
    explicit MicUplink(std::string device);
    ~MicUplink();

    MicUplink(const MicUplink&) = delete;
    MicUplink& operator=(const MicUplink&) = delete;

    /// Opens the decoder and the device. False with `why` (a viewer-facing
    /// sentence) when either cannot be had; the session then offers no mic.
    bool start(std::string& why);

    /// Closes the device and logs what the session carried. Idempotent; also
    /// run by the destructor.
    void stop();

    /// One packet as Track::onMessage delivers it — RTP, or the track's RTCP,
    /// which is skipped. Any thread; serialised inside.
    void onPacket(const uint8_t* data, size_t size);

    /// One binary message the page sent on the WebSocket transport's socket
    /// (MicRtp.h, parseMicFrame): Opus from its WebCodecs encoder, or PCM.
    /// Same thread rules as onPacket.
    void onSocketFrame(const uint8_t* data, size_t size);

    /// What the output plays into, once started.
    std::string deviceName() const;

    /// Empty when a mic can be played on this machine, else why not.
    static std::string unavailable(const std::string& device);

    /// The only payload type the relay offers on the audio section.
    static constexpr uint8_t kOpusPayloadType = 111;
    static constexpr int kRate = 48000;
    /// A gap wider than this many packets is a new talk spurt, not a loss:
    /// concealing a second of silence would only delay what comes next.
    static constexpr int kMaxConcealedPackets = 10;

private:
    /// The socket's frames as one sender, never mistaken for an RTP SSRC
    /// (a browser draws those at random, and this one it cannot pick).
    static constexpr uint32_t kSocketSource = 0xFFFFFFFFu;

    void decodeLocked(const uint8_t* payload, size_t size, bool fec, int frames);
    /// One Opus packet in sequence: losses concealed, late ones dropped.
    void opusLocked(uint16_t sequence, uint32_t source, const uint8_t* payload, size_t size,
                    const char* road);
    void countLocked(size_t payloadSize, const char* road);

    const std::string m_Device;
    // 40 ms held before playing, 150 ms at most: enough for Wi-Fi's bursts,
    // short enough that the voice still lands with the picture it goes with.
    mw::mic::JitterRing m_Ring{kRate / 25, kRate * 3 / 20};
    std::unique_ptr<mw::mic::IMicOutput> m_Output;

    mutable std::mutex m_Mutex;
    OpusDecoder* m_Decoder = nullptr;
    std::vector<float> m_Pcm;
    bool m_Started = false;
    bool m_HaveSequence = false;
    uint16_t m_LastSequence = 0;
    uint32_t m_Ssrc = 0;
    int m_LastFrames = kRate / 50;

    uint64_t m_Packets = 0;
    uint64_t m_Lost = 0;
    uint64_t m_Recovered = 0;
    uint64_t m_Late = 0;
    uint64_t m_Bad = 0;
    uint64_t m_PayloadBytes = 0;
    bool m_LoggedFirst = false;
    bool m_LoggedForeignPt = false;
};
