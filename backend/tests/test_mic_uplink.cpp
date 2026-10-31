/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 MoonlightWeb contributors.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The viewer's microphone on the way in (MicRtp.h, MicJitter.h): the RTP the
 * browser sends told apart from its RTCP and read without trusting a length,
 * the answer's audio section read for what it promises, and the ring between
 * the network and the device kept short — primed before it plays, silent and
 * primed again when it runs dry, trimmed when it grows.
 */

#include "../src/streaming/MicJitter.h"
#include "../src/streaming/MicRtp.h"

#include "test_framework.h"

#include <vector>

namespace {

std::vector<uint8_t> rtp(uint16_t seq, uint8_t pt = 111, size_t payload = 4)
{
    std::vector<uint8_t> p = {
        0x80, pt, uint8_t(seq >> 8), uint8_t(seq), 0, 0, 0x03, 0xC0, 0x12, 0x34, 0x56, 0x78};
    for (size_t i = 0; i < payload; ++i)
        p.push_back(uint8_t(0xA0 + i));
    return p;
}

} // namespace

void run_mic_uplink_tests()
{
    SECTION("MicRtp — a plain packet");
    {
        const auto p = rtp(0x1234);
        mw::mic::RtpPacket out;
        CHECK(mw::mic::parseRtp(p.data(), p.size(), out));
        CHECK_EQ(out.payloadType, 111);
        CHECK_EQ(out.sequence, 0x1234);
        CHECK_EQ(out.timestamp, 0x3C0u);
        CHECK_EQ(out.ssrc, 0x12345678u);
        CHECK_EQ(out.payloadSize, 4u);
        CHECK_EQ(out.payload[0], 0xA0);
    }

    SECTION("MicRtp — RTCP is never taken for RTP");
    {
        // Receiver report (201) and NACK feedback (205), as Chrome sends them
        // on the same track: libdatachannel hands both to onMessage.
        const uint8_t rr[] = {0x81, 201, 0x00, 0x07, 0, 0, 0, 1, 0, 0, 0, 2};
        const uint8_t fb[] = {0x81, 205, 0x00, 0x03, 0, 0, 0, 1, 0, 0, 0, 2};
        mw::mic::RtpPacket out;
        CHECK(mw::mic::isRtcp(rr, sizeof rr));
        CHECK(mw::mic::isRtcp(fb, sizeof fb));
        CHECK(!mw::mic::parseRtp(rr, sizeof rr, out));
        CHECK(!mw::mic::parseRtp(fb, sizeof fb, out));
        // A marker bit on PT 111 is 0xEF: still RTP.
        auto marked = rtp(1);
        marked[1] = 0x80 | 111;
        CHECK(!mw::mic::isRtcp(marked.data(), marked.size()));
        CHECK(mw::mic::parseRtp(marked.data(), marked.size(), out));
        CHECK_EQ(out.payloadType, 111);
    }

    SECTION("MicRtp — CSRCs, an extension and padding are stepped over");
    {
        auto p = rtp(7, 111, 0);
        p[0] = 0x80 | 0x20 | 0x10 | 0x01; // padding, extension, one CSRC
        const uint8_t csrc[] = {0xCA, 0xFE, 0xBA, 0xBE};
        const uint8_t ext[] = {0xBE, 0xDE, 0x00, 0x01, 0x10, 0xFF, 0x00, 0x00};
        p.insert(p.end(), csrc, csrc + 4);
        p.insert(p.end(), ext, ext + 8);
        p.push_back(0x42); // the payload
        p.push_back(0x00);
        p.push_back(0x02); // two bytes of padding
        mw::mic::RtpPacket out;
        CHECK(mw::mic::parseRtp(p.data(), p.size(), out));
        CHECK_EQ(out.payloadSize, 1u);
        CHECK_EQ(out.payload[0], 0x42);
    }

    SECTION("MicRtp — lengths that do not add up are refused");
    {
        mw::mic::RtpPacket out;
        const auto p = rtp(1);
        CHECK(!mw::mic::parseRtp(p.data(), 11, out)); // shorter than a header
        auto v1 = rtp(1);
        v1[0] = 0x40; // version 1
        CHECK(!mw::mic::parseRtp(v1.data(), v1.size(), out));
        auto csrc = rtp(1, 111, 0);
        csrc[0] = 0x8F; // fifteen CSRCs, none present
        CHECK(!mw::mic::parseRtp(csrc.data(), csrc.size(), out));
        auto ext = rtp(1, 111, 4);
        ext[0] = 0x90;
        ext[14] = 0xFF; // an extension far longer than the packet
        CHECK(!mw::mic::parseRtp(ext.data(), ext.size(), out));
        auto pad = rtp(1, 111, 2);
        pad[0] = 0xA0;
        pad.back() = 9; // more padding than payload
        CHECK(!mw::mic::parseRtp(pad.data(), pad.size(), out));
        CHECK(!mw::mic::parseRtp(nullptr, 0, out));
    }

    SECTION("MicRtp — sequence deltas wrap");
    {
        CHECK_EQ(mw::mic::sequenceDelta(10, 11), 1);
        CHECK_EQ(mw::mic::sequenceDelta(10, 10), 0);
        CHECK_EQ(mw::mic::sequenceDelta(10, 9), -1);
        CHECK_EQ(mw::mic::sequenceDelta(65535, 0), 1);
        CHECK_EQ(mw::mic::sequenceDelta(65534, 2), 4);
        CHECK_EQ(mw::mic::sequenceDelta(2, 65534), -4);
    }

    SECTION("MicRtp — the answer's audio section");
    {
        const std::string head = "v=0\r\no=- 1 2 IN IP4 0.0.0.0\r\ns=-\r\nt=0 0\r\n";
        const std::string video = "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video\r\na=recvonly\r\n";
        const std::string sends = "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio\r\na=sendrecv\r\n"
                                  "a=msid:- t\r\na=ssrc:42 cname:x\r\n";
        const std::string mute = "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio\r\na=recvonly\r\n";
        const std::string bare = "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=mid:audio\r\na=sendrecv\r\n";

        auto a = mw::mic::readAudioAnswer(head + video + sends);
        CHECK(a.found && a.sends && a.hasSsrc);
        a = mw::mic::readAudioAnswer(head + mute + video);
        CHECK(a.found && !a.sends);
        a = mw::mic::readAudioAnswer(head + bare);
        CHECK(a.found && a.sends && !a.hasSsrc);
        // Another section's lines are not the audio section's.
        const std::string videoSsrc =
            "m=video 9 UDP/TLS/RTP/SAVPF 96\r\na=mid:video\r\na=sendrecv\r\na=ssrc:7 cname:y\r\n";
        a = mw::mic::readAudioAnswer(head + mute + videoSsrc);
        CHECK(a.found && !a.sends && !a.hasSsrc);
        CHECK(!mw::mic::readAudioAnswer(head + video).found);
    }

    SECTION("MicRtp — the WebSocket transport's frames");
    {
        mw::mic::MicFrame f;
        const uint8_t opus[] = {0x4D, 1, 0x12, 0x34, 0xAA, 0xBB, 0xCC};
        CHECK(mw::mic::parseMicFrame(opus, sizeof opus, f));
        CHECK(f.codec == mw::mic::MicCodec::Opus);
        CHECK_EQ(f.sequence, 0x1234);
        CHECK_EQ(f.payloadSize, 3u);
        CHECK_EQ(f.payload[0], 0xAA);
        const uint8_t pcm[] = {0x4D, 2, 0, 1, 0x00, 0x40, 0x00, 0xC0};
        CHECK(mw::mic::parseMicFrame(pcm, sizeof pcm, f));
        CHECK(f.codec == mw::mic::MicCodec::Pcm16);
        CHECK_EQ(f.payloadSize, 4u);
        // Not a frame: another tag, an unknown codec, no payload, PCM cut
        // through a sample.
        const uint8_t tag[] = {0x7B, 1, 0, 0, 1};
        const uint8_t codec[] = {0x4D, 9, 0, 0, 1};
        const uint8_t empty[] = {0x4D, 1, 0, 0};
        const uint8_t odd[] = {0x4D, 2, 0, 0, 1, 2, 3};
        CHECK(!mw::mic::parseMicFrame(tag, sizeof tag, f));
        CHECK(!mw::mic::parseMicFrame(codec, sizeof codec, f));
        CHECK(!mw::mic::parseMicFrame(empty, sizeof empty, f));
        CHECK(!mw::mic::parseMicFrame(odd, sizeof odd, f));
        CHECK(!mw::mic::parseMicFrame(nullptr, 0, f));
    }

    SECTION("JitterRing — silent until primed, then plays in order");
    {
        mw::mic::JitterRing ring(4, 16);
        float out[4] = {9, 9, 9, 9};
        CHECK_EQ(ring.pull(out, 4), 0u);
        CHECK(out[0] == 0.0f && out[3] == 0.0f);
        const float a[3] = {1, 2, 3};
        ring.push(a, 3);
        CHECK_EQ(ring.pull(out, 2), 0u); // three held, four wanted before playing
        const float b[1] = {4};
        ring.push(b, 1);
        CHECK_EQ(ring.pull(out, 2), 2u);
        CHECK(out[0] == 1.0f && out[1] == 2.0f);
        CHECK_EQ(ring.size(), 2u);
    }

    SECTION("JitterRing — running dry goes silent and primes again");
    {
        mw::mic::JitterRing ring(2, 16);
        const float a[2] = {1, 2};
        ring.push(a, 2);
        float out[3];
        CHECK_EQ(ring.pull(out, 3), 2u);
        CHECK(out[0] == 1.0f && out[1] == 2.0f && out[2] == 0.0f);
        CHECK_EQ(ring.stats().underruns, 1u);
        const float b[1] = {5};
        ring.push(b, 1);
        CHECK_EQ(ring.pull(out, 1), 0u); // one held, two before playing again
        ring.push(b, 1);
        CHECK_EQ(ring.pull(out, 1), 1u);
    }

    SECTION("JitterRing — a backlog is trimmed to the target, oldest first");
    {
        mw::mic::JitterRing ring(4, 8);
        std::vector<float> burst(9);
        for (size_t i = 0; i < burst.size(); ++i)
            burst[i] = float(i);
        ring.push(burst.data(), burst.size()); // one past the cap
        CHECK_EQ(ring.size(), 4u);
        CHECK_EQ(ring.stats().trims, 1u);
        CHECK_EQ(ring.stats().trimmed, 5u);
        float out[4];
        CHECK_EQ(ring.pull(out, 4), 4u);
        CHECK(out[0] == 5.0f && out[3] == 8.0f); // the newest four kept

        // A burst far longer than the ring never holds more than the cap.
        std::vector<float> flood(1000, 1.0f);
        ring.push(flood.data(), flood.size());
        CHECK(ring.size() <= 8u);
    }
}
