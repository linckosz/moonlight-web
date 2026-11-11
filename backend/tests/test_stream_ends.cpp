/*
 * MoonlightWeb — Backend TNR. Copyright (C) 2026 Bruno Martin.
 * GPLv3 — see repository LICENSE.
 */

/**
 * How recent streams ended (StreamEnds.h): what the page that lost its link
 * reads before it decides between starting the stream again and ending it.
 */

#include "../src/streaming/StreamEnds.h"

#include "test_framework.h"

using State = StreamEnds::State;

namespace {

void begin(StreamEnds& ends, const QString& id, const QString& host, int slot, qint64 nowMs)
{
    ends.begin(id, host, slot, QStringLiteral("uid-") + host, QStringLiteral("token"), nowMs);
}

} // namespace

void run_stream_ends_tests()
{
    SECTION("StreamEnds — a crash is a worker gone unasked and unannounced");
    {
        CHECK(StreamEnds::stateAfterExit(false, false) == State::Crashed);
        // Said its session was over: the app closed, the link died under it.
        CHECK(StreamEnds::stateAfterExit(true, false) == State::Ended);
        // Asked to stop and killed once it did not in time.
        CHECK(StreamEnds::stateAfterExit(false, true) == State::Ended);
        CHECK(StreamEnds::stateAfterExit(true, true) == State::Ended);
    }

    SECTION("StreamEnds — the names the page reads");
    {
        CHECK_EQ(StreamEnds::name(State::Live), QStringLiteral("live"));
        CHECK_EQ(StreamEnds::name(State::Crashed), QStringLiteral("crashed"));
        CHECK_EQ(StreamEnds::name(State::Ended), QStringLiteral("ended"));
        CHECK_EQ(StreamEnds::name(State::Stopped), QStringLiteral("stopped"));
        CHECK_EQ(StreamEnds::name(State::Unknown), QStringLiteral("unknown"));
    }

    SECTION("StreamEnds — ids are 128 random bits");
    {
        const QString a = StreamEnds::newId();
        const QString b = StreamEnds::newId();
        CHECK_EQ(a.size(), 32);
        CHECK(a != b);
        bool hex = true;
        for (QChar c : a)
            hex = hex && QStringLiteral("0123456789abcdef").contains(c);
        CHECK(hex);
    }

    SECTION("StreamEnds — a stream is live, then ends one way");
    {
        StreamEnds ends;
        begin(ends, "s1", "A", 0, 1000);
        const StreamEnds::Entry live = ends.find("s1", 1000);
        CHECK(live.state == State::Live);
        CHECK_EQ(live.hostUuid, QStringLiteral("A"));
        CHECK_EQ(live.slot, 0);
        CHECK_EQ(live.clientUniqueId, QStringLiteral("uid-A"));
        CHECK_EQ(live.sessionToken, QStringLiteral("token"));
        CHECK_EQ(live.atMs, qint64(1000));
        ends.end("s1", State::Crashed, 2000);
        CHECK(ends.find("s1", 2000).state == State::Crashed);
        CHECK_EQ(ends.find("s1", 2000).atMs, qint64(2000));
    }

    SECTION("StreamEnds — the first end stands");
    {
        StreamEnds ends;
        begin(ends, "s1", "A", 0, 0);
        ends.end("s1", State::Stopped, 10);
        // The worker of a stopped stream, killed later, is no crash.
        ends.end("s1", State::Crashed, 20);
        CHECK(ends.find("s1", 30).state == State::Stopped);
    }

    SECTION("StreamEnds — what never streamed is not recorded");
    {
        StreamEnds ends;
        ends.end("never", State::Crashed, 0);
        CHECK_EQ(ends.size(), 0);
        CHECK(ends.find("never", 0).state == State::Unknown);
        begin(ends, "", "A", 0, 0);
        CHECK_EQ(ends.size(), 0);
    }

    SECTION("StreamEnds — an end ages out, a running stream does not");
    {
        StreamEnds ends;
        begin(ends, "long", "A", 0, 0);
        begin(ends, "short", "B", 2, 0);
        ends.end("short", State::Crashed, 1000);
        CHECK(ends.find("short", 1000 + StreamEnds::kMaxAgeMs).state == State::Crashed);
        CHECK(ends.find("short", 1001 + StreamEnds::kMaxAgeMs).state == State::Unknown);
        // Hours of streaming, then a crash: still recorded.
        const qint64 later = 3 * 3600 * 1000;
        CHECK(ends.find("long", later).state == State::Live);
        ends.end("long", State::Crashed, later);
        CHECK(ends.find("long", later + 5000).state == State::Crashed);
        // The aged entry went with the next change.
        CHECK_EQ(ends.size(), 1);
    }

    SECTION("StreamEnds — over capacity, the oldest end goes first");
    {
        StreamEnds ends;
        begin(ends, "running", "A", 0, 0);
        for (int i = 0; i < StreamEnds::kCapacity; ++i) {
            const QString id = QStringLiteral("e%1").arg(i);
            begin(ends, id, "B", 2, 10 + i);
            ends.end(id, State::Ended, 10 + i);
        }
        CHECK_EQ(ends.size(), StreamEnds::kCapacity);
        CHECK(ends.find("e0", 100).state == State::Unknown);
        CHECK(ends.find("e1", 100).state == State::Ended);
        CHECK(ends.find("running", 100).state == State::Live);
    }

    // A Windows PC's sign-in screen, streamed while Remote Desktop has left
    // the session (RemoteDesktopReturnPolicy): its worker goes with the
    // sign-in screen's session when the user signs in through it. Whether
    // that is what happened is read when the page asks: the session back on
    // the PC's screen says "signed_in", and the page starts the same card
    // again, which then streams that screen.
    SECTION("StreamEnds — the sign-in screen, signed in through");
    {
        StreamEnds ends;
        ends.begin("signin", "native", 0, "dev", "tok", 0, /*signInScreen=*/true);
        ends.begin("plain", "native", 1, "dev", "tok", 0);
        CHECK_EQ(StreamEnds::nameFor(ends.find("signin", 1), true), QStringLiteral("live"));
        ends.end("signin", State::Crashed, 10);
        ends.end("plain", State::Crashed, 10);
        CHECK_EQ(StreamEnds::nameFor(ends.find("signin", 20), true), QStringLiteral("signed_in"));
        // Not back on the screen: closed, or another account signed in.
        CHECK_EQ(StreamEnds::nameFor(ends.find("signin", 20), false), QStringLiteral("crashed"));
        // Any other stream is what it was, whatever the session does.
        CHECK_EQ(StreamEnds::nameFor(ends.find("plain", 20), true), QStringLiteral("crashed"));
        // The worker that ended its session itself, as it does when the
        // sign-in screen's session gets a user: the same answer.
        ends.begin("self", "native", 0, "dev", "tok", 30, true);
        ends.end("self", State::Ended, 40);
        CHECK_EQ(StreamEnds::nameFor(ends.find("self", 50), true), QStringLiteral("signed_in"));
        // Stop pressed, or a take-over: never read as a sign-in.
        ends.begin("stop", "native", 0, "dev", "tok", 60, true);
        ends.end("stop", State::Stopped, 70);
        CHECK_EQ(StreamEnds::nameFor(ends.find("stop", 80), true), QStringLiteral("stopped"));
        CHECK_EQ(StreamEnds::nameFor(StreamEnds::Entry{}, true), QStringLiteral("unknown"));
    }
}
