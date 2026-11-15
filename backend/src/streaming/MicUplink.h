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

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
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
//
// The administrator can refuse viewers' microphones while streams run
// (settings.json viewer_mic_allowed, issue #69). Whether this process takes
// them is a STATE (hostAllows()), not an event: start() and every packet read
// it, so an instance that starts after the refusal is refused too. The server
// sets it from its file at startup and at each change (allowAllByHost /
// refuseAllByHost), and gives it to each stream worker as the worker joins the
// pool and at each change after (applyHostState). Refusing also revokes every
// started instance: its device closes, whatever arrives after is dropped
// unread, and its relay tells the page. A revoked instance stays revoked, even
// once microphones are allowed again: only a new stream gets one.
class MicUplink : public std::enable_shared_from_this<MicUplink>
{
public:
    /// The device a started instance plays into: makeOutput() unless a test
    /// gives its own.
    using OutputFactory =
        std::function<std::unique_ptr<mw::mic::IMicOutput>(const std::string& device)>;

    /// What the viewer reads when the administrator refused microphones.
    static constexpr const char* kRefusedByHost = "this host does not accept viewers' microphones";

    /// @param device settings.json `mic_device` (MicOutput.h, makeOutput).
    explicit MicUplink(std::string device, OutputFactory output = nullptr);
    ~MicUplink();

    MicUplink(const MicUplink&) = delete;
    MicUplink& operator=(const MicUplink&) = delete;

    /// Opens the decoder and the device. False with `why` (a viewer-facing
    /// sentence) when either cannot be had; the session then offers no mic.
    bool start(std::string& why);

    /// Closes the device and logs what the session carried. Idempotent; also
    /// run by the destructor.
    void stop();

    /// Refused under a running stream: the device closes now, every packet
    /// after is dropped unread, and the hook set by setOnRevoked() runs once,
    /// with `why`. Any thread. Idempotent.
    void revoke(const std::string& why);
    bool revoked() const { return m_Revoked.load(std::memory_order_acquire); }

    /// Run once by revoke(), on the revoking thread: the relay that carries
    /// this microphone posts the page's message to its own thread from there.
    void setOnRevoked(std::function<void(const std::string&)> hook);

    /// Revokes every started instance of this process. Returns how many.
    static int revokeAll(const std::string& why);

    /// Whether this process takes a viewer's microphone (true until told).
    static bool hostAllows();
    /// Sets that state; false also revokes every started instance. What a
    /// worker does with the server's word, and the server at startup.
    static void applyHostState(bool allowed);

    /// The administrator changed viewer_mic_allowed: applyHostState() here,
    /// then the hook main.cpp set, which gives the state to every worker.
    static void allowAllByHost();
    static void refuseAllByHost();
    static void setHostStateElsewhere(std::function<void(bool allowed)> hook);

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

    /// The session's gate, before any device is looked at: why a requested
    /// microphone is refused, or empty. The administrator may refuse them all
    /// (settings.json viewer_mic_allowed, issue #69). Otherwise it only lands
    /// where the game is when the streamed host is this machine (a remote
    /// Sunshine would never hear it: GameStream carries no microphone), and
    /// only the owner may send one, never an invited player — the clipboard's
    /// rule.
    static std::string refusal(bool hostIsHere, bool owner, bool hostAllows)
    {
        if (!hostAllows) return kRefusedByHost;
        if (!hostIsHere)
            return "the host is another machine, and a microphone can only reach the one "
                   "MoonlightWeb runs on";
        if (!owner) return "an invited player cannot send a microphone";
        return {};
    }

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
    const OutputFactory m_OutputFactory;
    std::atomic<bool> m_Revoked{false};
    std::function<void(const std::string&)> m_OnRevoked; // under m_Mutex
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
