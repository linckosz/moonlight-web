/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * The bench switch debug_ice_drop_udp (IceUdpDrop.h).
 *
 * What the bench relies on: every UDP candidate is recognised, in either case
 * and in either form (trickled or inside a description), while every TCP one —
 * the passive candidate first of all — and every other SDP line goes through
 * untouched.
 */

#include "../src/streaming/IceUdpDrop.h"

#include "test_framework.h"

#include <string>

namespace {

void testUdpCandidatesAreRecognised()
{
    // libjuice's own lines, and Chrome's.
    CHECK(mw::ice::isUdpCandidate("candidate:1 1 UDP 2122317823 192.168.1.66 48552 typ host"));
    CHECK(mw::ice::isUdpCandidate(
        "candidate:3 1 udp 1686052607 198.51.100.129 61234 typ srflx raddr 0.0.0.0 rport 0"));
    CHECK(mw::ice::isUdpCandidate(
        "a=candidate:842163049 1 udp 2122260223 c0a8a9e2-1f3e.local 53172 typ host\r\n"));
    CHECK(mw::ice::isUdpCandidate("candidate:9 1 Udp 1 10.0.0.1 9 typ host"));
}

void testTcpCandidatesAndOtherLinesAreKept()
{
    CHECK(!mw::ice::isUdpCandidate(
        "candidate:2 1 TCP 2105524479 198.51.100.10 48552 typ host tcptype passive"));
    CHECK(!mw::ice::isUdpCandidate("a=candidate:4 1 tcp 1518280447 192.168.1.66 9 typ host "
                                   "tcptype active"));
    CHECK(!mw::ice::isUdpCandidate("a=end-of-candidates"));
    CHECK(!mw::ice::isUdpCandidate("a=ice-ufrag:udp"));
    CHECK(!mw::ice::isUdpCandidate(""));
    // A truncated line is not one we can read a transport from.
    CHECK(!mw::ice::isUdpCandidate("candidate:1 1"));
    CHECK(!mw::ice::isUdpCandidate("candidate:1 1 udpx 1 10.0.0.1 9 typ host"));
}

void testStripKeepsEverythingButUdpCandidates()
{
    const std::string sdp = "v=0\r\n"
                            "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                            "a=ice-ufrag:abcd\r\n"
                            "a=candidate:1 1 UDP 2122317823 192.168.1.66 48552 typ host\r\n"
                            "a=candidate:2 1 TCP 2105524479 192.168.1.66 48552 typ host "
                            "tcptype passive\r\n"
                            "a=candidate:3 1 udp 1686052607 198.51.100.10 48552 typ srflx\r\n"
                            "a=end-of-candidates\r\n";
    const std::string kept = "v=0\r\n"
                             "m=application 9 UDP/DTLS/SCTP webrtc-datachannel\r\n"
                             "a=ice-ufrag:abcd\r\n"
                             "a=candidate:2 1 TCP 2105524479 192.168.1.66 48552 typ host "
                             "tcptype passive\r\n"
                             "a=end-of-candidates\r\n";
    CHECK_EQ(mw::ice::stripUdpCandidates(sdp), kept);

    // Bare line endings, and a last line with none.
    CHECK_EQ(mw::ice::stripUdpCandidates("a=x\na=candidate:1 1 udp 1 10.0.0.1 9 typ host"),
             std::string("a=x\n"));
    CHECK_EQ(mw::ice::stripUdpCandidates(""), std::string());
    CHECK_EQ(mw::ice::stripUdpCandidates(kept), kept);
}

} // namespace

void run_ice_udp_drop_tests()
{
    testUdpCandidatesAreRecognised();
    testTcpCandidatesAndOtherLinesAreKept();
    testStripKeepsEverythingButUdpCandidates();
}
