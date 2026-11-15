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

#include "MicUplink.h"

#include "MicRtp.h"

#include <QDebug>
#include <QString>

#include <opus.h>

namespace {

void logLine(const std::string& line)
{
    qInfo().noquote() << "[Mic]" << QString::fromStdString(line);
}

// The longest packet Opus allows: 120 ms at 48 kHz.
constexpr int kMaxFrames = 5760;

// Every started instance of this process, for refuseAllByHost(). Weak: the
// relay owns them, and one that is gone has nothing left to revoke.
std::mutex g_LiveMutex;
std::vector<std::weak_ptr<MicUplink>> g_Live;
std::function<void(bool)> g_HostStateElsewhere;
// The host's word for this whole process (hostAllows()).
std::atomic<bool> g_HostAllows{true};

} // namespace

MicUplink::MicUplink(std::string device, OutputFactory output)
    : m_Device(std::move(device))
    , m_OutputFactory(std::move(output))
    , m_Pcm(kMaxFrames)
{}

MicUplink::~MicUplink()
{
    stop();
}

std::string MicUplink::unavailable(const std::string& device)
{
    return mw::mic::unavailable(device);
}

bool MicUplink::start(std::string& why)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Started) return true;
    if (revoked() || !hostAllows()) {
        why = kRefusedByHost;
        return false;
    }

    int err = OPUS_OK;
    m_Decoder = opus_decoder_create(kRate, 1, &err);
    if (!m_Decoder || err != OPUS_OK) {
        why = std::string("the Opus decoder could not be created (") + opus_strerror(err) + ")";
        m_Decoder = nullptr;
        return false;
    }

    m_Output =
        m_OutputFactory ? m_OutputFactory(m_Device) : mw::mic::makeOutput(m_Device, &logLine);
    if (!m_Output || !m_Output->start(m_Ring, why)) {
        m_Output.reset();
        opus_decoder_destroy(m_Decoder);
        m_Decoder = nullptr;
        if (why.empty()) why = "this machine has no device to play a microphone into";
        return false;
    }
    m_Started = true;
    logLine("ready: the viewer's microphone plays into \"" + m_Output->deviceName() + "\"");
    // Only an instance a shared_ptr owns can be found again (the session's
    // always is); a bare one is never revoked from outside.
    if (const std::weak_ptr<MicUplink> self = weak_from_this(); !self.expired()) {
        std::lock_guard<std::mutex> live(g_LiveMutex);
        g_Live.push_back(self);
    }
    return true;
}

void MicUplink::revoke(const std::string& why)
{
    if (m_Revoked.exchange(true, std::memory_order_acq_rel)) return;
    logLine("refused under the running stream: " + why);
    stop();
    std::function<void(const std::string&)> hook;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        hook = std::move(m_OnRevoked);
        m_OnRevoked = nullptr;
    }
    if (hook) hook(why);
}

void MicUplink::setOnRevoked(std::function<void(const std::string&)> hook)
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_OnRevoked = std::move(hook);
}

int MicUplink::revokeAll(const std::string& why)
{
    std::vector<std::shared_ptr<MicUplink>> live;
    {
        std::lock_guard<std::mutex> lock(g_LiveMutex);
        for (const auto& weak : g_Live)
            if (auto mic = weak.lock()) live.push_back(std::move(mic));
        g_Live.clear();
    }
    // Outside the list's lock: revoke() stops the device, which waits for its
    // thread, and runs the relay's hook.
    int revokedNow = 0;
    for (const auto& mic : live) {
        if (mic->revoked()) continue;
        mic->revoke(why);
        ++revokedNow;
    }
    return revokedNow;
}

bool MicUplink::hostAllows()
{
    return g_HostAllows.load(std::memory_order_acquire);
}

void MicUplink::applyHostState(bool allowed)
{
    // The state first: an instance starting while the others are revoked
    // reads it and is refused.
    const bool was = g_HostAllows.exchange(allowed, std::memory_order_acq_rel);
    if (was != allowed)
        logLine(allowed ? "viewers' microphones allowed by the host"
                        : "viewers' microphones refused by the host");
    if (allowed) return;
    const int here = revokeAll(kRefusedByHost);
    if (here > 0) logLine(std::to_string(here) + " closed under a running stream");
}

namespace {

void hostStateEverywhere(bool allowed)
{
    MicUplink::applyHostState(allowed);
    std::function<void(bool)> elsewhere;
    {
        std::lock_guard<std::mutex> lock(g_LiveMutex);
        elsewhere = g_HostStateElsewhere;
    }
    if (elsewhere) elsewhere(allowed);
}

} // namespace

void MicUplink::allowAllByHost()
{
    hostStateEverywhere(true);
}

void MicUplink::refuseAllByHost()
{
    hostStateEverywhere(false);
}

void MicUplink::setHostStateElsewhere(std::function<void(bool allowed)> hook)
{
    std::lock_guard<std::mutex> lock(g_LiveMutex);
    g_HostStateElsewhere = std::move(hook);
}

void MicUplink::stop()
{
    std::unique_ptr<mw::mic::IMicOutput> output;
    {
        std::lock_guard<std::mutex> lock(m_Mutex);
        if (!m_Started) return;
        m_Started = false;
        output = std::move(m_Output);
    }
    // Outside the lock: the device's thread pulls from the ring, never from
    // here, but stopping it waits for that thread.
    if (output) output->stop();

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (m_Decoder) {
        opus_decoder_destroy(m_Decoder);
        m_Decoder = nullptr;
    }
    const auto ring = m_Ring.stats();
    // What the browser really sent, in the unit its encoder is set in.
    const double seconds = double(ring.pushed) / kRate;
    const std::string rate =
        seconds >= 1.0
            ? ", " + std::to_string(int(m_PayloadBytes * 8 / 1000.0 / seconds + 0.5)) + " kbit/s"
            : std::string();
    logLine("closed: " + std::to_string(m_Packets) + " packets" + rate + ", " +
            std::to_string(m_Lost) + " lost (" + std::to_string(m_Recovered) +
            " recovered by FEC), " + std::to_string(m_Late) + " late, " + std::to_string(m_Bad) +
            " malformed; " + std::to_string(ring.underruns) + " underruns, " +
            std::to_string(ring.trims) + " trims (" + std::to_string(ring.trimmed * 1000 / kRate) +
            " ms dropped)");
}

std::string MicUplink::deviceName() const
{
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Output ? m_Output->deviceName() : std::string();
}

void MicUplink::onPacket(const uint8_t* data, size_t size)
{
    // Refused by the host: not even read.
    if (revoked() || !hostAllows()) return;
    mw::mic::RtpPacket rtp;
    if (!mw::mic::parseRtp(data, size, rtp)) {
        if (!mw::mic::isRtcp(data, size)) {
            std::lock_guard<std::mutex> lock(m_Mutex);
            ++m_Bad;
        }
        return;
    }

    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!m_Started || !m_Decoder) return;
    if (rtp.payloadType != kOpusPayloadType) {
        if (!m_LoggedForeignPt) {
            m_LoggedForeignPt = true;
            logLine("ignoring payload type " + std::to_string(rtp.payloadType) +
                    " on the audio section (only Opus, " + std::to_string(kOpusPayloadType) +
                    ", was offered)");
        }
        return;
    }
    opusLocked(rtp.sequence, rtp.ssrc, rtp.payload, rtp.payloadSize, "Opus over RTP");
}

void MicUplink::onSocketFrame(const uint8_t* data, size_t size)
{
    // Refused by the host: not even read.
    if (revoked() || !hostAllows()) return;
    mw::mic::MicFrame frame;
    std::lock_guard<std::mutex> lock(m_Mutex);
    if (!mw::mic::parseMicFrame(data, size, frame)) {
        ++m_Bad;
        return;
    }
    if (!m_Started || !m_Decoder) return;
    if (frame.codec == mw::mic::MicCodec::Opus) {
        opusLocked(frame.sequence, kSocketSource, frame.payload, frame.payloadSize,
                   "Opus over the WebSocket");
        return;
    }

    // PCM: a reliable socket loses nothing, so there is nothing to conceal;
    // only the order is checked, as for Opus.
    countLocked(frame.payloadSize, "16-bit PCM over the WebSocket");
    if (m_HaveSequence && m_Ssrc == kSocketSource &&
        mw::mic::sequenceDelta(m_LastSequence, frame.sequence) <= 0) {
        ++m_Late;
        return;
    }
    m_HaveSequence = true;
    m_Ssrc = kSocketSource;
    m_LastSequence = frame.sequence;
    const size_t samples = frame.payloadSize / 2;
    if (m_Pcm.size() < samples) m_Pcm.resize(samples);
    for (size_t i = 0; i < samples; ++i) {
        const auto s = static_cast<int16_t>(frame.payload[2 * i] | (frame.payload[2 * i + 1] << 8));
        m_Pcm[i] = s / 32768.0f;
    }
    m_Ring.push(m_Pcm.data(), samples);
}

void MicUplink::countLocked(size_t payloadSize, const char* road)
{
    ++m_Packets;
    m_PayloadBytes += payloadSize;
    if (!m_LoggedFirst) {
        m_LoggedFirst = true;
        logLine(std::string("first packet from the browser: ") + road);
    }
}

void MicUplink::opusLocked(uint16_t sequence, uint32_t source, const uint8_t* payload, size_t size,
                           const char* road)
{
    countLocked(size, road);

    // A new sender (the page swapped its track, or a reconnect) starts its
    // own sequence: nothing to conceal across that.
    if (!m_HaveSequence || source != m_Ssrc) {
        m_HaveSequence = true;
        m_Ssrc = source;
        m_LastSequence = sequence;
        decodeLocked(payload, size, false, kMaxFrames);
        return;
    }

    const int delta = mw::mic::sequenceDelta(m_LastSequence, sequence);
    if (delta <= 0) {
        // Already played, or concealed in its place: too late to matter.
        ++m_Late;
        return;
    }
    if (delta > 1) {
        const int missing = delta - 1;
        m_Lost += static_cast<uint64_t>(missing);
        if (missing <= kMaxConcealedPackets) {
            // PLC for all but the last missing packet, whose FEC copy rides in
            // this one (the offer's useinbandfec=1 asks the browser for it).
            // Only a SILK or hybrid packet can carry it. A CELT-only one —
            // which the browser's encoder may pick at a high rate — has
            // none, and the FEC decode is then the same PLC: not a recovery.
            for (int i = 0; i < missing - 1; ++i)
                decodeLocked(nullptr, 0, false, m_LastFrames);
            decodeLocked(payload, size, true, m_LastFrames);
            if (opus_packet_has_lbrr(payload, static_cast<opus_int32>(size)) > 0) ++m_Recovered;
        }
    }
    m_LastSequence = sequence;
    decodeLocked(payload, size, false, kMaxFrames);
}

void MicUplink::decodeLocked(const uint8_t* payload, size_t size, bool fec, int frames)
{
    const int n = opus_decode_float(m_Decoder, payload, static_cast<opus_int32>(size), m_Pcm.data(),
                                    frames, fec ? 1 : 0);
    if (n < 0) {
        ++m_Bad;
        return;
    }
    if (payload && !fec) {
        const int packetFrames =
            opus_packet_get_nb_samples(payload, static_cast<opus_int32>(size), kRate);
        if (packetFrames > 0) m_LastFrames = packetFrames;
    }
    m_Ring.push(m_Pcm.data(), static_cast<size_t>(n));
}
